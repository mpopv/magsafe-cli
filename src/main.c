// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "apple-smc.h"
#include "audio.h"
#include "error.h"
#include "firmware.h"
#include "led.h"
#include "timer.h"
#include "visualizer.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define VERSION "0.8.0"
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
    "  led stream <color>            Set green or amber brightness from lines on stdin\n"
    "  led get                       Show the light mode and PWM output\n"
    "  firmware version              Show the cable firmware version\n"
    "  firmware security             Show the setter lock and signature-skip flags\n"
    "  firmware calibration          Show the four stored light calibration values\n"
    "  visualizer [<color>]          Pulse green (default) or amber to the playing audio\n"
    "  timer <duration>              Count down on the light, then flash amber\n"
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
    "Blink, fade, and timer options:\n"
    "  -c, --count <n>               Cycles or alarm flashes, 1-300 or infinite\n"
    "                                (default infinite)\n"
    "  -i, --interval-ms <ms>        Blink or alarm on/off time, or dark time after\n"
    "                                each fade cycle, 100-10000 (default 500)\n"
    "  -d, --duration-ms <ms>        Fade ramp time, 500-60000 (default 1000)\n"
    "\n"
    "Visualizer options:\n"
    "      --preview                 Show the levels in the terminal, without the cable\n"
    "                                or sudo\n"
    "\n"
    "Fades take green or amber. Effects run until Ctrl-C unless --count is given;\n"
    "finite runs must fit in 60 seconds. Streams end with their input.\n"
    "A timer takes 10s to 24h, such as 25m, 90s, or 1h30m; a plain number is\n"
    "minutes. It dims green as time runs out and turns amber for the last fifth,\n"
    "at most 5 minutes. Its alarm flashes until --count or Ctrl-C, which exits 0.\n"
    "Every effect, stream, timer, and visualizer finishes with a reset. The\n"
    "visualizer needs System Audio Recording permission for your terminal app,\n"
    "and macOS 14.2 or later.\n"
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
  CMD_LED_STREAM,
  CMD_VISUALIZER,
  CMD_TIMER,
} Command;

static const struct {
  const char *name;
  const char *usage;
  int min_arguments, max_arguments;
} commands[] = {
    [CMD_HELP] = {"help", "", 0, 0},
    [CMD_VERSION] = {"version", "", 0, 0},
    [CMD_CAPABILITIES] = {"capabilities", "", 0, 0},
    [CMD_STATUS] = {"status", "", 0, 0},
    [CMD_RESET] = {"reset", "", 0, 0},
    [CMD_FIRMWARE_VERSION] = {"firmware version", "", 0, 0},
    [CMD_FIRMWARE_SECURITY] = {"firmware security", "", 0, 0},
    [CMD_FIRMWARE_CALIBRATION] = {"firmware calibration", "", 0, 0},
    [CMD_LED_GET] = {"led get", "", 0, 0},
    [CMD_LED_SET] = {"led set", " <mode>", 1, 1},
    [CMD_LED_BRIGHTNESS] = {"led brightness", " <color> <percent>", 2, 2},
    [CMD_LED_BLINK] = {"led blink", " <color> [-c <n>] [-i <ms>]", 1, 1},
    [CMD_LED_FADE_IN] = {"led fade-in", " <color> [-c <n>] [-d <ms>] [-i <ms>]", 1, 1},
    [CMD_LED_FADE_OUT] = {"led fade-out", " <color> [-c <n>] [-d <ms>] [-i <ms>]", 1, 1},
    [CMD_LED_FADE] = {"led fade", " <color> [-c <n>] [-d <ms>] [-i <ms>]", 1, 1},
    [CMD_LED_STREAM] = {"led stream", " <color>", 1, 1},
    [CMD_VISUALIZER] = {"visualizer", " [<color>] [--preview]", 0, 1},
    [CMD_TIMER] = {"timer", " <duration> [-c <n>] [-i <ms>]", 1, 1},
};

static const char *const mode_names[] = {[SMC_LED_AUTO] = "auto",
                                         [SMC_LED_OFF] = "off",
                                         [SMC_LED_GREEN] = "green",
                                         [SMC_LED_AMBER] = "amber"};
static const char *const selector_names[] = {"off", "amber", "green"};

typedef struct {
  Command command;
  bool json, dry_run;
  bool preview;         /* visualizer --preview */
  const char *argument; /* mode or color name, as given */
  SmcLedMode mode;      /* led set */
  unsigned percent;     /* led brightness */
  FwColor color;        /* led brightness, blink, fades, stream, and visualizer */
  bool alternate;       /* led blink alternate */
  unsigned long timer_ms;
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

/* The --count, --interval-ms, and --duration-ms values, where given. */
static int parse_repeat(Options *o, const char *count, const char *interval, const char *duration,
                        char *error, size_t size) {
  if (count && !strcmp(count, "infinite")) o->count = LED_INFINITE;
  else if (count && parse_number(count, 1, MAX_COUNT, &o->count))
    return fail(error, size, "invalid --count '%s' (expected 1-%u or infinite)", count, MAX_COUNT);
  if (interval && parse_number(interval, 100, 10000, &o->interval_ms))
    return fail(error, size, "invalid --interval-ms '%s' (expected 100-10000)", interval);
  if (duration && parse_number(duration, 500, 60000, &o->duration_ms))
    return fail(error, size, "invalid --duration-ms '%s' (expected 500-60000)", duration);
  return 0;
}

static int parse_effect(Options *o, const char *color, const char *count, const char *interval,
                        const char *duration, char *error, size_t size) {
  bool blink = o->command == CMD_LED_BLINK;
  o->alternate = blink && !strcmp(color, "alternate");
  if (o->alternate) o->color = FW_GREEN;
  else if (parse_color(color, &o->color))
    return fail(error, size, "invalid color '%s' (expected %s)", color,
                blink ? "green, amber, or alternate" : "green or amber");
  if (parse_repeat(o, count, interval, duration, error, size)) return -1;
  if (o->count != LED_INFINITE && total_ms(o) > MAX_TOTAL_MS)
    return fail(error, size,
                "planned run time is %lu ms; finite effects are limited to %u ms "
                "(omit --count to run until stopped)",
                total_ms(o), MAX_TOTAL_MS);
  return 0;
}

/* The timer's alarm time, if it is finite. */
static unsigned long alarm_ms(const Options *o) { return o->count * 2ul * o->interval_ms; }

static int parse_timer(Options *o, const char *text, const char *count, const char *interval,
                       char *error, size_t size) {
  if (timer_parse(text, &o->timer_ms) || o->timer_ms < TIMER_MIN_MS)
    return fail(error, size, "invalid duration '%s' (expected 10s-24h, such as 25m, 90s, or 1h30m)",
                text);
  if (parse_repeat(o, count, interval, NULL, error, size)) return -1;
  if (o->count != LED_INFINITE && alarm_ms(o) > MAX_TOTAL_MS)
    return fail(error, size,
                "planned alarm time is %lu ms; finite alarms are limited to %u ms "
                "(omit --count to flash until stopped)",
                alarm_ms(o), MAX_TOTAL_MS);
  return 0;
}

static void set_once(const char **value, const char *flag, char *error, size_t size) {
  if (*value && !*error) fail(error, size, "%s given more than once", flag);
  *value = optarg;
}

static int parse(int argc, char **argv, Options *o, char *error, size_t size) {
  enum { OPTION_JSON = 256, OPTION_VERSION, OPTION_PREVIEW };
  static const struct option options[] = {
      {"help", no_argument, NULL, 'h'},
      {"version", no_argument, NULL, OPTION_VERSION},
      {"json", no_argument, NULL, OPTION_JSON},
      {"dry-run", no_argument, NULL, 'n'},
      {"count", required_argument, NULL, 'c'},
      {"interval-ms", required_argument, NULL, 'i'},
      {"duration-ms", required_argument, NULL, 'd'},
      {"preview", no_argument, NULL, OPTION_PREVIEW},
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
      case OPTION_PREVIEW: o->preview = true; break;
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
  int given = words - used;
  if (given < commands[o->command].min_arguments || given > commands[o->command].max_arguments)
    return fail(error, size, "usage: magsafe %s%s", name, commands[o->command].usage);
  bool timer = o->command == CMD_TIMER;
  if (!is_effect(o->command) && !timer && (count || interval || duration))
    return fail(error, size, "blink, fade, and timer options do not apply to '%s'", name);
  if ((o->command == CMD_LED_BLINK || timer) && duration)
    return fail(error, size, "--duration-ms applies only to fade commands");
  if (o->preview && o->command != CMD_VISUALIZER)
    return fail(error, size, "--preview applies only to 'visualizer'");

  if (given) o->argument = args[0];
  switch (o->command) {
    case CMD_LED_SET:
      if (parse_mode(args[0], &o->mode))
        return fail(error, size, "invalid mode '%s' (expected auto, off, green, or amber)",
                    args[0]);
      return 0;
    case CMD_VISUALIZER:
      if (!given) o->argument = "green";
      /* fall through */
    case CMD_LED_STREAM:
      if (parse_color(o->argument, &o->color))
        return fail(error, size, "invalid color '%s' (expected green or amber)", o->argument);
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
    case CMD_TIMER: return parse_timer(o, args[0], count, interval, error, size);
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
                                          "brightness-stream",
                                          "visualizer",
                                          "timer",
                                          "volatile-brightness",
                                          "reset"};
  static const char *const unavailable[] = {"firmware-flash", "security-write", "calibration-write",
                                            "raw-memory",     "raw-pwm",        "rgb-colors",
                                            "factory-reset"};
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
  if (o->argument && o->command != CMD_TIMER) put_string("color", o->argument);
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
  if (o->command == CMD_TIMER) {
    bool infinite = o->count == LED_INFINITE;
    put_number("timer_ms", o->timer_ms);
    put_number("warning_ms", timer_warning_ms(o->timer_ms));
    if (infinite) put_string("count", "infinite");
    else put_number("count", o->count);
    put_number("interval_ms", o->interval_ms);
    put_number("preparation_ms", LED_PREPARE_MS);
    if (infinite) put_null("duration_ms");
    else put_number("duration_ms", o->timer_ms + alarm_ms(o));
  }
  if (o->command == CMD_LED_STREAM) put_number("max_rate_hz", LED_STREAM_HZ);
  if (o->command == CMD_VISUALIZER) {
    put_bool("preview", o->preview);
    put_number("frame_rate_hz", VISUALIZER_HZ);
  }
  if (o->command == CMD_LED_STREAM || (o->command == CMD_VISUALIZER && !o->preview)) {
    put_number("preparation_ms", LED_PREPARE_MS);
    put_null("duration_ms");
  }
  end();
}

/* Privilege and locking */

/* The absolute path of this program, to run it again. */
static int executable_path(char resolved[PATH_MAX], char *error, size_t size) {
  char path[PATH_MAX];
  uint32_t length = sizeof(path);
  if (_NSGetExecutablePath(path, &length) || !realpath(path, resolved))
    return fail(error, size, "cannot locate the magsafe executable");
  return 0;
}

/* Re-run this command through sudo unless it already runs as root. Returns 0
 * when no elevation is needed; otherwise returns only on failure. */
static int elevate(int argc, char **argv, char *error, size_t size) {
  if (geteuid() == 0) return 0;
  char resolved[PATH_MAX];
  if (executable_path(resolved, error, size)) return -1;
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

static bool invalid_input; /* led stream stopped at an invalid line */

typedef struct {
  bool infinite, drawn;
} Countdown;

/* Show the time left on a terminal, then the alarm. */
static void show_timer(void *context, unsigned long remaining_ms, bool alarm) {
  Countdown *countdown = context;
  char left[16];
  timer_format(remaining_ms, left, sizeof(left));
  if (alarm) fprintf(stderr, "\r\033[Ktime's up%s", countdown->infinite ? " (Ctrl-C to stop)" : "");
  else fprintf(stderr, "\r\033[K%s left", left);
  countdown->drawn = true;
}

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

    case CMD_LED_STREAM: {
      LedStreamResult stream = {0};
      if (fw_open(fw, error, size) || smc_open(smc, error, size) ||
          led_catch_signals(error, size) ||
          led_stream(fw, smc, o->color, STDIN_FILENO, &stream, error, size)) {
        invalid_input = stream.invalid_input;
        return -1;
      }
      if (!json_output) return 0;
      begin(name);
      put_firmware(fw);
      put_string("color", o->argument);
      put_number("values", stream.values);
      put_number("writes", stream.writes);
      put_number("brightness_percent", 100);
      put_bool("system_color_control_requested", true);
      end();
      return 0;
    }

    case CMD_TIMER: {
      Countdown countdown = {.infinite = o->count == LED_INFINITE};
      LedTimer timer = {.duration_ms = o->timer_ms,
                        .count = o->count,
                        .interval_ms = o->interval_ms,
                        .context = &countdown};
      if (!json_output && isatty(STDERR_FILENO)) timer.show = show_timer;
      unsigned long flashes = 0;
      int result = fw_open(fw, error, size) || smc_open(smc, error, size) ||
                   led_catch_signals(error, size) ||
                   led_timer(fw, smc, &timer, &flashes, error, size);
      if (countdown.drawn) fputc('\n', stderr);
      if (result) return -1;
      if (!json_output) return 0;
      begin(name);
      put_firmware(fw);
      put_number("timer_ms", o->timer_ms);
      put_number("flashes", flashes);
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

/* Visualizer. This process captures and analyzes audio without privileges
 * and writes brightness values to 'magsafe led stream', run through sudo.
 * Only this process prints the result: it collects the helper's standard
 * output and passes it on, so that --json still prints one object. */

extern char **environ;

typedef struct {
  pid_t pid;
  int input; /* the helper's standard input, nonblocking */
  int status;
  bool reaped;
} Helper;

/* Start 'magsafe [--json] led stream <color>' through sudo, unless this
 * process is already root, with pipes for its standard input and output. */
static int start_helper(const Options *o, Helper *helper, int *output, char *error, size_t size) {
  char path[PATH_MAX];
  if (executable_path(path, error, size)) return -1;
  int to[2], from[2];
  if (pipe(to)) return fail(error, size, "cannot create a pipe: %s", strerror(errno));
  if (pipe(from)) {
    int saved = errno;
    close(to[0]);
    close(to[1]);
    return fail(error, size, "cannot create a pipe: %s", strerror(saved));
  }
  for (int i = 0; i < 2; ++i) {
    fcntl(to[i], F_SETFD, FD_CLOEXEC);
    fcntl(from[i], F_SETFD, FD_CLOEXEC);
  }
  fcntl(to[1], F_SETFL, O_NONBLOCK);

  char *args[8];
  int count = 0;
  if (geteuid() != 0) {
    args[count++] = "/usr/bin/sudo";
    args[count++] = "--";
  }
  args[count++] = path;
  if (o->json) args[count++] = "--json";
  args[count++] = "led";
  args[count++] = "stream";
  args[count++] = (char *)o->argument;
  args[count] = NULL;

  /* SIGPIPE is ignored here, but the helper keeps the default. */
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, to[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, from[1], STDOUT_FILENO);
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);
  int result = posix_spawn(&helper->pid, args[0], &actions, &attributes, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(to[0]);
  close(from[1]);
  if (result) {
    close(to[1]);
    close(from[0]);
    return fail(error, size, "cannot run %s: %s", args[0], strerror(result));
  }
  helper->input = to[1];
  *output = from[0];
  return 0;
}

/* Send one brightness to the helper. A full pipe drops the frame, since the
 * helper uses only the newest value anyway. */
static int send_level(void *context, unsigned percent, bool hint, char *error, size_t size) {
  Helper *helper = context;
  if (hint) fprintf(stderr, "%s\n", VISUALIZER_HINT);
  if (waitpid(helper->pid, &helper->status, WNOHANG) == helper->pid) {
    helper->reaped = true;
    return fail(error, size, "the light helper stopped");
  }
  char line[8];
  int length = snprintf(line, sizeof(line), "%u\n", percent);
  if (write(helper->input, line, (size_t)length) >= 0 || errno == EAGAIN || errno == EINTR)
    return 0;
  if (errno == EPIPE) return fail(error, size, "the light helper stopped");
  return fail(error, size, "cannot send brightness: %s", strerror(errno));
}

typedef struct {
  bool terminal, drawn;
} Meter;

/* Draw the brightness as a bar on a terminal, or print it as a line. */
static int show_level(void *context, unsigned percent, bool hint, char *error, size_t size) {
  (void)error, (void)size;
  Meter *meter = context;
  if (hint) fprintf(stderr, meter->terminal ? "\r\033[K%s\n" : "%s\n", VISUALIZER_HINT);
  if (!meter->terminal) {
    fprintf(stderr, "%u\n", percent);
    return 0;
  }
  char bar[41];
  unsigned filled = (percent * 40 + 50) / 100;
  memset(bar, '#', filled);
  memset(bar + filled, ' ', 40 - filled);
  bar[40] = '\0';
  fprintf(stderr, "\r%3u%% [%s]", percent, bar);
  meter->drawn = true;
  return 0;
}

/* Run the visualizer until it stops, and return the exit code. */
static int visualize(const Options *o, char *error, size_t size) {
  /* Check audio permission before sudo asks for a password. */
  if (led_catch_signals(error, size) || audio_check_permission(&led_stop_signal, error, size))
    return report_error(o, error, led_stop_signal ? 128 + led_stop_signal : 1);
  if (o->preview) {
    Meter meter = {.terminal = isatty(STDERR_FILENO)};
    visualizer_run(show_level, &meter, error, size);
    if (meter.drawn) fputc('\n', stderr);
    return report_error(o, error, led_stop_signal ? 128 + led_stop_signal : 1);
  }

  Helper helper = {0};
  int output;
  if (start_helper(o, &helper, &output, error, size)) return report_error(o, error, 1);
  signal(SIGPIPE, SIG_IGN);
  visualizer_run(send_level, &helper, error, size);

  /* The helper resets the light and exits when its input ends. */
  close(helper.input);
  char captured[2048], discard[512];
  size_t length = 0;
  for (ssize_t count = 1; count != 0;) {
    bool room = length < sizeof(captured);
    count = room ? read(output, captured + length, sizeof(captured) - length)
                 : read(output, discard, sizeof(discard));
    if (count < 0 && errno != EINTR) break;
    if (count > 0 && room) length += (size_t)count;
  }
  close(output);
  while (!helper.reaped && waitpid(helper.pid, &helper.status, 0) < 0) {
    if (errno != EINTR) {
      helper.status = 1 << 8; /* exit status 1 */
      break;
    }
  }
  int code = WIFEXITED(helper.status)     ? WEXITSTATUS(helper.status)
             : WIFSIGNALED(helper.status) ? 128 + WTERMSIG(helper.status)
                                          : 1;

  /* A failed helper, or sudo, has reported its own error. */
  if (code && !o->json) return code;
  if (code && length) {
    fwrite(captured, 1, length, stdout);
    return code;
  }
  if (led_stop_signal) return report_error(o, error, 128 + led_stop_signal);
  if (code) {
    fail(error, size, "sudo or 'magsafe led stream' exited with status %d", code);
    return report_error(o, error, code);
  }
  return report_error(o, error, 1);
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
  if (o.command == CMD_VISUALIZER) return visualize(&o, error, sizeof(error));

  if (elevate(argc, argv, error, sizeof(error))) return report_error(&o, error, 1);
  int lock = acquire_lock(error, sizeof(error));
  if (lock < 0) return report_error(&o, error, 1);
  FwClient fw = {0};
  SmcClient smc = {0};
  int result = execute(&o, &fw, &smc, error, sizeof(error));
  fw_close(&fw);
  smc_close(&smc);
  close(lock);
  if (result)
    return report_error(&o, error, led_stop_signal ? 128 + led_stop_signal : invalid_input ? 2 : 1);
  return 0;
}
