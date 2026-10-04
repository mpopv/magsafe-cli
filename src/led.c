// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "led.h"
#include "error.h"
#include "stream.h"
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_MS UINT64_C(1000000)
#define NS_PER_SECOND UINT64_C(1000000000)
#define RAMP_STEP_MS 50u
#define VERIFY_POLL_MS 100u
#define VERIFY_TIMEOUT_MS 2000u
#define STREAM_CHECK_MS 5000u
#define STREAM_WAKE_MS 100u /* longest wait, so a stop signal is seen promptly */

volatile sig_atomic_t led_stop_signal;

static void stop(int signal) { led_stop_signal = signal; }

int led_catch_signals(char *error, size_t size) {
  struct sigaction action = {.sa_handler = stop};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
      sigaction(SIGHUP, &action, NULL))
    return fail(error, size, "cannot install signal handlers: %s", strerror(errno));
  return 0;
}

static int interrupted(char *error, size_t size) {
  return led_stop_signal ? fail(error, size, "interrupted by signal %d", (int)led_stop_signal) : 0;
}

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

/* Sleep until a monotonic deadline. A stop signal ends the wait early. */
static int wait_until(uint64_t deadline, char *error, size_t size) {
  for (uint64_t now; !led_stop_signal && (now = now_ns()) < deadline;) {
    uint64_t left = deadline - now;
    struct timespec pause = {(time_t)(left / NS_PER_SECOND), (long)(left % NS_PER_SECOND)};
    nanosleep(&pause, NULL);
  }
  return interrupted(error, size);
}

static int wait_ms(unsigned milliseconds, char *error, size_t size) {
  return wait_until(now_ns() + milliseconds * NS_PER_MS, error, size);
}

static SmcLedMode smc_mode(FwColor color) {
  return color == FW_GREEN ? SMC_LED_GREEN : SMC_LED_AMBER;
}

static int set_brightness(FwClient *fw, FwColor color, unsigned percent, char *error, size_t size) {
  if (interrupted(error, size)) return -1;
  return fw_set_brightness(fw, color, percent, error, size);
}

/* Poll until the cable's PWM output matches color at percent. */
static int verify(FwClient *fw, const uint16_t calibration[4], FwColor color, unsigned percent,
                  FwLedState *state, char *error, size_t size) {
  uint64_t deadline = now_ns() + VERIFY_TIMEOUT_MS * NS_PER_MS;
  for (;;) {
    if (interrupted(error, size) || fw_read_led(fw, state, error, size)) return -1;
    if (state->color == color + 1u &&
        state->pwm0 == fw_predict_pwm(calibration[color * 2], percent) &&
        state->pwm3 == fw_predict_pwm(calibration[color * 2 + 1], percent))
      return 0;
    uint64_t now = now_ns();
    if (now >= deadline) break;
    uint64_t next = now + VERIFY_POLL_MS * NS_PER_MS;
    if (wait_until(next < deadline ? next : deadline, error, size)) return -1;
  }
  return fail(error, size, "PWM readback did not match %u%%: pwm0=%u, pwm3=%u, selector=%u",
              percent, state->pwm0, state->pwm3, state->color);
}

int led_brightness(FwClient *fw, SmcClient *smc, FwColor color, unsigned percent, FwLedState *state,
                   char *error, size_t size) {
  uint16_t calibration[4];
  if (fw_read_calibration(fw, calibration, error, size)) return -1;
  if (fw_set_brightness(fw, color, percent, error, size) ||
      smc_set_led(smc, smc_mode(color), error, size) ||
      verify(fw, calibration, color, percent, state, error, size))
    return append(error, size, "brightness may have changed; 'magsafe reset' restores it");
  return 0;
}

int led_reset(FwClient *fw, SmcClient *smc, char *error, size_t size) {
  char amber[160] = "ok", green[160] = "ok", system[160] = "ok";
  bool failed = fw_set_brightness(fw, FW_AMBER, 100, amber, sizeof(amber)) != 0;
  failed |= fw_set_brightness(fw, FW_GREEN, 100, green, sizeof(green)) != 0;
  failed |= smc_set_led(smc, SMC_LED_AUTO, system, sizeof(system)) != 0;
  if (!failed) return 0;
  return append(error, size,
                "reset incomplete (amber brightness: %s; green brightness: %s; "
                "system color control: %s)",
                amber, green, system);
}

/* Zero both scales, select color, and wait while the cable's own color
 * transition finishes in the dark. The wait is not an optical measurement. */
static int prepare(FwClient *fw, SmcClient *smc, FwColor color, char *error, size_t size) {
  if (set_brightness(fw, FW_AMBER, 0, error, size) ||
      set_brightness(fw, FW_GREEN, 0, error, size) || interrupted(error, size) ||
      smc_set_led(smc, smc_mode(color), error, size))
    return -1;
  return wait_ms(LED_PREPARE_MS, error, size);
}

/* Ramp from 0% to 100%, or back, with at most one write per RAMP_STEP_MS.
 * Slow writes skip steps instead of causing a burst. */
static int ramp(FwClient *fw, FwColor color, bool down, unsigned duration_ms, char *error,
                size_t size) {
  uint64_t start = now_ns(), duration = duration_ms * NS_PER_MS;
  unsigned previous = down ? 100 : 0;
  for (;;) {
    if (wait_ms(RAMP_STEP_MS, error, size)) return -1;
    uint64_t elapsed = now_ns() - start;
    unsigned progress = elapsed >= duration ? 100 : (unsigned)(elapsed * 100 / duration);
    unsigned percent = down ? 100 - progress : progress;
    if (percent != previous && set_brightness(fw, color, percent, error, size)) return -1;
    if (progress == 100) return 0;
    previous = percent;
  }
}

static int blink_once(FwClient *fw, FwColor color, unsigned interval_ms, char *error, size_t size) {
  if (set_brightness(fw, color, 100, error, size) || wait_ms(interval_ms, error, size) ||
      set_brightness(fw, color, 0, error, size) || wait_ms(interval_ms, error, size))
    return -1;
  return 0;
}

/* fade-in ramps up then steps to 0%; fade-out steps to 100% then ramps down;
 * fade ramps both ways. Each cycle checks PWM at 100% and 0%. */
static int fade_once(FwClient *fw, const LedEffect *effect, const uint16_t calibration[4],
                     char *error, size_t size) {
  FwLedState state;
  FwColor color = effect->color;
  int rise = effect->pattern == LED_FADE_OUT
                 ? set_brightness(fw, color, 100, error, size)
                 : ramp(fw, color, false, effect->duration_ms, error, size);
  if (rise || verify(fw, calibration, color, 100, &state, error, size)) return -1;
  int fall = effect->pattern == LED_FADE_IN
                 ? set_brightness(fw, color, 0, error, size)
                 : ramp(fw, color, true, effect->duration_ms, error, size);
  if (fall || verify(fw, calibration, color, 0, &state, error, size)) return -1;
  return wait_ms(effect->interval_ms, error, size);
}

int led_run(FwClient *fw, SmcClient *smc, const LedEffect *effect, char *error, size_t size) {
  uint16_t calibration[4];
  /* This read also checks the firmware version before anything changes. */
  if (fw_read_calibration(fw, calibration, error, size)) return -1;
  bool forever = effect->count == LED_INFINITE;
  FwColor color = effect->color;
  int result = prepare(fw, smc, color, error, size);
  for (unsigned cycle = 0; !result && (forever || cycle < effect->count); ++cycle) {
    if (effect->pattern == LED_BLINK) {
      if (effect->alternate && cycle > 0) {
        color = color == FW_GREEN ? FW_AMBER : FW_GREEN;
        result = prepare(fw, smc, color, error, size);
      }
      if (!result) result = blink_once(fw, color, effect->interval_ms, error, size);
    } else {
      result = fade_once(fw, effect, calibration, error, size);
    }
  }
  /* Cleanup ignores the stop signal so that it always runs. */
  if (led_reset(fw, smc, error, size)) return -1;
  return result ? -1 : interrupted(error, size);
}

/* Fail if the cable no longer drives color, for example because macOS
 * changed the light while a stream ran. */
static int check_color(FwClient *fw, FwColor color, char *error, size_t size) {
  static const char *const names[] = {"off", "amber", "green"};
  FwLedState state;
  if (fw_read_led(fw, &state, error, size)) return -1;
  if (state.color != color + 1u)
    return fail(error, size, "the cable switched to %s; macOS or another program changed the light",
                names[state.color]);
  return 0;
}

int led_stream(FwClient *fw, SmcClient *smc, FwColor color, int input, LedStreamResult *result,
               char *error, size_t size) {
  *result = (LedStreamResult){0};
  FwLedState state;
  /* This read also checks the firmware version before anything changes. */
  if (fw_read_led(fw, &state, error, size)) return -1;
  StreamParser parser = {0};
  unsigned current = 0; /* prepare leaves both scales at 0% */
  uint64_t interval = NS_PER_SECOND / LED_STREAM_HZ, next_write = 0;
  int status = prepare(fw, smc, color, error, size);
  uint64_t next_check = now_ns() + STREAM_CHECK_MS * NS_PER_MS;
  bool ended = false;
  while (!status && !ended) {
    /* Wait for input, for the next allowed write, or for the next check. */
    uint64_t now = now_ns(), deadline = next_check;
    if (parser.have_value && parser.value != current && next_write < deadline)
      deadline = next_write;
    uint64_t wait = deadline > now ? deadline - now : 0;
    if (wait > STREAM_WAKE_MS * NS_PER_MS) wait = STREAM_WAKE_MS * NS_PER_MS;
    struct pollfd descriptor = {.fd = input, .events = POLLIN};
    int ready = poll(&descriptor, 1, (int)((wait + NS_PER_MS - 1) / NS_PER_MS));
    if ((status = interrupted(error, size))) break;
    if (ready < 0 && errno != EINTR && errno != EAGAIN) {
      status = fail(error, size, "cannot wait for input: %s", strerror(errno));
      break;
    }
    if (ready > 0) {
      char buffer[4096];
      ssize_t count = read(input, buffer, sizeof(buffer));
      if (count < 0 && errno != EINTR && errno != EAGAIN) {
        status = fail(error, size, "cannot read input: %s", strerror(errno));
      } else if (count >= 0) {
        ended = count == 0;
        status = ended ? stream_finish(&parser, error, size)
                       : stream_feed(&parser, buffer, (size_t)count, error, size);
        result->invalid_input = status != 0;
      }
    }

    /* Ending input skips its last value, since the reset follows at once. */
    now = now_ns();
    if (!status && !ended && parser.have_value && parser.value != current && now >= next_write) {
      status = set_brightness(fw, color, parser.value, error, size);
      current = parser.value;
      next_write = now + interval;
      result->writes++;
    }
    if (!status && !ended && now >= next_check) {
      status = check_color(fw, color, error, size);
      next_check = now_ns() + STREAM_CHECK_MS * NS_PER_MS;
    }
  }
  result->values = parser.values;
  /* Cleanup ignores the stop signal so that it always runs. */
  if (led_reset(fw, smc, error, size)) return -1;
  return status ? -1 : interrupted(error, size);
}
