// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_APPLE_SMC_H
#define MAGSAFE_APPLE_SMC_H

#include <IOKit/IOKitLib.h>
#include <stddef.h>
#include <stdint.h>

/* The Mac's MagSafe light color control: the AppleSMC key ACLC. */

typedef enum { SMC_LED_AUTO = 0, SMC_LED_OFF = 1, SMC_LED_GREEN = 3, SMC_LED_AMBER = 4 } SmcLedMode;

/* Start with SmcClient client = {0}. Do not copy an open client. */
typedef struct {
  io_connect_t connection;
} SmcClient;

int smc_open(SmcClient *client, char *error, size_t size);
void smc_close(SmcClient *client);
int smc_read_led(SmcClient *client, uint8_t *mode, char *error, size_t size);
/* Writes require root. */
int smc_set_led(SmcClient *client, SmcLedMode mode, char *error, size_t size);

#endif
