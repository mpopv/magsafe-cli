// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "settings.h"
#include "error.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const color_names[] = {[SMC_LED_AUTO] = "auto",
                                          [SMC_LED_OFF] = "off",
                                          [SMC_LED_GREEN] = "green",
                                          [SMC_LED_AMBER] = "amber"};

const char *settings_path(void) {
  const char *path = getenv("MAGSAFE_SETTINGS");
  return path && *path && getuid() != 0 && geteuid() != 0 ? path : SETTINGS_PATH;
}

const char *settings_color_name(SmcLedMode color) {
  return (size_t)color < sizeof(color_names) / sizeof(*color_names) && color_names[color]
             ? color_names[color]
             : "auto";
}

int settings_color(const char *name, SmcLedMode *color) {
  for (size_t i = 0; i < sizeof(color_names) / sizeof(*color_names); ++i) {
    if (color_names[i] && !strcmp(name, color_names[i])) {
      *color = (SmcLedMode)i;
      return 0;
    }
  }
  return -1;
}

bool settings_are_default(const Settings *settings) {
  return settings->dim == 100 && settings->color == SMC_LED_AUTO;
}

int settings_parse(const char *text, Settings *settings, char *error, size_t size) {
  Settings parsed = SETTINGS_DEFAULTS;
  unsigned line = 0;
  for (const char *p = text; *p;) {
    const char *end = strchr(p, '\n');
    size_t length = end ? (size_t)(end - p) : strlen(p);
    char buffer[SETTINGS_TEXT_MAX];
    line++;
    if (length >= sizeof(buffer)) return fail(error, size, "settings line %u is too long", line);
    memcpy(buffer, p, length);
    buffer[length] = '\0';
    p += length + (end != NULL);
    if (!buffer[0] || buffer[0] == '#') continue;
    char *value = strchr(buffer, '=');
    if (!value) return fail(error, size, "settings line %u has no '='", line);
    *value++ = '\0';
    if (!strcmp(buffer, "dim")) {
      char *rest;
      errno = 0;
      unsigned long dim = strtoul(value, &rest, 10);
      if (*value < '0' || *value > '9' || *rest || errno || dim > 100)
        return fail(error, size, "invalid dim '%s' in settings (expected 0-100)", value);
      parsed.dim = (unsigned)dim;
    } else if (!strcmp(buffer, "color")) {
      if (settings_color(value, &parsed.color))
        return fail(error, size, "invalid color '%s' in settings", value);
    }
  }
  *settings = parsed;
  return 0;
}

void settings_format(const Settings *settings, char *text, size_t size) {
  snprintf(text, size, "# Written by magsafe. See 'magsafe settings'.\ndim=%u\ncolor=%s\n",
           settings->dim, settings_color_name(settings->color));
}

int settings_load(Settings *settings, char *error, size_t size) {
  *settings = SETTINGS_DEFAULTS;
  const char *path = settings_path();
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT) return 0;
    return fail(error, size, "cannot read %s: %s", path, strerror(errno));
  }
  char text[SETTINGS_TEXT_MAX * 4];
  ssize_t length = read(fd, text, sizeof(text) - 1);
  int saved = errno;
  close(fd);
  if (length < 0) return fail(error, size, "cannot read %s: %s", path, strerror(saved));
  if ((size_t)length == sizeof(text) - 1) return fail(error, size, "%s is too large", path);
  text[length] = '\0';
  return settings_parse(text, settings, error, size);
}

int settings_save(const Settings *settings, char *error, size_t size) {
  const char *path = settings_path();
  bool standard = !strcmp(path, SETTINGS_PATH);
  if (standard && mkdir(SETTINGS_DIR, 0755) && errno != EEXIST)
    return fail(error, size, "cannot create %s: %s", SETTINGS_DIR, strerror(errno));
  char temporary[1024], text[SETTINGS_TEXT_MAX];
  snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
  int fd = mkstemp(temporary);
  if (fd < 0) return fail(error, size, "cannot write %s: %s", path, strerror(errno));
  settings_format(settings, text, sizeof(text));
  size_t length = strlen(text);
  bool written =
      write(fd, text, length) == (ssize_t)length && fchmod(fd, 0644) == 0 && fsync(fd) == 0;
  int saved = errno;
  if (close(fd) || !written || rename(temporary, path)) {
    if (written) saved = errno;
    unlink(temporary);
    return fail(error, size, "cannot write %s: %s", path, strerror(saved));
  }
  return 0;
}
