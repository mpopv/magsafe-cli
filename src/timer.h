// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_TIMER_H
#define MAGSAFE_TIMER_H

#include <stddef.h>

/* The timer's schedule: green dimming from full brightness as time runs out,
 * amber for the warning period at the end, then an alarm. */

#define TIMER_MIN_MS 10000ul
#define TIMER_MAX_MS 86400000ul       /* 24 hours */
#define TIMER_WARNING_MAX_MS 300000ul /* the warning lasts 5 minutes, */
#define TIMER_WARNING_SHARE 5u        /* or a fifth of a shorter timer */

/* Parse a duration such as 25m, 90s, 1h30m, or 45, which means minutes.
 * Units come in the order h, m, s, each at most once. Fails on any other
 * text or on a duration over TIMER_MAX_MS, but not on a short one. */
int timer_parse(const char *text, unsigned long *milliseconds);

/* Write milliseconds, rounded up to whole seconds, as h:mm:ss or m:ss. */
void timer_format(unsigned long milliseconds, char *text, size_t size);

unsigned long timer_warning_ms(unsigned long total_ms);

/* The brightness percentage at elapsed_ms: full at the start, and dimming at
 * an even perceived rate to about a third at the end. */
unsigned timer_percent(unsigned long elapsed_ms, unsigned long total_ms);

#endif
