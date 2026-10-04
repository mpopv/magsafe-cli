// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "analysis.h"
#include <math.h>
#include <stdbool.h>

#define BAND_LOW_HZ 40.0
#define BAND_HIGH_HZ 160.0
#define ATTACK_S 0.010
#define RELEASE_S 0.150
#define GATE_DB -60.0           /* quieter bass is silence */
#define PEAK_FLOOR_DB -50.0     /* quieter bass never reaches full brightness */
#define PEAK_DECAY_DB_PER_S 2.0 /* how fast the gain adapts to quieter music */
#define RANGE_DB 30.0           /* bass this far below the peak is dark */
#define BEAT_RISE_DB 6.0        /* rise above the envelope that counts as a beat */
#define BEAT_WINDOW_DB 12.0     /* beats are at most this far below the peak */
#define BEAT_GAP_S 0.15         /* shortest time between beats */
#define PULSE_DECAY_S 0.15
#define ENVELOPE_SHARE 0.7 /* the envelope's top, which leaves beats room to stand out */
#define GAMMA 2.2          /* perceived brightness is not linear in PWM */
#define SAMPLE_LIMIT 8.0
#define TINY 1e-9

static double decibels(double amplitude) { return 20.0 * log10(amplitude + TINY); }

/* A Butterworth section, from the Audio EQ Cookbook by Robert Bristow-Johnson. */
static Biquad biquad(double rate, double frequency, double q, bool high) {
  double w = 2.0 * M_PI * frequency / rate, c = cos(w), alpha = sin(w) / (2.0 * q);
  double a0 = 1.0 + alpha, b0 = (high ? 1.0 + c : 1.0 - c) / 2.0 / a0;
  return (Biquad){.b0 = b0,
                  .b1 = high ? -2.0 * b0 : 2.0 * b0,
                  .b2 = b0,
                  .a1 = -2.0 * c / a0,
                  .a2 = (1.0 - alpha) / a0};
}

static double filter(Biquad *f, double x) {
  double y = f->b0 * x + f->z1;
  f->z1 = f->b1 * x - f->a1 * y + f->z2;
  f->z2 = f->b2 * x - f->a2 * y;
  return y;
}

void viz_init(VizState *state, double sample_rate, unsigned frame_hz) {
  /* The two low-pass Q values make a fourth-order Butterworth filter. */
  *state = (VizState){
      .sample_rate = sample_rate,
      .frame_samples = (size_t)(sample_rate / frame_hz + 0.5),
      .high_pass = biquad(sample_rate, BAND_LOW_HZ, M_SQRT1_2, true),
      .low_pass = {biquad(sample_rate, BAND_HIGH_HZ, 0.54119610, false),
                   biquad(sample_rate, BAND_HIGH_HZ, 1.30656296, false)},
      .peak_db = PEAK_FLOOR_DB,
      .since_beat = BEAT_GAP_S,
  };
}

unsigned viz_step(VizState *s, const float *samples, size_t count) {
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    double x = isfinite(samples[i]) ? fmin(fmax(samples[i], -SAMPLE_LIMIT), SAMPLE_LIMIT) : 0.0;
    double y = filter(&s->low_pass[1], filter(&s->low_pass[0], filter(&s->high_pass, x)));
    sum += y * y;
  }
  double seconds = (double)(count ? count : s->frame_samples) / s->sample_rate;
  double level = count ? sqrt(sum / (double)count) : 0.0;
  double level_db = decibels(level), before_db = decibels(s->envelope);

  /* The envelope rises fast and falls slowly. The gain follows its peak. */
  double time = level > s->envelope ? ATTACK_S : RELEASE_S;
  s->envelope = level + (s->envelope - level) * exp(-seconds / time);
  double envelope_db = decibels(s->envelope);
  s->peak_db = fmax(fmax(envelope_db, s->peak_db - PEAK_DECAY_DB_PER_S * seconds), PEAK_FLOOR_DB);
  double scaled = envelope_db < GATE_DB ? 0.0 : (envelope_db - s->peak_db + RANGE_DB) / RANGE_DB;
  scaled = fmin(fmax(scaled, 0.0), 1.0);

  /* A beat is a loud frame well above the envelope that came before it. */
  s->since_beat += seconds;
  s->pulse *= exp(-seconds / PULSE_DECAY_S);
  if (level_db - before_db >= BEAT_RISE_DB && level_db >= GATE_DB &&
      level_db >= s->peak_db - BEAT_WINDOW_DB && s->since_beat >= BEAT_GAP_S) {
    s->pulse = 1.0;
    s->since_beat = 0.0;
  }
  double light = fmax(s->pulse, ENVELOPE_SHARE * scaled);
  return (unsigned)lround(100.0 * pow(light, GAMMA));
}
