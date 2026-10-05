// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_SETTINGS_H
#define MAGSAFE_SETTINGS_H

#include "apple-smc.h"
#include <stdbool.h>
#include <stddef.h>

/* Persistent settings: the brightness and color mode that the light returns
 * to after every command, and that the daemon applies again at boot, on
 * plug-in, and on wake. Stored as "key=value" lines in a root-owned file. */

#define SETTINGS_DIR "/Library/Application Support/magsafe"
#define SETTINGS_PATH SETTINGS_DIR "/settings"
#define SETTINGS_TEXT_MAX 256u

typedef struct {
  unsigned dim;     /* brightness of both colors, 0-100 */
  SmcLedMode color; /* the Mac's color mode */
} Settings;

#define SETTINGS_DEFAULTS ((Settings){.dim = 100, .color = SMC_LED_AUTO})

/* The settings file. A process that is not root may name another one with
 * MAGSAFE_SETTINGS, for tests. */
const char *settings_path(void);

/* Parse the file's text. Unknown keys are ignored, for newer versions. */
int settings_parse(const char *text, Settings *settings, char *error, size_t size);
void settings_format(const Settings *settings, char *text, size_t size);
bool settings_are_default(const Settings *settings);
const char *settings_color_name(SmcLedMode color);
int settings_color(const char *name, SmcLedMode *color);

/* A missing file means the defaults. */
int settings_load(Settings *settings, char *error, size_t size);
/* Replace the file in one step. Requires root, except with MAGSAFE_SETTINGS. */
int settings_save(const Settings *settings, char *error, size_t size);

#endif
