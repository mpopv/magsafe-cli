// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_FIRMWARE_H
#define MAGSAFE_FIRMWARE_H

#include "hpm.h"
#include <stddef.h>
#include <stdint.h>

/* Fixed operations on the A2363 cable firmware. Every operation except
 * fw_open requires the exact supported version. There is no flash, memory,
 * security, or stored-calibration write. */

#define FW_SUPPORTED_VERSION UINT32_C(0x03020000)
#define FW_SUPPORTED_VERSION_TEXT "3.2.0"
#define FW_SECURITY_SETTER_LOCKED UINT32_C(0x100)
#define FW_SECURITY_SIGNATURE_SKIP UINT32_C(0x200)

/* The two brightness scales. The LED color selector reports color + 1. */
typedef enum { FW_AMBER = 0, FW_GREEN = 1 } FwColor;

/* Start with FwClient client = {0}. fw_close also accepts a closed client. */
typedef struct {
  HpmClient *hpm;
  uint32_t version_word;
  size_t version_length;
} FwClient;

typedef struct {
  uint32_t pwm3;
  uint32_t pwm0;
  uint32_t color; /* selector: 0 off, 1 amber, 2 green */
} FwLedState;

/* Open the cable connection and read its version. */
int fw_open(FwClient *client, char *error, size_t size);
void fw_close(FwClient *client);
void fw_format_version(uint32_t version_word, char *text, size_t size);

int fw_read_security(FwClient *client, uint32_t *security, char *error, size_t size);
int fw_read_led(FwClient *client, FwLedState *state, char *error, size_t size);
/* Order: amber PWM0, amber PWM3, green PWM0, green PWM3. */
int fw_read_calibration(FwClient *client, uint16_t values[4], char *error, size_t size);
/* Set one RAM brightness scale to 0..100%. Lasts until the cable resets. */
int fw_set_brightness(FwClient *client, FwColor color, unsigned percent, char *error, size_t size);

/* The PWM value the firmware produces for a calibration value at percent. */
uint32_t fw_predict_pwm(uint16_t calibration, unsigned percent);

#endif
