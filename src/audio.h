// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_AUDIO_H
#define MAGSAFE_AUDIO_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

/* Capture a mono mix of everything the Mac plays, except this process, with a
 * Core Audio process tap. Requires macOS 14.2 and System Audio Recording
 * permission, which macOS keeps per app: for a command-line tool, the
 * terminal app that runs it. Without permission, capture delivers nothing. */
typedef struct Audio Audio;

/* Fail unless the terminal app has System Audio Recording permission. When
 * it was never decided, ask macOS to request it and wait for an answer or a
 * stop signal. Uses private TCC functions, and passes if they are missing. */
int audio_check_permission(const volatile sig_atomic_t *stop, char *error, size_t size);

int audio_open(Audio **audio, char *error, size_t size);
void audio_close(Audio *audio);

/* Copy the samples captured since the previous call, keeping the newest
 * capacity of them, and return how many were copied, or -1. When the default
 * output device changes, capture restarts on the new device: *restarted is
 * then true, and the sample rate and latency may have changed. */
long audio_read(Audio *audio, float *samples, size_t capacity, bool *restarted, char *error,
                size_t size);

double audio_sample_rate(const Audio *audio);

/* How long after capture the default output device plays a sound, as the
 * device reports it. Bluetooth headphones report much more than speakers. */
unsigned audio_output_latency_ms(const Audio *audio);

#endif
