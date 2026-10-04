// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_ANALYSIS_H
#define MAGSAFE_ANALYSIS_H

#include <stddef.h>

/* Turn music into light brightness one frame at a time. The light follows the
 * bass (40-160 Hz): a smoothed bass level scaled to the loudest recent bass,
 * plus a full-brightness pulse on each sharp rise, such as a kick drum. Time
 * is counted in samples, not wall time, so results are repeatable. */

typedef struct {
  double b0, b1, b2, a1, a2; /* coefficients, normalized so that a0 is 1 */
  double z1, z2;
} Biquad;

typedef struct {
  double sample_rate;
  size_t frame_samples; /* nominal frame length, used for empty frames */
  Biquad high_pass, low_pass[2];
  double envelope;   /* smoothed bass amplitude */
  double peak_db;    /* the automatic gain's reference level */
  double pulse;      /* beat pulse, 0 to 1 */
  double since_beat; /* seconds */
} VizState;

void viz_init(VizState *state, double sample_rate, unsigned frame_hz);
/* Analyze one frame of mono samples and return the brightness, 0-100. An
 * empty frame counts as one nominal frame of silence. */
unsigned viz_step(VizState *state, const float *samples, size_t count);

#endif
