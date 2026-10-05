// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_MORSE_H
#define MAGSAFE_MORSE_H

#include <stddef.h>

/* International Morse code. Timing is in units: a dot is 1 unit and a dash 3,
 * with 1 unit between the parts of a character, 3 between characters, and 7
 * between words. */

#define MORSE_TEXT_MAX 200u /* characters of normalized text */
#define MORSE_WORD_GAP 7u

/* The dots and dashes for c, in either case, or null if Morse has none. */
const char *morse_code(char c);

/* Join count words with single spaces, in upper case, into text, which holds
 * MORSE_TEXT_MAX + 1 bytes. Fails on a character without a code, on text
 * longer than MORSE_TEXT_MAX, or on text with no characters. */
int morse_text(char *const *words, int count, char text[MORSE_TEXT_MAX + 1], char *error,
               size_t size);

/* Write text as dots and dashes: characters separated by spaces, and words by
 * " / ". Truncates to fit. */
void morse_render(const char *text, char *code, size_t size);

/* Units from the start of the first dot or dash to the end of the last. */
unsigned long morse_units(const char *text);

#endif
