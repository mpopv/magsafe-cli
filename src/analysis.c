// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "analysis.h"
#include <math.h>
#include <stdbool.h>

#define HOP_S 0.005
#define TREBLE_CUT_HZ 400.0
#define ENERGY_S 0.025 /* band energy smoothing */
#define GATE_DB -60.0  /* quieter bass is silence */

/* Beats. Strengths are in deviations above the mean rise. */
#define STATS_S 2.0 /* how far back the rise statistics look */
#define DEVIATION_MIN_DB 0.5
#define OUTLIER_Z 6.0     /* larger rises count as this strong in the statistics */
#define BEAT_Z 2.5        /* the weakest beat */
#define MIN_RISE_DB 3.0   /* the smallest beat, whatever the statistics */
#define BEAT_GAP_S 0.15   /* shortest time between beats */
#define MIN_FLASH 0.4     /* the dimmest beat flash */
#define TYPICAL_SHARE 0.8 /* beats this share of a typical one flash to full, */
#define FULL_MIN_Z 5.0    /* or off a confident grid, at least this strong */
#define TYPICAL_WEIGHT 0.3
#define TYPICAL_MAX_Z 12.0 /* a stronger beat, such as one after silence, counts as this */

/* Flash decay follows the tempo: a share of the time between beats. */
#define DECAY_SHARE 0.3
#define DECAY_MIN_S 0.06
#define DECAY_MAX_S 0.25
#define DECAY_DEFAULT_S 0.14
#define INTERVAL_MIN_S 0.25
#define INTERVAL_MAX_S 1.5

/* Tempo: the autocorrelation of recent rises, weighted toward 120 BPM. */
#define TEMPO_UPDATE_HOPS 50 /* every 0.25 s */
#define TEMPO_MIN_BPM 70.0
#define TEMPO_MAX_BPM 180.0
#define TEMPO_CENTER_BPM 120.0
#define TEMPO_OCTAVES 1.0    /* width of the weighting */
#define PERIODICITY_MIN 0.25 /* weaker repetition is no steady tempo */
#define MAX_LAG 256
#define GRID_TOLERANCE 0.12 /* on the grid: within this share of a period */
#define OCCUPANCY_LOW 0.55  /* the grid counts only when most grid beats had a beat, */
#define OCCUPANCY_HIGH 0.7  /* as with a kick on every beat, and fully from here */
#define GRID_BEAT_Z 1.5     /* the weakest beat where a confident grid expects one */
#define OFF_GRID_SHARE 0.45 /* off a confident grid, beats flash this share */

/* Glow between beats */
#define LEVEL_S 0.08
#define RANGE_RELAX_DB_PER_S 6.0
#define RANGE_MIN_DB 12.0 /* a smaller range of bass levels never spans the full glow */
#define GLOW_WITH_BEATS 0.2
#define GLOW_WITHOUT_BEATS 0.7
#define BEATLESS_S 1.5 /* after this long without beats, the glow grows over as long again */

#define GAMMA 2.2 /* perceived brightness is not linear in PWM */
#define SAMPLE_LIMIT 8.0
/* Two sections with these Q values make a fourth-order Butterworth filter. */
#define Q1 0.54119610
#define Q2 1.30656296
#define TINY 1e-12

static const double band_edges[VIZ_BANDS][2] = {{30, 60}, {60, 100}, {100, 160}};

static double clamp(double x, double low, double high) { return fmin(fmax(x, low), high); }

/* A filter section, from the Audio EQ Cookbook by Robert Bristow-Johnson. */
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
  *state = (VizState){
      .sample_rate = sample_rate,
      .frame_samples = (size_t)(sample_rate / frame_hz + 0.5),
      .hop_samples = (size_t)(sample_rate * HOP_S + 0.5),
      .since_beat = BEAT_GAP_S,
      .decay = DECAY_DEFAULT_S,
      .typical_z = BEAT_Z + 1.0,
      .level = GATE_DB,
      .floor = GATE_DB,
      .ceiling = GATE_DB,
  };
  state->treble_cut[0] = biquad(sample_rate, TREBLE_CUT_HZ, Q1, false);
  state->treble_cut[1] = biquad(sample_rate, TREBLE_CUT_HZ, Q2, false);
  for (int b = 0; b < VIZ_BANDS; ++b) {
    VizBand *band = &state->bands[b];
    band->high_pass = biquad(sample_rate, band_edges[b][0], M_SQRT1_2, true);
    band->low_pass = biquad(sample_rate, band_edges[b][1], M_SQRT1_2, false);
  }
}

/* The rise above the mean from ago hops before the newest. */
static double onset(const VizState *s, size_t ago) {
  return s->onsets[(s->hops - 1 - ago) % VIZ_TEMPO_HOPS];
}

/* How sure the grid is: 1 when nearly every grid beat had a beat. */
static double grid_confidence(const VizState *s) {
  if (!s->period) return 0.0;
  return clamp((s->occupancy - OCCUPANCY_LOW) / (OCCUPANCY_HIGH - OCCUPANCY_LOW), 0.0, 1.0);
}

/* Find a steady tempo in the recent rises, and the grid of beats on it. */
static void estimate_tempo(VizState *s) {
  double hop_s = (double)s->hop_samples / s->sample_rate;
  size_t n = s->hops < VIZ_TEMPO_HOPS ? (size_t)s->hops : VIZ_TEMPO_HOPS;
  size_t lag_min = (size_t)ceil(60.0 / (TEMPO_MAX_BPM * hop_s));
  size_t lag_max = (size_t)floor(60.0 / (TEMPO_MIN_BPM * hop_s));
  double energy = 0.0, correlation[MAX_LAG + 2] = {0};
  s->period = 0.0;
  if (n < VIZ_TEMPO_HOPS / 2 || lag_max + 1 > MAX_LAG || lag_min < 2) return;
  for (size_t i = 0; i < n; ++i) energy += onset(s, i) * onset(s, i);
  if (energy <= 0.0) return;

  size_t best = 0;
  double best_score = 0.0;
  for (size_t lag = lag_min - 1; lag <= lag_max + 1; ++lag) {
    double sum = 0.0;
    for (size_t i = 0; i + lag < n; ++i) sum += onset(s, i) * onset(s, i + lag);
    correlation[lag] = sum / (double)(n - lag);
    double bpm = 60.0 / ((double)lag * hop_s),
           octaves = log2(bpm / TEMPO_CENTER_BPM) / TEMPO_OCTAVES;
    double score = correlation[lag] * exp(-0.5 * octaves * octaves);
    if (lag >= lag_min && lag <= lag_max && score > best_score) {
      best = lag;
      best_score = score;
    }
  }
  if (!best || correlation[best] / (energy / (double)n) < PERIODICITY_MIN) return;

  /* Refine the period between hops, then find the phase whose beats line up
   * with the most rises. */
  double before = correlation[best - 1], peak = correlation[best], after = correlation[best + 1];
  double curve = before - 2.0 * peak + after;
  double period = (double)best + (curve < 0.0 ? 0.5 * (before - after) / curve : 0.0);
  size_t best_phase = 0;
  double best_sum = -1.0;
  for (size_t phase = 0; phase < (size_t)period; ++phase) {
    double sum = 0.0;
    for (double at = (double)phase; at + 1.0 < (double)n; at += period) {
      size_t i = (size_t)lround(at);
      double strongest = onset(s, i);
      if (i > 0) strongest = fmax(strongest, onset(s, i - 1));
      if (i + 1 < n) strongest = fmax(strongest, onset(s, i + 1));
      sum += strongest;
    }
    if (sum > best_sum) {
      best_sum = sum;
      best_phase = phase;
    }
  }
  /* Occupancy: the share of grid beats in the history that had a beat. */
  size_t tolerance = (size_t)(GRID_TOLERANCE * period), ticks = 0, hits = 0;
  for (double at = (double)best_phase; at + (double)tolerance < (double)n; at += period) {
    size_t i = (size_t)lround(at);
    if (i < tolerance) continue; /* this beat's window is still open */
    bool hit = false;
    for (size_t j = i - tolerance; j <= i + tolerance && !hit; ++j)
      hit = s->started[(s->hops - 1 - j) % VIZ_TEMPO_HOPS];
    ticks++;
    hits += hit;
  }
  s->occupancy = ticks ? (double)hits / (double)ticks : 0.0;
  s->period = period;
  s->grid = s->hops - 1 - best_phase;
  s->next_tick = (double)s->grid;
  while (s->next_tick + GRID_TOLERANCE * period < (double)(s->hops - 1)) s->next_tick += period;
  s->decay = clamp(DECAY_SHARE * period * hop_s, DECAY_MIN_S, DECAY_MAX_S);
}

/* Analyze the band energies at the end of one hop. */
static void hop(VizState *s) {
  double seconds = (double)s->hop_samples / s->sample_rate, rise = 0.0, total = 0.0;
  unsigned slot = s->hops % VIZ_FLUX_HOPS;
  for (int b = 0; b < VIZ_BANDS; ++b) {
    VizBand *band = &s->bands[b];
    /* Rises start from the gate, so a sound out of silence is a fair beat. */
    double level = fmax(10.0 * log10(band->energy + TINY), GATE_DB);
    if (s->hops >= VIZ_FLUX_HOPS && level > band->history[slot])
      rise += level - band->history[slot];
    band->history[slot] = level;
    total += band->energy;
  }
  s->hops++;
  double level = fmax(10.0 * log10(total + TINY), GATE_DB - 20.0);
  bool audible = level >= GATE_DB;
  /* An outlier, such as the first sound after silence, counts only as a
   * large rise, in the tempo history and in the statistics, so that it hides
   * neither the tempo nor the beats after it. */
  double deviation = fmax(s->rise_deviation, DEVIATION_MIN_DB);
  double counted = fmin(rise, s->rise_mean + OUTLIER_Z * deviation);
  s->onsets[(s->hops - 1) % VIZ_TEMPO_HOPS] = (float)fmax(counted - s->rise_mean, 0.0);
  s->started[(s->hops - 1) % VIZ_TEMPO_HOPS] = 0;
  if (s->hops % TEMPO_UPDATE_HOPS == 0) estimate_tempo(s);
  if (s->period && (double)(s->hops - 1) > s->next_tick + GRID_TOLERANCE * s->period)
    s->next_tick += s->period;

  /* A beat is a rise well above the recent rises. Its flash shows how far.
   * A confident grid expects a beat, so a smaller rise will do there. */
  double z = (rise - s->rise_mean) / deviation;
  double confidence = grid_confidence(s), threshold = BEAT_Z;
  if (confidence > 0.0 && fabs((double)(s->hops - 1) - s->next_tick) <= GRID_TOLERANCE * s->period)
    threshold -= (BEAT_Z - GRID_BEAT_Z) * confidence;
  s->since_beat += seconds;
  s->pulse *= exp(-seconds / s->decay);
  if (audible && rise >= MIN_RISE_DB && z >= threshold) {
    if (s->since_beat >= BEAT_GAP_S) {
      /* The song's typical full-strength beat flashes to full, and weaker
       * ones less. */
      if (s->beat_z && s->beat_share == 1.0)
        s->typical_z += (fmin(s->beat_z, TYPICAL_MAX_Z) - s->typical_z) * TYPICAL_WEIGHT;
      s->beat_z = 0.0;
      /* Without a steady tempo, the flash decay follows the beat intervals. */
      if (!s->period && s->since_beat >= INTERVAL_MIN_S && s->since_beat <= INTERVAL_MAX_S) {
        s->interval =
            s->interval ? s->interval + (s->since_beat - s->interval) * 0.3 : s->since_beat;
        s->decay = clamp(DECAY_SHARE * s->interval, DECAY_MIN_S, DECAY_MAX_S);
      }
      /* With a kick on nearly every grid beat, a beat off the grid is
       * probably something else, such as a bass note. */
      s->beat_share = 1.0;
      if (s->period) {
        double offset = fmod((double)(s->hops - 1 - s->grid), s->period);
        if (fmin(offset, s->period - offset) > GRID_TOLERANCE * s->period)
          s->beat_share = 1.0 - (1.0 - OFF_GRID_SHARE) * confidence;
      }
      s->since_beat = 0.0;
      s->beats++;
      s->started[(s->hops - 1) % VIZ_TEMPO_HOPS] = 1;
    }
    /* A beat that is still rising brightens its flash. */
    s->beat_z = fmax(s->beat_z, z);
    double full = fmax(TYPICAL_SHARE * s->typical_z, FULL_MIN_Z);
    double strength = clamp((z - threshold) / (full - threshold), 0.0, 1.0);
    /* A beat on a confident grid is a kick, and flashes to full. */
    if (s->beat_share == 1.0) strength = fmax(strength, confidence);
    s->pulse = fmax(s->pulse, s->beat_share * (MIN_FLASH + (1.0 - MIN_FLASH) * strength));
  }
  /* Until the window fills, the statistics average everything so far. */
  double weight = fmax(1.0 - exp(-seconds / STATS_S), 1.0 / s->hops);
  s->rise_mean += (counted - s->rise_mean) * weight;
  s->rise_deviation += (fabs(counted - s->rise_mean) - s->rise_deviation) * weight;

  /* The glow follows the bass level within its recent range. */
  s->level += (level - s->level) * (1.0 - exp(-seconds / LEVEL_S));
  s->ceiling = fmax(s->level, s->ceiling - RANGE_RELAX_DB_PER_S * seconds);
  s->floor = fmin(s->level, s->floor + RANGE_RELAX_DB_PER_S * seconds);
  double span = fmax(s->ceiling - s->floor, RANGE_MIN_DB);
  double glow = s->level < GATE_DB ? 0.0 : clamp((s->level - s->ceiling + span) / span, 0.0, 1.0);
  double beatless = clamp((s->since_beat - BEATLESS_S) / BEATLESS_S, 0.0, 1.0);
  double share = GLOW_WITH_BEATS + (GLOW_WITHOUT_BEATS - GLOW_WITH_BEATS) * beatless;
  s->light = fmax(s->light, fmax(s->pulse, share * glow));
}

unsigned viz_step(VizState *s, const float *samples, size_t count) {
  double smoothing = 1.0 - exp(-1.0 / (ENERGY_S * s->sample_rate));
  size_t length = count ? count : s->frame_samples;
  unsigned long hops = s->hops;
  double previous = s->light;
  s->light = 0.0;
  for (size_t i = 0; i < length; ++i) {
    double x = count && isfinite(samples[i]) ? clamp(samples[i], -SAMPLE_LIMIT, SAMPLE_LIMIT) : 0.0;
    x = filter(&s->treble_cut[1], filter(&s->treble_cut[0], x));
    for (int b = 0; b < VIZ_BANDS; ++b) {
      VizBand *band = &s->bands[b];
      double y = filter(&band->low_pass, filter(&band->high_pass, x));
      band->energy += (y * y - band->energy) * smoothing;
    }
    if (++s->hop_used == s->hop_samples) {
      s->hop_used = 0;
      hop(s);
    }
  }
  /* A frame shorter than a hop keeps the last brightness. */
  if (s->hops == hops) s->light = previous;
  return (unsigned)lround(100.0 * pow(s->light, GAMMA));
}
