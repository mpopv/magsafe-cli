// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_VISUALIZER_H
#define MAGSAFE_VISUALIZER_H

#include <stdbool.h>
#include <stddef.h>

#define VISUALIZER_HZ 30u
#define VISUALIZER_HINT                                                                            \
  "magsafe: no audio yet. If something is playing, allow your terminal app under System "          \
  "Settings > Privacy & Security > Screen & System Audio Recording, then run magsafe again."

/* Receives one brightness per frame. hint is true once, when no audio has
 * arrived for several seconds. Returns 0 to continue, or -1 to stop. */
typedef int (*VisualizerSink)(void *context, unsigned percent, bool hint, char *error, size_t size);

/* Capture system audio and pass a brightness to sink VISUALIZER_HZ times a
 * second, delayed to line up with what the output device plays. Runs until a
 * stop signal, an audio error, or a sink error, then returns -1. */
int visualizer_run(VisualizerSink sink, void *context, char *error, size_t size);

#endif
