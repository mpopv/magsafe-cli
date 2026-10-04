// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "visualizer.h"
#include "analysis.h"
#include "audio.h"
#include "error.h"
#include "led.h"
#include <stdint.h>
#include <time.h>

#define NS_PER_SECOND UINT64_C(1000000000)
#define MAX_DELAY_FRAMES 32u /* about a second */
#define LIGHT_LATENCY_MS 25u /* estimate: half a frame of analysis plus one cable write */
#define HINT_SECONDS 5u
#define CAPACITY 16384u /* samples per read; a frame is about 1600 */

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

/* Frames to hold each brightness so that the light changes when the sound
 * is heard rather than when it is captured. */
static unsigned delay_frames(const Audio *audio) {
  unsigned latency = audio_output_latency_ms(audio);
  if (latency <= LIGHT_LATENCY_MS) return 0;
  unsigned frames = ((latency - LIGHT_LATENCY_MS) * VISUALIZER_HZ + 500) / 1000;
  return frames < MAX_DELAY_FRAMES ? frames : MAX_DELAY_FRAMES - 1;
}

int visualizer_run(VisualizerSink sink, void *context, char *error, size_t size) {
  Audio *audio;
  if (audio_open(&audio, error, size)) return -1;
  static float samples[CAPACITY];
  VizState state;
  viz_init(&state, audio_sample_rate(audio), VISUALIZER_HZ);
  unsigned history[MAX_DELAY_FRAMES] = {0}, delay = delay_frames(audio);
  uint64_t period = NS_PER_SECOND / VISUALIZER_HZ, next = now_ns(), frame = 0;
  bool heard = false;
  int result = 0;
  while (!result) {
    /* Wait for the next frame. After a stall, start again from now. */
    uint64_t now = now_ns();
    next = next + period < now ? now : next + period;
    for (; !led_stop_signal && (now = now_ns()) < next;) {
      uint64_t left = next - now;
      struct timespec pause = {(time_t)(left / NS_PER_SECOND), (long)(left % NS_PER_SECOND)};
      nanosleep(&pause, NULL);
    }
    if (led_stop_signal) {
      result = fail(error, size, "interrupted by signal %d", (int)led_stop_signal);
      break;
    }

    bool restarted;
    long count = audio_read(audio, samples, CAPACITY, &restarted, error, size);
    if (count < 0) break;
    if (restarted) {
      viz_init(&state, audio_sample_rate(audio), VISUALIZER_HZ);
      delay = delay_frames(audio);
    }
    for (long i = 0; !heard && i < count; ++i) heard = samples[i] != 0.0f;
    history[frame % MAX_DELAY_FRAMES] = viz_step(&state, samples, (size_t)count);
    unsigned percent = frame >= delay ? history[(frame - delay) % MAX_DELAY_FRAMES] : 0;
    bool hint = !heard && frame == HINT_SECONDS * VISUALIZER_HZ;
    frame++;
    result = sink(context, percent, hint, error, size);
  }
  audio_close(audio);
  return -1;
}
