// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_STREAM_H
#define MAGSAFE_STREAM_H

#include <stdbool.h>
#include <stddef.h>

/* Parse brightness percentages written one per line, as 'led stream' reads
 * them. Spaces, tabs, and a carriage return around a number are allowed, and
 * blank lines are skipped. Start with StreamParser parser = {0}. */

#define STREAM_LINE_MAX 32u /* longest accepted line, without its newline */

typedef struct {
  char line[STREAM_LINE_MAX];
  size_t length;
  bool too_long;
  unsigned long lines;  /* lines seen, for error messages */
  unsigned long values; /* valid values seen */
  bool have_value;
  unsigned value; /* the newest valid value */
} StreamParser;

/* Parse count bytes. Fails on the first invalid line. */
int stream_feed(StreamParser *parser, const char *bytes, size_t count, char *error, size_t size);
/* Parse a last line that has no newline. */
int stream_finish(StreamParser *parser, char *error, size_t size);

#endif
