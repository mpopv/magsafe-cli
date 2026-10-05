// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

/* Unit tests for the parts that need no cable, audio, or sudo: the
 * visualizer's analysis and the 'led stream' input parser. */

#include "analysis.h"
#include "stream.h"
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000.0 /* keep in step with the limited mix buffer below */
#define FRAME_HZ 30u
#define FRAME 1600u /* samples per frame at RATE and FRAME_HZ */
#define MAX_FRAMES 600u

static int failures, tests;

static void check(bool ok, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void check(bool ok, const char *format, ...) {
  tests++;
  if (ok) return;
  failures++;
  va_list args;
  va_start(args, format);
  fputs("FAIL: ", stdout);
  vprintf(format, args);
  putchar('\n');
  va_end(args);
}

/* Analysis */

typedef double (*Signal)(size_t index, double gain);

/* Analyze seconds of signal in frames. Return the number of frames. */
static size_t run(Signal signal, double gain, double seconds, unsigned out[MAX_FRAMES]) {
  VizState state;
  viz_init(&state, RATE, FRAME_HZ);
  size_t frames = (size_t)(seconds * FRAME_HZ);
  float buffer[FRAME];
  for (size_t f = 0; f < frames; ++f) {
    for (size_t i = 0; i < FRAME; ++i) buffer[i] = (float)signal(f * FRAME + i, gain);
    out[f] = viz_step(&state, buffer, FRAME);
  }
  return frames;
}

static double silence(size_t index, double gain) {
  (void)index;
  (void)gain;
  return 0.0;
}

/* A 120 BPM kick drum: a decaying 55 Hz tone. Onsets fall 700 samples into
 * a frame, not on a frame boundary. */
#define KICK_PERIOD 24000u
#define KICK_OFFSET 700u
static double kick(size_t index, double gain) {
  double t = (double)((index + KICK_PERIOD - KICK_OFFSET) % KICK_PERIOD) / RATE;
  return gain * 0.8 * exp(-t / 0.08) * sin(2.0 * M_PI * 55.0 * t);
}

/* Repeatable white noise. */
static double noise(size_t index, double gain) {
  uint32_t x = (uint32_t)index * UINT32_C(2654435761);
  x ^= x >> 16;
  x *= UINT32_C(0x45d9f3b);
  x ^= x >> 16;
  return gain * 0.5 * ((double)x / UINT32_MAX * 2.0 - 1.0);
}

/* A 1 kHz tone that fades in over 20 ms, since an abrupt start is a click
 * with bass in it. */
static double treble(size_t index, double gain) {
  double t = (double)index / RATE, fade = t < 0.02 ? (1.0 - cos(M_PI * t / 0.02)) / 2.0 : 1.0;
  return gain * 0.5 * fade * sin(2.0 * M_PI * 1000.0 * t);
}

/* A loud, limited mix like the songs that kept version 0.7.0 near full
 * brightness: a 128 BPM kick, a sustained bass that ducks under each kick,
 * and broadband noise, driven 12 dB into a limiter. */
#define MIX_PERIOD 22500u /* samples per beat at 128 BPM */
#define MIX_SECONDS 10u
static float limited[MIX_SECONDS * 48000u];

static void make_limited_mix(void) {
  double kick_phase = 0.0, envelope = 0.0;
  double attack = exp(-1.0 / (0.001 * RATE)), release = exp(-1.0 / (0.060 * RATE));
  for (size_t i = 0; i < MIX_SECONDS * (size_t)RATE; ++i) {
    double t = (double)((i + MIX_PERIOD - KICK_OFFSET) % MIX_PERIOD) / RATE, time = i / RATE;
    kick_phase += 2.0 * M_PI * (50.0 + 70.0 * exp(-t / 0.04)) / RATE;
    double kick = exp(-t / 0.12) * sin(kick_phase);
    double duck = 1.0 - 0.7 * exp(-t / 0.08);
    double bass =
        0.6 * duck * (sin(2.0 * M_PI * 55.0 * time) + 0.5 * sin(4.0 * M_PI * 55.0 * time));
    double x = (kick + bass + noise(i, 0.6)) * 4.0, level = fabs(x); /* +12 dB */
    envelope = level > envelope ? level + (envelope - level) * attack
                                : level + (envelope - level) * release;
    limited[i] = (float)tanh(x * (envelope > 0.9 ? 0.9 / envelope : 1.0));
  }
}

static double limited_mix(size_t index, double gain) {
  return gain * limited[index % (sizeof(limited) / sizeof(*limited))];
}

static double garbage(size_t index, double gain) {
  if (index % 97 == 0) return NAN;
  if (index % 89 == 0) return -INFINITY;
  return noise(index, gain) * 1e6;
}

/* Count the frames where the light jumps to at least 90%. */
static unsigned count_flashes(const unsigned *out, size_t frames) {
  unsigned flashes = 0;
  for (size_t f = 0; f < frames; ++f) flashes += out[f] >= 90 && (f == 0 || out[f - 1] < 90);
  return flashes;
}

static int compare(const void *a, const void *b) {
  return (int)*(const unsigned *)a - (int)*(const unsigned *)b;
}

static unsigned max_of(const unsigned *out, size_t from, size_t to) {
  unsigned max = 0;
  for (size_t f = from; f < to; ++f)
    if (out[f] > max) max = out[f];
  return max;
}

static unsigned min_of(const unsigned *out, size_t from, size_t to) {
  unsigned min = 100;
  for (size_t f = from; f < to; ++f)
    if (out[f] < min) min = out[f];
  return min;
}

static void test_analysis(void) {
  unsigned out[MAX_FRAMES], quiet[MAX_FRAMES];
  size_t frames;

  frames = run(silence, 1.0, 3.0, out);
  check(max_of(out, 0, frames) == 0, "silence: max %u, expected 0", max_of(out, 0, frames));

  frames = run(kick, 1.0, 4.0, out);
  unsigned flashes = count_flashes(out, frames);
  check(flashes == 8, "kick: %u flashes in 4 s, expected 8", flashes);
  /* Each beat period is 15 frames. The light is dark in the second half of
   * each one. */
  for (size_t beat = 0; beat < 7; ++beat) {
    unsigned low = min_of(out, beat * 15 + 8, beat * 15 + 15);
    check(low <= 10, "kick: beat %zu only falls to %u%%", beat, low);
  }

  run(kick, 0.1, 4.0, quiet);
  unsigned difference = 0;
  for (size_t f = 0; f < frames; ++f) {
    unsigned d = out[f] > quiet[f] ? out[f] - quiet[f] : quiet[f] - out[f];
    if (d > difference) difference = d;
  }
  check(difference <= 1, "kick at -20 dB: differs by up to %u%%", difference);

  /* After the tempo settles, every kick of the limited mix flashes, and the
   * light falls between kicks, instead of staying near full. */
  frames = run(limited_mix, 1.0, MIX_SECONDS, out);
  unsigned kicks = 0, flashed = 0, lit = 0;
  for (size_t sample = KICK_OFFSET; sample < frames * FRAME; sample += MIX_PERIOD) {
    size_t f = sample / FRAME;
    if (f < 3 * FRAME_HZ || f + 12 >= frames) continue;
    kicks++;
    flashed += max_of(out, f, f + 3) >= 80;
    unsigned low = min_of(out, f + 6, f + 12);
    check(low <= 15, "limited mix: the kick at frame %zu only falls to %u%%", f, low);
  }
  for (size_t f = 3 * FRAME_HZ; f < frames; ++f) lit += out[f] >= 60;
  check(flashed == kicks, "limited mix: %u of %u kicks flash to 80%%", flashed, kicks);
  check(lit * 3 < frames - 3 * FRAME_HZ, "limited mix: %u of %zu frames at 60%% or more", lit,
        frames - 3 * FRAME_HZ);

  /* Steady noise has no beat. It may flash now and then, but stays dim. */
  frames = run(noise, 1.0, 10.0, out);
  unsigned noise_flashes = count_flashes(out + 2 * FRAME_HZ, frames - 2 * FRAME_HZ);
  unsigned sorted[MAX_FRAMES];
  memcpy(sorted, out + 2 * FRAME_HZ, (frames - 2 * FRAME_HZ) * sizeof(unsigned));
  qsort(sorted, frames - 2 * FRAME_HZ, sizeof(unsigned), compare);
  unsigned median = sorted[(frames - 2 * FRAME_HZ) / 2];
  check(noise_flashes <= 12, "steady noise: %u flashes in 8 s", noise_flashes);
  check(median <= 20, "steady noise: median %u%%", median);

  frames = run(treble, 1.0, 3.0, out);
  check(max_of(out, 0, frames) == 0, "1 kHz tone: max %u%%, expected 0", max_of(out, 0, frames));

  frames = run(garbage, 1.0, 3.0, out);
  check(max_of(out, 0, frames) <= 100, "garbage input: max %u%%", max_of(out, 0, frames));

  /* A kick after silence flashes, and empty frames count as silence. */
  VizState state;
  viz_init(&state, RATE, FRAME_HZ);
  float buffer[FRAME] = {0};
  for (int f = 0; f < 10; ++f) viz_step(&state, buffer, FRAME);
  for (size_t i = 0; i < FRAME; ++i) buffer[i] = (float)kick(i + KICK_OFFSET, 1.0);
  unsigned first = viz_step(&state, buffer, FRAME), last = first;
  for (int f = 0; f < 60; ++f) last = viz_step(&state, NULL, 0);
  check(first == 100 && last == 0,
        "kick after silence, then empty frames: %u%% then %u%%, "
        "expected 100%% then 0%%",
        first, last);
}

/* Stream parser */

/* Feed each chunk, then finish. Expect the given newest value and count, or
 * the given error message. */
static void expect_stream(const char *const *chunks, int value, unsigned long values,
                          const char *message) {
  StreamParser parser = {0};
  char error[256] = "";
  int result = 0;
  for (size_t i = 0; chunks[i] && !result; ++i)
    result = stream_feed(&parser, chunks[i], strlen(chunks[i]), error, sizeof(error));
  if (!result) result = stream_finish(&parser, error, sizeof(error));
  if (message) {
    check(result && !strcmp(error, message), "stream '%s': got error '%s', expected '%s'",
          chunks[0], error, message);
  } else {
    int got = parser.have_value ? (int)parser.value : -1;
    check(!result && got == value && parser.values == values,
          "stream '%s': got %d (%lu values, error '%s'), expected %d (%lu values)", chunks[0], got,
          parser.values, error, value, values);
  }
}

#define CHUNKS(...) ((const char *const[]){__VA_ARGS__, NULL})

static void test_stream(void) {
  expect_stream(CHUNKS("0\n100\n"), 100, 2, NULL);
  expect_stream(CHUNKS("4", "2\n"), 42, 1, NULL);
  expect_stream(CHUNKS(" 42 \r\n"), 42, 1, NULL);
  expect_stream(CHUNKS("\t7\n\n\n"), 7, 1, NULL);
  expect_stream(CHUNKS("007\n"), 7, 1, NULL);
  expect_stream(CHUNKS("\n\n"), -1, 0, NULL);
  expect_stream(CHUNKS("10\n77"), 77, 2, NULL);
  expect_stream(CHUNKS(""), -1, 0, NULL);
  expect_stream(CHUNKS("101\n"), 0, 0, "invalid brightness '101' on line 1 (expected 0-100)");
  expect_stream(CHUNKS("5\nabc\n"), 0, 0, "invalid brightness 'abc' on line 2 (expected 0-100)");
  expect_stream(CHUNKS("-1\n"), 0, 0, "invalid brightness '-1' on line 1 (expected 0-100)");
  expect_stream(CHUNKS("+4\n"), 0, 0, "invalid brightness '+4' on line 1 (expected 0-100)");
  expect_stream(CHUNKS("1 2\n"), 0, 0, "invalid brightness '1 2' on line 1 (expected 0-100)");
  expect_stream(CHUNKS("1000\n"), 0, 0, "invalid brightness '1000' on line 1 (expected 0-100)");
  expect_stream(CHUNKS("\x01\n"), 0, 0, "invalid brightness '?' on line 1 (expected 0-100)");
  expect_stream(CHUNKS("50\n\n1.5"), 0, 0, "invalid brightness '1.5' on line 3 (expected 0-100)");
  expect_stream(CHUNKS("1111111111111111111111111111111111111111\n"), 0, 0,
                "line 1 is too long (expected a brightness of 0-100)");
}

int main(void) {
  make_limited_mix();
  test_analysis();
  test_stream();
  if (failures) {
    printf("%d of %d unit tests failed.\n", failures, tests);
    return 1;
  }
  printf("All %d unit tests passed.\n", tests);
  return 0;
}
