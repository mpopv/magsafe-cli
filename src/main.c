// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "apple-smc.h"
#include "error.h"
#include "firmware.h"
#include "led.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERSION "0.6.0"
#define LOCK_PATH "/var/run/magsafe-cli.lock"
#define MAX_COUNT 300u
#define MAX_TOTAL_MS 60000u
#define COUNT(array) (sizeof(array) / sizeof(*(array)))

static const char help_text[] =
    "magsafe " VERSION " - control the light on an Apple MagSafe 3 cable\n"
    "\n"
    "Usage: magsafe [options] <command> [<args>]\n"
    "\n"
    "Commands:\n"
    "  status                        Show firmware, security flags, and light state\n"
    "  reset                         Restore 100% brightness and macOS color control\n"
    "  led set <mode>                Set the light to auto, off, green, or amber\n"
    "  led brightness <color> <pct>  Light green or amber at 0-100% brightness\n"
    "  led blink <color>             Blink green, amber, or alternate\n"
    "  led fade-in <color>           Ramp up, then switch off\n"
    "  led fade-out <color>          Switch on, then ramp down\n"
    "  led fade <color>              Ramp up, then ramp down\n"
    "  led get                       Show the light mode and PWM output\n"
    "  firmware version              Show the cable firmware version\n"
    "  firmware security             Show the setter lock and signature-skip flags\n"
    "  firmware calibration          Show the four stored light calibration values\n"
    "  capabilities                  List supported and unavailable functions\n"
    "  version                       Show the magsafe version\n"
    "  help                          Show this help\n"
    "\n"
    "Options:\n"
    "      --json                    Print results and errors as one JSON object\n"
    "  -n, --dry-run                 Print the plan without touching hardware\n"
    "  -h, --help                    Show this help\n"
    "      --version                 Show the magsafe version\n"
    "\n"
    "Blink and fade options:\n"
    "  -c, --count <n>               Cycles, 1-300 or infinite (default infinite)\n"
    "  -i, --interval-ms <ms>        Blink on/off time, or dark time after each fade\n"
    "                                cycle, 100-10000 (default 500)\n"
    "  -d, --duration-ms <ms>        Fade ramp time, 500-60000 (default 1000)\n"
    "\n"
    "Fades take green or amber. Effects run until Ctrl-C unless --count is given;\n"
    "finite runs must fit in 60 seconds. Every effect finishes with a reset.\n"
    "Diagnostics, brightness, and effects require cable firmware " FW_SUPPORTED_VERSION_TEXT ".\n"
    "Hardware commands run through sudo, which may ask for your password.\n";

typedef enum {
  CMD_HELP,
  CMD_VERSION,
  CMD_CAPABILITIES,
  CMD_STATUS,
  CMD_RESET,
  CMD_FIRMWARE_VERSION,
  CMD_FIRMWARE_SECURITY,
  CMD_FIRMWARE_CALIBRATION,
  CMD_LED_GET,
  CMD_LED_SET,
  CMD_LED_BRIGHTNESS,
  CMD_LED_BLINK,
  CMD_LED_FADE_IN,
  CMD_LED_FADE_OUT,
  CMD_LED_FADE,
} Command;

static const struct {
  const char *name;
  const char *usage;
  int arguments;
} commands[] = {
    [CMD_HELP] = {"help", "", 0},
    [CMD_VERSION] = {"version", "", 0},
    [CMD_CAPABILITIES] = {"capabilities", "", 0},
    [CMD_STATUS] = {"status", "", 0},
    [CMD_RESET] = {"reset", "", 0},
    [CMD_FIRMWARE_VERSION] = {"firmware version", "", 0},
    [CMD_FIRMWARE_SECURITY] = {"firmware security", "", 0},
    [CMD_FIRMWARE_CALIBRATION] = {"firmware calibration", "", 0},
    [CMD_LED_GET] = {"led get", "", 0},
    [CMD_LED_SET] = {"led set", " <mode>", 1},
    [CMD_LED_BRIGHTNESS] = {"led brightness", " <color> <percent>", 2},
    [CMD_LED_BLINK] = {"led blink", " <color> [-c <n>] [-i <ms>]", 1},
    [CMD_LED_FADE_IN] = {"led fade-in", " <color> [-c <n>] [-d <ms>] [-i <ms>]", 1},
    [CMD_LED_FADE_OUT] = {"led fade-out", " <color> [-c <n>] [-d <ms>] [-i <ms>]", 1},
    [CMD_LED_FADE] = {"led fade", " <color> [-c <n>] [-d <ms>] [-i <ms>]", 1},
};

static const char *const mode_names[] = {[SMC_LED_AUTO] = "auto",
                                         [SMC_LED_OFF] = "off",
                                         [SMC_LED_GREEN] = "green",
                                         [SMC_LED_AMBER] = "amber"};
static const char *const selector_names[] = {"off", "amber", "green"};

typedef struct {
  Command command;
  bool json, dry_run;
  const char *argument; /* mode or color name, as given */
  SmcLedMode mode;      /* led set */
  unsigned percent;     /* led brightness */
  FwColor color;        /* led brightness, blink, and fades */
  bool alternate;       /* led blink alternate */
  unsigned count, interval_ms, duration_ms;
} Options;

static bool is_fade(Command command) {
  return command == CMD_LED_FADE_IN || command == CMD_LED_FADE_OUT || command == CMD_LED_FADE;
}

static bool is_effect(Command command) { return command == CMD_LED_BLINK || is_fade(command); }

/* Dark preparation time. Alternate blink prepares before every flash. */
static unsigned long preparation_ms(const Options *o) {
  return (o->alternate ? o->count : 1ul) * LED_PREPARE_MS;
}

/* Planned time of a finite effect, including preparation. */
static unsigned long total_ms(const Options *o) {
  unsigned long ramps = o->command == CMD_LED_FADE ? 2 : 1;
  unsigned long cycle =
      o->command == CMD_LED_BLINK ? 2ul * o->interval_ms : ramps * o->duration_ms + o->interval_ms;
  return preparation_ms(o) + o->count * cycle;
}

/* Output. JSON mode prints one object. Text mode prints aligned "key: value"
 * lines, so both modes share the same field names. */

static bool json_output;
static struct {
  const char *key;
  char value[256];
} lines[16];
static size_t line_count;

static void json_string(const char *text) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
    if (*p == '"' || *p == '\\') printf("\\%c", *p);
    else if (*p < 0x20) printf("\\u%04x", *p);
    else putchar(*p);
  }
  putchar('"');
}

/* Start a result. Text mode omits the command name. */
static void begin(const char *command) {
  line_count = 0;
  if (!json_output) return;
  printf("{\"ok\":true");
  if (command) {
    printf(",\"command\":");
    json_string(command);
  }
}

static void field(const char *key, const char *value, bool quoted) {
  if (json_output) {
    printf(",\"%s\":", key);
    if (quoted) json_string(value);
    else fputs(value, stdout);
  } else if (line_count < COUNT(lines)) {
    lines[line_count].key = key;
    snprintf(lines[line_count].value, sizeof(lines[0].value), "%s", value);
    line_count++;
  }
}

static void end(void) {
  if (json_output) {
    puts("}");
    return;
  }
  int width = 0;
  for (size_t i = 0; i < line_count; ++i) {
    int length = (int)strlen(lines[i].key);
    if (length > width) width = length;
  }
  for (size_t i = 0; i < line_count; ++i)
    printf("%s:%*s %s\n", lines[i].key, width - (int)strlen(lines[i].key), "", lines[i].value);
}

static void put_string(const char *key, const char *value) { field(key, value, true); }

static void put_number(const char *key, unsigned long value) {
  char text[24];
  snprintf(text, sizeof(text), "%lu", value);
  field(key, text, false);
}

static void put_bool(const char *key, bool value) { field(key, value ? "true" : "false", false); }

static void put_null(const char *key) { field(key, json_output ? "null" : "unbounded", false); }

static void put_hex(const char *key, uint32_t value) {
  char text[16];
  snprintf(text, sizeof(text), "0x%08x", value);
  field(key, text, true);
}

/* A number that text mode also names, such as "3 (green)". */
static void put_code(const char *key, unsigned value, const char *name) {
  char text[32];
  if (json_output || !name) snprintf(text, sizeof(text), "%u", value);
  else snprintf(text, sizeof(text), "%u (%s)", value, name);
  field(key, text, false);
}

/* A JSON array, or a comma-separated list in text mode. Items need no escaping. */
static void put_list(const char *key, const char *const *items, size_t count) {
  char text[256] = "";
  for (size_t i = 0; i < count; ++i) {
    size_t used = strlen(text);
    if (json_output)
      snprintf(text + used, sizeof(text) - used, "%s\"%s\"", i ? "," : "[", items[i]);
    else snprintf(text + used, sizeof(text) - used, "%s%s", i ? ", " : "", items[i]);
  }
  if (json_output) strlcat(text, "]", sizeof(text));
  field(key, text, false);
}

static void put_firmware(const FwClient *fw) {
  char version[16];
  fw_format_version(fw->version_word, version, sizeof(version));
  put_string("firmware", version);
  put_hex("version_word", fw->version_word);
}

static void put_security(uint32_t security) {
  put_hex("security_word", security);
  put_bool("configuration_setter_locked", security & FW_SECURITY_SETTER_LOCKED);
  put_bool("signature_skip_active", security & FW_SECURITY_SIGNATURE_SKIP);
}

static void put_led(const FwLedState *led) {
  put_number("pwm0", led->pwm0);
  put_number("pwm3", led->pwm3);
  put_code("color_selector", led->color, selector_names[led->color]);
}

static int report_error(const Options *o, const char *message, int code) {
  if (o->json) {
    printf("{\"ok\":false,\"error\":");
    json_string(message);
    puts("}");
  } else {
    fprintf(stderr, "magsafe: %s\n", message);
    if (code == 2) fputs("Try 'magsafe --help' for more information.\n", stderr);
  }
  return code;
}

/* Parsing */

/* Accept only plain decimal digits within [low, high]. */
static int parse_number(const char *text, unsigned low, unsigned high, unsigned *out) {
  char *end;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  if (*text < '0' || *text > '9' || *end || errno || value < low || value > high) return -1;
  *out = (unsigned)value;
  return 0;
}

static int parse_color(const char *name, FwColor *color) {
  if (!strcmp(name, "green")) *color = FW_GREEN;
  else if (!strcmp(name, "amber")) *color = FW_AMBER;
  else return -1;
  return 0;
}

static int parse_mode(const char *name, SmcLedMode *mode) {
  for (size_t i = 0; i < COUNT(mode_names); ++i) {
    if (mode_names[i] && !strcmp(name, mode_names[i])) {
      *mode = (SmcLedMode)i;
      return 0;
    }
  }
  return -1;
}

/* Match a one- or two-word command name. Return the number of words used. */
static int find_command(int count, char **words, Command *command, char *error, size_t size) {
  char pair[64] = "";
  if (count > 1) snprintf(pair, sizeof(pair), "%s %s", words[0], words[1]);
  size_t length = strlen(words[0]);
  bool group = false;
  for (size_t i = 0; i < COUNT(commands); ++i) {
    const char *name = commands[i].name;
    *command = (Command)i;
    if (!strcmp(name, words[0])) return 1;
    if (count > 1 && !strcmp(name, pair)) return 2;
    group |= !strncmp(name, words[0], length) && name[length] == ' ';
  }
  if (!group) return fail(error, size, "unknown command '%s'", words[0]);
  if (count == 1) return fail(error, size, "'%s' needs a subcommand", words[0]);
  return fail(error, size, "unknown command '%s %s'", words[0], words[1]);
}

static int parse_effect(Options *o, const char *color, const char *count, const char *interval,
                        const char *duration, char *error, size_t size) {
  bool blink = o->command == CMD_LED_BLINK;
  o->alternate = blink && !strcmp(color, "alternate");
  if (o->alternate) o->color = FW_GREEN;
  else if (parse_color(color, &o->color))
    return fail(error, size, "invalid color '%s' (expected %s)", color,
                blink ? "green, amber, or alternate" : "green or amber");
  if (count && !strcmp(count, "infinite")) o->count = LED_INFINITE;
  else if (count && parse_number(count, 1, MAX_COUNT, &o->count))
    return fail(error, size, "invalid --count '%s' (expected 1-%u or infinite)", count, MAX_COUNT);
  if (interval && parse_number(interval, 100, 10000, &o->interval_ms))
    return fail(error, size, "invalid --interval-ms '%s' (expected 100-10000)", interval);
  if (duration && parse_number(duration, 500, 60000, &o->duration_ms))
    return fail(error, size, "invalid --duration-ms '%s' (expected 500-60000)", duration);
  if (o->count != LED_INFINITE && total_ms(o) > MAX_TOTAL_MS)
    return fail(error, size,
                "planned run time is %lu ms; finite effects are limited to %u ms "
                "(omit --count to run until stopped)",
                total_ms(o), MAX_TOTAL_MS);
  return 0;
}

static void set_once(const char **value, const char *flag, char *error, size_t size) {
  if (*value && !*error) fail(error, size, "%s given more than once", flag);
  *value = optarg;
}

static int parse(int argc, char **argv, Options *o, char *error, size_t size) {
  enum { OPTION_JSON = 256, OPTION_VERSION };
  static const struct option options[] = {
      {"help", no_argument, NULL, 'h'},
      {"version", no_argument, NULL, OPTION_VERSION},
      {"json", no_argument, NULL, OPTION_JSON},
      {"dry-run", no_argument, NULL, 'n'},
      {"count", required_argument, NULL, 'c'},
      {"interval-ms", required_argument, NULL, 'i'},
      {"duration-ms", required_argument, NULL, 'd'},
      {NULL, 0, NULL, 0},
  };
  *o = (Options){
      .command = CMD_HELP, .count = LED_INFINITE, .interval_ms = 500, .duration_ms = 1000};
  const char *count = NULL, *interval = NULL, *duration = NULL;
  bool help = false, version = false;

  /* Options may appear anywhere. Keep scanning after an error so that a
   * later --json still applies to the error message. */
  opterr = 0;
  for (int option; (option = getopt_long(argc, argv, ":hnc:i:d:", options, NULL)) != -1;) {
    switch (option) {
      case 'h': help = true; break;
      case OPTION_VERSION: version = true; break;
      case OPTION_JSON: o->json = true; break;
      case 'n': o->dry_run = true; break;
      case 'c': set_once(&count, "--count", error, size); break;
      case 'i': set_once(&interval, "--interval-ms", error, size); break;
      case 'd': set_once(&duration, "--duration-ms", error, size); break;
      case ':':
        if (!*error) fail(error, size, "option '%s' needs a value", argv[optind - 1]);
        break;
      default:
        if (!*error) fail(error, size, "invalid option '%s'", argv[optind - 1]);
        break;
    }
  }
  if (help || version) {
    o->command = help ? CMD_HELP : CMD_VERSION;
    return 0;
  }
  if (*error) return -1;
  int words = argc - optind;
  if (words == 0) return 0;

  int used = find_command(words, argv + optind, &o->command, error, size);
  if (used < 0) return -1;
  char **args = argv + optind + used;
  const char *name = commands[o->command].name;
  if (words - used != commands[o->command].arguments)
    return fail(error, size, "usage: magsafe %s%s", name, commands[o->command].usage);
  if (!is_effect(o->command) && (count || interval || duration))
    return fail(error, size, "blink and fade options do not apply to '%s'", name);
  if (o->command == CMD_LED_BLINK && duration)
    return fail(error, size, "--duration-ms applies only to fade commands");

  if (commands[o->command].arguments) o->argument = args[0];
  switch (o->command) {
    case CMD_LED_SET:
      if (parse_mode(args[0], &o->mode))
        return fail(error, size, "invalid mode '%s' (expected auto, off, green, or amber)",
                    args[0]);
      return 0;
    case CMD_LED_BRIGHTNESS:
      if (parse_color(args[0], &o->color))
        return fail(error, size, "invalid color '%s' (expected green or amber)", args[0]);
      if (parse_number(args[1], 0, 100, &o->percent))
        return fail(error, size, "invalid percent '%s' (expected 0-100)", args[1]);
      return 0;
    case CMD_LED_BLINK:
    case CMD_LED_FADE_IN:
    case CMD_LED_FADE_OUT:
    case CMD_LED_FADE: return parse_effect(o, args[0], count, interval, duration, error, size);
    default: return 0;
  }
}

/* Commands that need no hardware */

static void capabilities(void) {
  static const char *const supported[] = {"version",
                                          "security-state",
                                          "led-state",
                                          "calibration-read",
                                          "green",
                                          "amber",
                                          "off",
                                          "system-control",
                                          "blink",
                                          "fade-in",
                                          "fade-out",
                                          "fade",
                                          "volatile-brightness",
                                          "reset"};
  static const char *const unavailable[] = {
      "firmware-flash", "security-write",     "calibration-write", "raw-memory",
      "raw-pwm",        "arbitrary-patterns", "rgb-colors",        "factory-reset"};
  begin(NULL);
  put_string("cli_version", VERSION);
  put_string("diagnostic_firmware", FW_SUPPORTED_VERSION_TEXT);
  put_list("supported", supported, COUNT(supported));
  put_list("unavailable", unavailable, COUNT(unavailable));
  end();
}

static void dry_run(const Options *o) {
  begin(commands[o->command].name);
  put_bool("dry_run", true);
  put_number("device_calls", 0);
  if (o->argument) put_string("color", o->argument);
  if (o->command == CMD_LED_BRIGHTNESS) put_number("percent", o->percent);
  if (is_effect(o->command)) {
    bool infinite = o->count == LED_INFINITE;
    if (infinite) put_string("count", "infinite");
    else put_number("count", o->count);
    put_number("interval_ms", o->interval_ms);
    if (is_fade(o->command)) put_number("fade_ms", o->duration_ms);
    if (infinite && o->alternate) put_null("preparation_ms");
    else put_number("preparation_ms", preparation_ms(o));
    if (infinite) put_null("duration_ms");
    else put_number("duration_ms", total_ms(o));
  }
  end();
}

/* Privilege and locking */

/* Re-run this command through sudo unless it already runs as root. Returns 0
 * when no elevation is needed; otherwise returns only on failure. */
static int elevate(int argc, char **argv, char *error, size_t size) {
  if (geteuid() == 0) return 0;
  char path[PATH_MAX], resolved[PATH_MAX];
  uint32_t length = sizeof(path);
  if (_NSGetExecutablePath(path, &length) || !realpath(path, resolved))
    return fail(error, size, "cannot locate the magsafe executable");
  char **args = calloc((size_t)argc + 3, sizeof(*args));
  if (!args) return fail(error, size, "out of memory");
  args[0] = "/usr/bin/sudo";
  args[1] = "--";
  args[2] = resolved;
  memcpy(args + 3, argv + 1, (size_t)(argc - 1) * sizeof(*args));
  execv(args[0], args);
  int saved = errno;
  free(args);
  return fail(error, size, "cannot run /usr/bin/sudo: %s", strerror(saved));
}

/* Allow one hardware command at a time. The lock is released on exit. */
static int acquire_lock(char *error, size_t size) {
  int fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return fail(error, size, "cannot open %s: %s", LOCK_PATH, strerror(errno));
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1) {
    close(fd);
    return fail(error, size, "unexpected lock file %s", LOCK_PATH);
  }
  if (flock(fd, LOCK_EX | LOCK_NB)) {
    close(fd);
    return fail(error, size, "another magsafe command is running");
  }
  return fd;
}

/* Hardware commands. Reads print their results; changes print only in JSON
 * mode, and are otherwise silent on success. */

static int execute(const Options *o, FwClient *fw, SmcClient *smc, char *error, size_t size) {
  const char *name = commands[o->command].name;
  FwLedState led = {0};
  uint32_t security = 0;
  uint16_t calibration[4] = {0};
  uint8_t control = 0;

  switch (o->command) {
    case CMD_FIRMWARE_VERSION:
    case CMD_FIRMWARE_SECURITY:
    case CMD_FIRMWARE_CALIBRATION:
      if (fw_open(fw, error, size) ||
          (o->command == CMD_FIRMWARE_SECURITY && fw_read_security(fw, &security, error, size)) ||
          (o->command == CMD_FIRMWARE_CALIBRATION &&
           fw_read_calibration(fw, calibration, error, size)))
        return -1;
      begin(name);
      put_firmware(fw);
      if (o->command == CMD_FIRMWARE_SECURITY) put_security(security);
      if (o->command == CMD_FIRMWARE_CALIBRATION) {
        char values[48];
        snprintf(values, sizeof(values), "[%u,%u,%u,%u]", calibration[0], calibration[1],
                 calibration[2], calibration[3]);
        field("calibration", values, false);
      }
      end();
      return 0;

    case CMD_STATUS:
    case CMD_LED_GET:
      if (fw_open(fw, error, size) || smc_open(smc, error, size) ||
          (o->command == CMD_STATUS && fw_read_security(fw, &security, error, size)) ||
          fw_read_led(fw, &led, error, size) || smc_read_led(smc, &control, error, size))
        return -1;
      begin(name);
      put_firmware(fw);
      if (o->command == CMD_STATUS) put_security(security);
      put_code("smc_control", control, control < COUNT(mode_names) ? mode_names[control] : NULL);
      put_led(&led);
      end();
      return 0;

    case CMD_LED_SET:
      if (smc_open(smc, error, size) || smc_set_led(smc, o->mode, error, size)) return -1;
      if (!json_output) return 0;
      begin(name);
      put_string("color", o->argument);
      end();
      return 0;

    case CMD_LED_BRIGHTNESS:
      if (fw_open(fw, error, size) || smc_open(smc, error, size) ||
          led_brightness(fw, smc, o->color, o->percent, &led, error, size))
        return -1;
      if (!json_output) return 0;
      begin(name);
      put_firmware(fw);
      put_led(&led);
      put_string("color", o->argument);
      put_number("percent", o->percent);
      put_bool("pwm_verified", true);
      end();
      return 0;

    case CMD_LED_BLINK:
    case CMD_LED_FADE_IN:
    case CMD_LED_FADE_OUT:
    case CMD_LED_FADE: {
      LedPattern patterns[] = {[CMD_LED_BLINK] = LED_BLINK,
                               [CMD_LED_FADE_IN] = LED_FADE_IN,
                               [CMD_LED_FADE_OUT] = LED_FADE_OUT,
                               [CMD_LED_FADE] = LED_FADE};
      LedEffect effect = {.pattern = patterns[o->command],
                          .color = o->color,
                          .alternate = o->alternate,
                          .count = o->count,
                          .interval_ms = o->interval_ms,
                          .duration_ms = o->duration_ms};
      if (fw_open(fw, error, size) || smc_open(smc, error, size) ||
          led_catch_signals(error, size) || led_run(fw, smc, &effect, error, size))
        return -1;
      if (!json_output) return 0;
      begin(name);
      put_firmware(fw);
      put_string("color", o->argument);
      if (o->command == CMD_LED_BLINK) {
        put_number("flashes", o->count);
      } else {
        put_number("fades", o->count);
        put_number("fade_ms", o->duration_ms);
        put_number("interval_ms", o->interval_ms);
        put_bool("pwm_verified", true);
      }
      put_number("brightness_percent", 100);
      put_bool("system_color_control_requested", true);
      end();
      return 0;
    }

    case CMD_RESET:
      /* Without the cable, reset still returns color control to macOS, and
       * led_reset reports the brightness steps as unavailable. */
      fw_open(fw, NULL, 0);
      smc_open(smc, NULL, 0);
      if (led_reset(fw, smc, error, size)) return -1;
      if (!json_output) return 0;
      begin(name);
      put_firmware(fw);
      put_number("brightness_percent", 100);
      put_bool("system_color_control_requested", true);
      end();
      return 0;

    default: return fail(error, size, "'%s' is not a hardware command", name);
  }
}

int main(int argc, char **argv) {
  Options o;
  char error[512] = "";
  if (parse(argc, argv, &o, error, sizeof(error))) return report_error(&o, error, 2);
  json_output = o.json;

  switch (o.command) {
    case CMD_HELP: fputs(help_text, stdout); return 0;
    case CMD_VERSION: puts("magsafe " VERSION); return 0;
    case CMD_CAPABILITIES: capabilities(); return 0;
    default: break;
  }
  if (o.dry_run) {
    dry_run(&o);
    return 0;
  }

  if (elevate(argc, argv, error, sizeof(error))) return report_error(&o, error, 1);
  int lock = acquire_lock(error, sizeof(error));
  if (lock < 0) return report_error(&o, error, 1);
  FwClient fw = {0};
  SmcClient smc = {0};
  int result = execute(&o, &fw, &smc, error, sizeof(error));
  fw_close(&fw);
  smc_close(&smc);
  close(lock);
  if (result) return report_error(&o, error, led_stop_signal ? 128 + led_stop_signal : 1);
  return 0;
}
