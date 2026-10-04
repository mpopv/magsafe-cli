// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "firmware.h"
#include "error.h"
#include <stdio.h>

#define VERSION_OFFSET UINT16_C(0x2800)
#define SECURITY_OFFSET UINT16_C(0x3c00)
#define DIAGNOSTIC_OFFSET UINT16_C(0x5000)
#define READ_LED 0x15
#define READ_CALIBRATION UINT32_C(0x07)
#define SET_BRIGHTNESS UINT32_C(0x10)
#define FIXED_POINT_ONE UINT32_C(16384)
#define FULL_CURVE_VALUE UINT32_C(16367)

static uint32_t read_word(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
         ((uint32_t)bytes[3] << 24);
}

static void write_word(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

static int require_supported(const FwClient *client, char *error, size_t size) {
  if (!client->hpm) return fail(error, size, "cable not available");
  if (client->version_length != 4 || client->version_word != FW_SUPPORTED_VERSION) {
    char version[16];
    fw_format_version(client->version_word, version, sizeof(version));
    return fail(error, size,
                "requires cable firmware " FW_SUPPORTED_VERSION_TEXT " (cable reports %s)",
                version);
  }
  return 0;
}

static int read_exact(FwClient *client, uint16_t offset, uint8_t *bytes, size_t length, char *error,
                      size_t size) {
  size_t received = 0;
  if (hpm_read(client->hpm, offset, bytes, length, &received, error, size)) return -1;
  if (received != length)
    return fail(error, size, "unexpected firmware response size %zu (expected %zu)", received,
                length);
  return 0;
}

/* Run a fixed diagnostic command. Its result is read back from the same offset. */
static int diagnostic(FwClient *client, uint32_t command, uint32_t argument, uint8_t result[4],
                      char *error, size_t size) {
  uint8_t payload[8];
  write_word(payload, command);
  write_word(payload + 4, argument);
  if (hpm_write(client->hpm, DIAGNOSTIC_OFFSET, payload, sizeof(payload), error, size)) return -1;
  return read_exact(client, DIAGNOSTIC_OFFSET, result, 4, error, size);
}

int fw_open(FwClient *client, char *error, size_t size) {
  *client = (FwClient){0};
  if (hpm_open(&client->hpm, error, size)) return -1;
  uint8_t version[24];
  size_t received = 0;
  if (hpm_read(client->hpm, VERSION_OFFSET, version, sizeof(version), &received, error, size)) {
    fw_close(client);
    return -1;
  }
  client->version_word = read_word(version);
  client->version_length = received;
  return 0;
}

void fw_close(FwClient *client) {
  hpm_close(client->hpm);
  *client = (FwClient){0};
}

void fw_format_version(uint32_t version_word, char *text, size_t size) {
  /* 0xMMmmpp00, where the major version is the low six bits of MM. */
  snprintf(text, size, "%u.%u.%u", (version_word >> 24) & 63u, (version_word >> 16) & 255u,
           (version_word >> 8) & 255u);
}

int fw_read_security(FwClient *client, uint32_t *security, char *error, size_t size) {
  uint8_t bytes[4];
  if (require_supported(client, error, size) ||
      read_exact(client, SECURITY_OFFSET, bytes, sizeof(bytes), error, size))
    return -1;
  *security = read_word(bytes);
  return 0;
}

int fw_read_led(FwClient *client, FwLedState *state, char *error, size_t size) {
  const uint8_t command[4] = {READ_LED, 0, 0, 0};
  uint8_t bytes[12];
  if (require_supported(client, error, size) ||
      hpm_write(client->hpm, DIAGNOSTIC_OFFSET, command, sizeof(command), error, size) ||
      read_exact(client, DIAGNOSTIC_OFFSET, bytes, sizeof(bytes), error, size))
    return -1;
  uint32_t color = read_word(bytes + 8);
  if (color > 2) return fail(error, size, "unexpected LED color selector %u", color);
  *state = (FwLedState){.pwm3 = read_word(bytes), .pwm0 = read_word(bytes + 4), .color = color};
  return 0;
}

int fw_read_calibration(FwClient *client, uint16_t values[4], char *error, size_t size) {
  if (require_supported(client, error, size)) return -1;
  for (uint32_t index = 0; index < 4; ++index) {
    uint8_t bytes[4];
    if (diagnostic(client, READ_CALIBRATION, index, bytes, error, size)) return -1;
    uint32_t value = read_word(bytes);
    if (value > UINT16_MAX)
      return fail(error, size, "unexpected calibration value %u at index %u", value, index);
    values[index] = (uint16_t)value;
  }
  return 0;
}

int fw_set_brightness(FwClient *client, FwColor color, unsigned percent, char *error, size_t size) {
  if ((color != FW_AMBER && color != FW_GREEN) || percent > 100)
    return fail(error, size, "invalid brightness request");
  /* The firmware numbers the amber scale 2 and the green scale 1. */
  uint32_t argument = ((color == FW_AMBER ? UINT32_C(2) : UINT32_C(1)) << 16) | percent;
  uint8_t bytes[4];
  if (require_supported(client, error, size) ||
      diagnostic(client, SET_BRIGHTNESS, argument, bytes, error, size))
    return -1;
  uint32_t status = read_word(bytes);
  if (status)
    return fail(error, size, "the cable rejected the brightness change (status %u)", status);
  return 0;
}

uint32_t fw_predict_pwm(uint16_t calibration, unsigned percent) {
  if (percent > 100) return UINT32_MAX;
  uint32_t limited = calibration < 120 ? 120 : calibration > 1000 ? 1000 : calibration;
  uint32_t calibration_scale = limited * FIXED_POINT_ONE / 1000;
  uint32_t percent_scale = percent * FIXED_POINT_ONE / 100;
  uint32_t combined = calibration_scale * percent_scale / FIXED_POINT_ONE;
  if (combined > FIXED_POINT_ONE) combined = FIXED_POINT_ONE;
  return FULL_CURVE_VALUE * combined / FIXED_POINT_ONE;
}
