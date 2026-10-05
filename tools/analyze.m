// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

/* Development tool: run the visualizer's analysis over audio files and show
 * the light as a sparkline, 30 characters a second, with statistics. Levels
 * are perceived brightness, which undoes the analysis's gamma curve. Several
 * files are looped to the same length and mixed, so that single-instrument
 * loops can stand in for a song. --limit drives the mix into a limiter, as
 * loud mastering does.
 *
 * Usage: build/analyze [--limit] [--seconds <s>] <file>...
 */

#include "analysis.h"
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000.0
#define FRAME_HZ 30u
#define FRAME 1600u
#define DRIVE_DB 12.0
#define CEILING 0.9

/* Decode a file to mono samples at RATE. */
static float *load(const char *path, size_t *count) {
  ExtAudioFileRef file;
  NSURL *url = [NSURL fileURLWithPath:@(path)];
  if (ExtAudioFileOpenURL((__bridge CFURLRef)url, &file)) return NULL;
  AudioStreamBasicDescription format = {0};
  UInt32 size = sizeof(format);
  ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &format);
  UInt32 channels = format.mChannelsPerFrame ? format.mChannelsPerFrame : 1;
  AudioStreamBasicDescription client = {.mSampleRate = RATE,
                                        .mFormatID = kAudioFormatLinearPCM,
                                        .mFormatFlags =
                                            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                        .mBytesPerPacket = 4 * channels,
                                        .mFramesPerPacket = 1,
                                        .mBytesPerFrame = 4 * channels,
                                        .mChannelsPerFrame = channels,
                                        .mBitsPerChannel = 32};
  ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client);
  size_t capacity = 1 << 20, used = 0;
  float *mono = malloc(capacity * sizeof(float)), *chunk = malloc(4096 * channels * sizeof(float));
  for (;;) {
    AudioBufferList list = {1, {{channels, 4096 * channels * 4, chunk}}};
    UInt32 frames = 4096;
    if (ExtAudioFileRead(file, &frames, &list) || frames == 0) break;
    if (used + frames > capacity) mono = realloc(mono, (capacity *= 2) * sizeof(float));
    for (UInt32 f = 0; f < frames; ++f) {
      float sum = 0;
      for (UInt32 c = 0; c < channels; ++c) sum += chunk[f * channels + c];
      mono[used++] = sum / (float)channels;
    }
  }
  free(chunk);
  ExtAudioFileDispose(file);
  *count = used;
  return mono;
}

/* A fast peak limiter after DRIVE_DB of gain: 1 ms attack, 60 ms release. */
static void limit(float *samples, size_t count) {
  double drive = pow(10.0, DRIVE_DB / 20.0), envelope = 0;
  double attack = exp(-1.0 / (0.001 * RATE)), release = exp(-1.0 / (0.060 * RATE));
  for (size_t i = 0; i < count; ++i) {
    double x = samples[i] * drive, level = fabs(x);
    envelope = level > envelope ? level + (envelope - level) * attack
                                : level + (envelope - level) * release;
    double gain = envelope > CEILING ? CEILING / envelope : 1.0;
    samples[i] = (float)tanh(x * gain);
  }
}

static int compare(const void *a, const void *b) {
  return (int)*(const unsigned *)a - (int)*(const unsigned *)b;
}

int main(int argc, char **argv) {
  bool limited = false, times = false;
  double seconds = 0;
  int first = 1;
  for (; first < argc && argv[first][0] == '-'; ++first) {
    if (!strcmp(argv[first], "--limit")) limited = true;
    else if (!strcmp(argv[first], "--times")) times = true;
    else if (!strcmp(argv[first], "--seconds") && first + 1 < argc) seconds = atof(argv[++first]);
    else {
      fprintf(stderr, "usage: analyze [--limit] [--times] [--seconds <s>] <file>...\n");
      return 2;
    }
  }
  if (first == argc) {
    fprintf(stderr, "usage: analyze [--limit] [--times] [--seconds <s>] <file>...\n");
    return 2;
  }

  int files = argc - first;
  float *sources[64];
  size_t lengths[64], total = 0;
  for (int i = 0; i < files && i < 64; ++i) {
    if (!(sources[i] = load(argv[first + i], &lengths[i])) || !lengths[i]) {
      fprintf(stderr, "analyze: cannot read %s\n", argv[first + i]);
      return 1;
    }
    if (lengths[i] > total) total = lengths[i];
  }
  if (seconds > 0) total = (size_t)(seconds * RATE);
  float *mix = calloc(total, sizeof(float));
  for (int i = 0; i < files; ++i)
    for (size_t s = 0; s < total; ++s) mix[s] += sources[i][s % lengths[i]];
  if (limited) limit(mix, total);

  VizState state;
  viz_init(&state, RATE, FRAME_HZ);
  size_t frames = total / FRAME;
  unsigned *out = malloc(frames * sizeof(unsigned)), *sorted = malloc(frames * sizeof(unsigned));
  static const char *const blocks[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  unsigned flashes = 0, high = 0;
  bool *beat = calloc(frames, sizeof(bool));
  for (size_t f = 0; f < frames; ++f) {
    unsigned long beats = state.beats;
    unsigned percent = viz_step(&state, mix + f * FRAME, FRAME);
    beat[f] = state.beats != beats;
    if (times && f % (2 * FRAME_HZ) == 0)
      fprintf(stderr, "%zus:%.0f ", f / FRAME_HZ,
              state.period ? 60.0 / (state.period * state.hop_samples / RATE) : 0.0);
    out[f] = (unsigned)lround(100.0 * pow(percent / 100.0, 1.0 / 2.2));
    flashes += out[f] >= 90 && (f == 0 || out[f - 1] < 90);
    high += out[f] >= 60;
    if (f % FRAME_HZ == 0) printf("%s%3zus ", f ? "\n" : "", f / FRAME_HZ);
    fputs(blocks[(out[f] * 8 + 50) / 100], stdout);
  }
  memcpy(sorted, out, frames * sizeof(unsigned));
  qsort(sorted, frames, sizeof(unsigned), compare);
  /* Each beat's frame, and its brightest frame within the next two. */
  if (times) {
    printf("\nbeats:");
    for (size_t f = 0; f < frames; ++f) {
      if (!beat[f]) continue;
      unsigned peak = out[f];
      for (size_t g = f + 1; g < frames && g <= f + 2; ++g)
        if (out[g] > peak) peak = out[g];
      printf(" %zu:%u", f, peak);
    }
  }
  printf("\nperceived: flashes to 90%%+ %u (%.1f/s), frames at 60%%+ %.0f%%, "
         "p10 %u, p50 %u, p90 %u\n",
         flashes, flashes * (double)FRAME_HZ / (double)frames, 100.0 * high / frames,
         sorted[frames / 10], sorted[frames / 2], sorted[frames * 9 / 10]);
  return 0;
}
