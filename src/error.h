// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_ERROR_H
#define MAGSAFE_ERROR_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Functions that can fail return 0 on success, or -1 with a message in an
 * optional caller-supplied buffer. A null buffer discards the message. */

static inline int fail(char *error, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
static inline int append(char *error, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/* Replace the message in error and return -1. */
static inline int fail(char *error, size_t size, const char *format, ...) {
  if (error && size) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
  }
  return -1;
}

/* Add to the message in error, after "; " if it is not empty, and return -1. */
static inline int append(char *error, size_t size, const char *format, ...) {
  if (!error || !size) return -1;
  size_t used = strnlen(error, size);
  if (used && used + 3 < size) {
    memcpy(error + used, "; ", 3);
    used += 2;
  }
  if (used + 1 < size) {
    va_list args;
    va_start(args, format);
    vsnprintf(error + used, size - used, format, args);
    va_end(args);
  }
  return -1;
}

#endif
