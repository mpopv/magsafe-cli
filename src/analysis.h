// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_ANALYSIS_H
#define MAGSAFE_ANALYSIS_H

#include <stddef.h>

/* Turn music into light brightness, keyed on the kick drum. Every 5 ms, three
 * narrow bass bands (30-160 Hz) are checked for sharp rises. A rise that
 * stands out from the song's recent rises is a beat, and flashes the light by
 * how much it stands out compared with the song's typical beat, so a loud,
 * limited song still flashes fully on its kicks. The rises also give a tempo
 * and a grid of beats. When nearly every grid beat has a beat, as with a kick
 * on every beat, beats on the grid flash fully, smaller rises count there,
 * and beats off it, such as bass notes between kicks, flash dimmer. Flashes
 * fade over a share of the beat period. Between beats, a dim glow follows the
 * bass level, stretched to its recent range, and grows when no beats come.
 * Time is counted in samples, so results are repeatable. */

#define VIZ_BANDS 3
#define VIZ_FLUX_HOPS 6     /* a rise is measured over this many hops */
#define VIZ_TEMPO_HOPS 1200 /* rise history for the tempo: 6 s */

typedef struct {
  double b0, b1, b2, a1, a2; /* coefficients, normalized so that a0 is 1 */
  double z1, z2;
} Biquad;

typedef struct {
  Biquad high_pass, low_pass;
  double energy;                 /* smoothed band energy */
  double history[VIZ_FLUX_HOPS]; /* recent band levels, dB */
} VizBand;

typedef struct {
  double sample_rate;
  size_t frame_samples; /* nominal frame length, used for empty frames */
  size_t hop_samples, hop_used;
  Biquad treble_cut[2]; /* keeps higher sounds out of every band */
  VizBand bands[VIZ_BANDS];
  unsigned long hops;                    /* hops analyzed */
  double rise_mean, rise_deviation;      /* statistics of recent rises, dB */
  double beat_z, typical_z;              /* the current beat's peak, and a typical beat's */
  double beat_share;                     /* the current beat's share of its flash */
  unsigned long beats;                   /* beats detected */
  double since_beat, interval;           /* seconds; interval is between recent beats */
  float onsets[VIZ_TEMPO_HOPS];          /* recent rises above the mean, by hop */
  unsigned char started[VIZ_TEMPO_HOPS]; /* whether a beat started, by hop */
  double period;                         /* beat period in hops, or 0 without a steady tempo */
  unsigned long grid;                    /* the hop of a recent beat on the tempo grid */
  double next_tick;                      /* the hop of the next beat on the grid */
  double occupancy;                      /* share of recent grid beats that had a beat */
  double pulse, decay;                   /* beat flash, 0 to 1, and its time constant */
  double level, floor, ceiling;          /* bass level and its recent range, dB */
  double light;                          /* brightest light in the current frame */
} VizState;

void viz_init(VizState *state, double sample_rate, unsigned frame_hz);
/* Analyze one frame of mono samples and return the brightness, 0-100. An
 * empty frame counts as one nominal frame of silence. */
unsigned viz_step(VizState *state, const float *samples, size_t count);

#endif
