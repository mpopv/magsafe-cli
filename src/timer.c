// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "timer.h"
#include <math.h>
#include <stdio.h>

#define FLOOR 0.3 /* perceived brightness at the end */
#define GAMMA 2.2 /* perceived brightness is not linear in PWM */

int timer_parse(const char *text, unsigned long *milliseconds) {
  static const struct {
    char unit;
    unsigned long milliseconds;
  } units[] = {{'h', 3600000ul}, {'m', 60000ul}, {'s', 1000ul}};
  unsigned long total = 0;
  size_t next = 0; /* the first unit that may still come */
  const char *p = text;
  if (*p < '0' || *p > '9') return -1;
  while (*p) {
    unsigned long value = 0;
    if (*p < '0' || *p > '9') return -1;
    for (; *p >= '0' && *p <= '9'; ++p) {
      value = value * 10 + (unsigned long)(*p - '0');
      if (value > TIMER_MAX_MS) return -1;
    }
    /* A plain number means minutes. */
    if (!*p && p != text && next == 0 && total == 0) {
      total = value * 60000ul;
      break;
    }
    size_t u = next;
    while (u < sizeof(units) / sizeof(*units) && units[u].unit != *p) ++u;
    if (u == sizeof(units) / sizeof(*units)) return -1;
    total += value * units[u].milliseconds;
    if (total > TIMER_MAX_MS) return -1;
    next = u + 1;
    ++p;
  }
  if (total > TIMER_MAX_MS) return -1;
  *milliseconds = total;
  return 0;
}

void timer_format(unsigned long milliseconds, char *text, size_t size) {
  unsigned long seconds = (milliseconds + 999) / 1000;
  if (seconds >= 3600)
    snprintf(text, size, "%lu:%02lu:%02lu", seconds / 3600, seconds / 60 % 60, seconds % 60);
  else snprintf(text, size, "%lu:%02lu", seconds / 60, seconds % 60);
}

unsigned long timer_warning_ms(unsigned long total_ms) {
  unsigned long share = total_ms / TIMER_WARNING_SHARE;
  return share < TIMER_WARNING_MAX_MS ? share : TIMER_WARNING_MAX_MS;
}

unsigned timer_percent(unsigned long elapsed_ms, unsigned long total_ms) {
  double progress = total_ms && elapsed_ms < total_ms ? (double)elapsed_ms / (double)total_ms : 1.0;
  return (unsigned)lround(100.0 * pow(1.0 - (1.0 - FLOOR) * progress, GAMMA));
}
