// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_HPM_H
#define MAGSAFE_HPM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Messages to the cable through the MagSafe port's AppleHPM controller, a
 * private Apple interface. Only the fixed firmware offsets that firmware.c
 * uses are accepted, with 4 to 24 bytes in multiples of 4. */
typedef struct HpmClient HpmClient;

/* Whether a cable is connected to the MagSafe port. Reads the registry only. */
bool hpm_connected(void);

int hpm_open(HpmClient **client, char *error, size_t size);
void hpm_close(HpmClient *client);
int hpm_read(HpmClient *client, uint16_t offset, void *output, size_t capacity, size_t *received,
             char *error, size_t size);
int hpm_write(HpmClient *client, uint16_t offset, const void *input, size_t length, char *error,
              size_t size);

#endif
