// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "apple-smc.h"
#include "error.h"
#include <string.h>

/* AppleSMC user-client method 2 exchanges one 80-byte message. */
enum {
  STRUCT_METHOD = 2,
  MESSAGE_SIZE = 80,
  KEY_OFFSET = 0,
  SIZE_OFFSET = 28,
  TYPE_OFFSET = 32,
  ATTRIBUTES_OFFSET = 36,
  RESULT_OFFSET = 40,
  COMMAND_OFFSET = 42,
  PAYLOAD_OFFSET = 48,
  READ_KEY = 5,
  WRITE_KEY = 6,
  READ_KEY_INFO = 9
};

#define KEY_ACLC UINT32_C(0x41434c43) /* 'ACLC' */
#define TYPE_UI8 UINT32_C(0x75693820) /* 'ui8 ' */

static void put_word(uint8_t *destination, uint32_t value) {
  memcpy(destination, &value, sizeof(value));
}

static uint32_t get_word(const uint8_t *source) {
  uint32_t value;
  memcpy(&value, source, sizeof(value));
  return value;
}

static int exchange(const SmcClient *client, const uint8_t request[MESSAGE_SIZE],
                    uint8_t response[MESSAGE_SIZE], const char *operation, char *error,
                    size_t size) {
  if (client->connection == IO_OBJECT_NULL) return fail(error, size, "AppleSMC not available");
  size_t response_size = MESSAGE_SIZE;
  memset(response, 0, MESSAGE_SIZE);
  kern_return_t status = IOConnectCallStructMethod(client->connection, STRUCT_METHOD, request,
                                                   MESSAGE_SIZE, response, &response_size);
  if (status != kIOReturnSuccess)
    return fail(error, size, "%s failed (IOKit 0x%08x)", operation, (unsigned)status);
  if (response_size != MESSAGE_SIZE)
    return fail(error, size, "%s failed (%zu-byte response)", operation, response_size);
  if (response[RESULT_OFFSET])
    return fail(error, size, "%s failed (SMC result 0x%02x)", operation, response[RESULT_OFFSET]);
  return 0;
}

/* Build an ACLC request after checking that the key holds one ui8 byte. */
static int aclc_request(const SmcClient *client, uint8_t command, uint8_t request[MESSAGE_SIZE],
                        char *error, size_t size) {
  uint8_t query[MESSAGE_SIZE] = {0};
  uint8_t info[MESSAGE_SIZE] = {0};
  put_word(query + KEY_OFFSET, KEY_ACLC);
  query[COMMAND_OFFSET] = READ_KEY_INFO;
  if (exchange(client, query, info, "reading ACLC information", error, size)) return -1;
  uint32_t key_size = get_word(info + SIZE_OFFSET);
  uint32_t type = get_word(info + TYPE_OFFSET);
  if (key_size != 1 || type != TYPE_UI8)
    return fail(error, size, "ACLC has size %u and type 0x%08x; expected one ui8 byte", key_size,
                type);
  memset(request, 0, MESSAGE_SIZE);
  put_word(request + KEY_OFFSET, KEY_ACLC);
  put_word(request + SIZE_OFFSET, key_size);
  put_word(request + TYPE_OFFSET, type);
  request[ATTRIBUTES_OFFSET] = info[ATTRIBUTES_OFFSET];
  request[COMMAND_OFFSET] = command;
  return 0;
}

int smc_open(SmcClient *client, char *error, size_t size) {
  /* IOServiceGetMatchingService consumes the matching dictionary. */
  io_service_t service =
      IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
  if (service == IO_OBJECT_NULL) return fail(error, size, "AppleSMC service not found");
  kern_return_t status = IOServiceOpen(service, mach_task_self(), 0, &client->connection);
  IOObjectRelease(service);
  if (status != kIOReturnSuccess) {
    smc_close(client);
    return fail(error, size, "cannot open AppleSMC (IOKit 0x%08x)", (unsigned)status);
  }
  return 0;
}

void smc_close(SmcClient *client) {
  if (client->connection != IO_OBJECT_NULL) IOServiceClose(client->connection);
  client->connection = IO_OBJECT_NULL;
}

int smc_read_led(SmcClient *client, uint8_t *mode, char *error, size_t size) {
  uint8_t request[MESSAGE_SIZE] = {0}, response[MESSAGE_SIZE] = {0};
  if (aclc_request(client, READ_KEY, request, error, size) ||
      exchange(client, request, response, "reading ACLC", error, size))
    return -1;
  *mode = response[PAYLOAD_OFFSET];
  return 0;
}

int smc_set_led(SmcClient *client, SmcLedMode mode, char *error, size_t size) {
  if (mode != SMC_LED_AUTO && mode != SMC_LED_OFF && mode != SMC_LED_GREEN && mode != SMC_LED_AMBER)
    return fail(error, size, "unsupported LED mode %d", (int)mode);
  uint8_t request[MESSAGE_SIZE] = {0}, response[MESSAGE_SIZE] = {0};
  if (aclc_request(client, WRITE_KEY, request, error, size)) return -1;
  request[PAYLOAD_OFFSET] = (uint8_t)mode;
  return exchange(client, request, response, "writing ACLC", error, size);
}
