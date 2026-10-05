// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "morse.h"
#include "error.h"
#include <stdbool.h>
#include <string.h>

static const struct {
  char character;
  const char *code;
} table[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},    {'D', "-.."},    {'E', "."},
    {'F', "..-."},   {'G', "--."},    {'H', "...."},    {'I', ".."},     {'J', ".---"},
    {'K', "-.-"},    {'L', ".-.."},   {'M', "--"},      {'N', "-."},     {'O', "---"},
    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},     {'S', "..."},    {'T', "-"},
    {'U', "..-"},    {'V', "...-"},   {'W', ".--"},     {'X', "-..-"},   {'Y', "-.--"},
    {'Z', "--.."},   {'0', "-----"},  {'1', ".----"},   {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},   {'7', "--..."},  {'8', "---.."},
    {'9', "----."},  {'.', ".-.-.-"}, {',', "--..--"},  {'?', "..--.."}, {'\'', ".----."},
    {'!', "-.-.--"}, {'/', "-..-."},  {'(', "-.--."},   {')', "-.--.-"}, {'&', ".-..."},
    {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},   {'+', ".-.-."},  {'-', "-....-"},
    {'_', "..--.-"}, {'"', ".-..-."}, {'$', "...-..-"}, {'@', ".--.-."},
};

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

const char *morse_code(char c) {
  c = upper(c);
  for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
    if (table[i].character == c) return table[i].code;
  return NULL;
}

int morse_text(char *const *words, int count, char text[MORSE_TEXT_MAX + 1], char *error,
               size_t size) {
  size_t length = 0;
  for (int w = 0; w < count; ++w) {
    for (const char *p = words[w];; ++p) {
      /* Each run of spaces, and each end of a word, is one space. */
      bool end = !*p, space = end || *p == ' ' || *p == '\t' || *p == '\n';
      if (!space && !morse_code(*p)) {
        if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f)
          return fail(error, size,
                      "'%c' has no Morse code (use letters, digits, and . , ? ' ! / ( ) & : ; = + "
                      "- _ \" $ @)",
                      *p);
        return fail(error, size, "only letters, digits, and some punctuation have Morse codes");
      }
      if (!space && length == MORSE_TEXT_MAX)
        return fail(error, size, "text is too long (at most %u characters)", MORSE_TEXT_MAX);
      if (!space) text[length++] = upper(*p);
      else if (length && length < MORSE_TEXT_MAX && text[length - 1] != ' ') text[length++] = ' ';
      if (end) break;
    }
  }
  if (length && text[length - 1] == ' ') --length;
  text[length] = '\0';
  if (!length) return fail(error, size, "nothing to send");
  return 0;
}

void morse_render(const char *text, char *code, size_t size) {
  size_t used = 0;
  if (!size) return;
  code[0] = '\0';
  for (const char *p = text; *p; ++p) {
    const char *part = *p == ' ' ? "/" : morse_code(*p);
    size_t length = strlen(part), separator = p != text;
    if (used + separator + length + 1 > size) break;
    if (separator) code[used++] = ' ';
    memcpy(code + used, part, length + 1);
    used += length;
  }
}

unsigned long morse_units(const char *text) {
  unsigned long units = 0;
  for (const char *p = text; *p; ++p) {
    if (p != text) units += *p == ' ' || p[-1] == ' ' ? 0 : 3; /* between characters */
    if (*p == ' ') {
      units += MORSE_WORD_GAP;
      continue;
    }
    const char *code = morse_code(*p);
    for (const char *s = code; *s; ++s) units += (s != code) + (*s == '.' ? 1u : 3u);
  }
  return units;
}
