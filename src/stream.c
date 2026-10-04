// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "stream.h"
#include "error.h"

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Check one complete line and keep its value. */
static int end_line(StreamParser *parser, char *error, size_t size) {
  size_t start = 0, end = parser->length;
  bool too_long = parser->too_long;
  parser->lines++;
  parser->length = 0;
  parser->too_long = false;
  if (too_long)
    return fail(error, size, "line %lu is too long (expected a brightness of 0-100)",
                parser->lines);
  while (start < end && is_space(parser->line[start])) ++start;
  while (end > start && is_space(parser->line[end - 1])) --end;
  if (start == end) return 0;

  unsigned value = 0;
  bool valid = end - start <= 3;
  for (size_t i = start; valid && i < end; ++i) {
    char c = parser->line[i];
    valid = c >= '0' && c <= '9';
    value = value * 10 + (unsigned)(c - '0');
  }
  if (!valid || value > 100) {
    /* Show the line without control or non-ASCII bytes. */
    char shown[STREAM_LINE_MAX + 1];
    size_t length = end - start;
    for (size_t i = 0; i < length; ++i) {
      unsigned char c = (unsigned char)parser->line[start + i];
      shown[i] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    shown[length] = '\0';
    return fail(error, size, "invalid brightness '%s' on line %lu (expected 0-100)", shown,
                parser->lines);
  }
  parser->value = value;
  parser->have_value = true;
  parser->values++;
  return 0;
}

int stream_feed(StreamParser *parser, const char *bytes, size_t count, char *error, size_t size) {
  for (size_t i = 0; i < count; ++i) {
    if (bytes[i] == '\n') {
      if (end_line(parser, error, size)) return -1;
    } else if (parser->length < STREAM_LINE_MAX) {
      parser->line[parser->length++] = bytes[i];
    } else {
      parser->too_long = true;
    }
  }
  return 0;
}

int stream_finish(StreamParser *parser, char *error, size_t size) {
  return parser->length || parser->too_long ? end_line(parser, error, size) : 0;
}
