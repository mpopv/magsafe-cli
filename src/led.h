// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_LED_H
#define MAGSAFE_LED_H

#include "apple-smc.h"
#include "firmware.h"
#include <signal.h>
#include <stdbool.h>

/* Light behavior that combines the cable's RAM brightness scales with the
 * Mac's color control. The caller opens both clients. */

#define LED_PREPARE_MS 450u /* dark pause after each color selection */
#define LED_INFINITE 0u     /* effect count that repeats until stopped */

typedef enum { LED_BLINK, LED_FADE_IN, LED_FADE_OUT, LED_FADE } LedPattern;

/* The caller checks the ranges; see the help text in main.c. */
typedef struct {
  LedPattern pattern;
  FwColor color;        /* the first color when alternating */
  bool alternate;       /* blink only: switch colors after each flash */
  unsigned count;       /* cycles, or LED_INFINITE */
  unsigned interval_ms; /* blink: each on and off time; fade: dark time after each cycle */
  unsigned duration_ms; /* fade: time for each ramp */
} LedEffect;

/* Set by SIGINT, SIGTERM, or SIGHUP once led_catch_signals has run. A running
 * effect stops at its next step, and still resets the light. */
extern volatile sig_atomic_t led_stop_signal;
int led_catch_signals(char *error, size_t size);

/* Set color's brightness scale, select color, and wait for matching PWM output. */
int led_brightness(FwClient *fw, SmcClient *smc, FwColor color, unsigned percent, FwLedState *state,
                   char *error, size_t size);

/* Set both brightness scales to 100% and return color control to macOS. Every
 * step is attempted, even with a closed client. Failures are added to error. */
int led_reset(FwClient *fw, SmcClient *smc, char *error, size_t size);

/* Run a blink or fade, then led_reset. Brightness is checked at each fade peak
 * and trough. */
int led_run(FwClient *fw, SmcClient *smc, const LedEffect *effect, char *error, size_t size);

#endif
