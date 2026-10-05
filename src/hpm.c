// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "hpm.h"
#include "error.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define ATOMIC_COMMAND UINT32_C(0x43465561) /* 'CFUa' */
#define REQUEST_TIMEOUT 10

typedef IOReturn (*HpmAtomicCommand)(void *, unsigned char, unsigned long long, unsigned long long,
                                     unsigned long long, unsigned long long, unsigned long long,
                                     unsigned short, unsigned short, unsigned long long,
                                     unsigned long long, unsigned int);

/* The observed layout of the private interface. Only atomic is called. */
typedef struct {
  IUNKNOWN_C_GUTS;
  unsigned char unused[0x48];
  HpmAtomicCommand atomic;
} HpmInterface;

_Static_assert(sizeof(void *) == 8, "Only the observed 64-bit ABI is supported");
_Static_assert(offsetof(HpmInterface, QueryInterface) == 0x08, "QueryInterface offset");
_Static_assert(offsetof(HpmInterface, Release) == 0x18, "Release offset");
_Static_assert(offsetof(HpmInterface, atomic) == 0x68, "atomic offset");

struct HpmClient {
  io_service_t port;
  io_service_t hpm;
  IOCFPlugInInterface **plugin;
  HpmInterface **interface;
};

static CFUUIDRef plugin_type(void) {
  return CFUUIDGetConstantUUIDWithBytes(NULL, 0x12, 0xa1, 0xdc, 0xcf, 0xcf, 0x7a, 0x47, 0x75, 0xbe,
                                        0xe5, 0x9c, 0x43, 0x19, 0xf4, 0xcd, 0x2b);
}

static CFUUIDRef interface_type(void) {
  return CFUUIDGetConstantUUIDWithBytes(NULL, 0xc1, 0x3a, 0xcd, 0xd9, 0x20, 0x9e, 0x4b, 0x01, 0xb7,
                                        0xbe, 0xe0, 0x5c, 0xd8, 0x83, 0xc7, 0xb1);
}

static bool property_is_true(io_registry_entry_t entry, CFStringRef key) {
  CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
  bool result = value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value);
  if (value) CFRelease(value);
  return result;
}

static bool property_is_string(io_registry_entry_t entry, CFStringRef key, CFStringRef expected) {
  CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
  bool result = value && CFGetTypeID(value) == CFStringGetTypeID() && CFEqual(value, expected);
  if (value) CFRelease(value);
  return result;
}

static bool property_is_number(io_registry_entry_t entry, CFStringRef key, int expected) {
  CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
  int actual = 0;
  bool result = value && CFGetTypeID(value) == CFNumberGetTypeID() &&
                CFNumberGetValue(value, kCFNumberIntType, &actual) && actual == expected;
  if (value) CFRelease(value);
  return result;
}

/* Find the single active MagSafe 3 port and its AppleHPM grandparent. */
static int find_port(io_service_t *port_out, io_service_t *hpm_out, char *error, size_t size) {
  io_iterator_t iterator = IO_OBJECT_NULL;
  IOReturn result = IOServiceGetMatchingServices(
      kIOMainPortDefault, IOServiceMatching("AppleHPMInterfaceType11"), &iterator);
  if (result != kIOReturnSuccess)
    return fail(error, size, "cannot search for the MagSafe port (0x%08x)", (unsigned)result);
  io_service_t port = IOIteratorNext(iterator);
  io_service_t extra = IOIteratorNext(iterator);
  IOObjectRelease(iterator);
  if (!port || extra) {
    if (port) IOObjectRelease(port);
    if (extra) IOObjectRelease(extra);
    return fail(error, size, port ? "found more than one MagSafe port" : "no MagSafe port found");
  }
  if (!property_is_true(port, CFSTR("ConnectionActive"))) {
    IOObjectRelease(port);
    return fail(error, size, "no cable is connected to the MagSafe port");
  }
  io_registry_entry_t hal = IO_OBJECT_NULL, hpm = IO_OBJECT_NULL;
  if (!property_is_string(port, CFSTR("PortTypeDescription"), CFSTR("MagSafe 3")) ||
      !property_is_number(port, CFSTR("PortNumber"), 1) ||
      !property_is_number(port, CFSTR("PortType"), 17) ||
      IORegistryEntryGetParentEntry(port, kIOServicePlane, &hal) != kIOReturnSuccess ||
      !IOObjectConformsTo(hal, "AppleHPMDeviceHALType3") ||
      IORegistryEntryGetParentEntry(hal, kIOServicePlane, &hpm) != kIOReturnSuccess ||
      !IOObjectConformsTo(hpm, "AppleHPM")) {
    if (hal) IOObjectRelease(hal);
    if (hpm) IOObjectRelease(hpm);
    IOObjectRelease(port);
    return fail(error, size, "unsupported MagSafe port layout");
  }
  IOObjectRelease(hal);
  *port_out = port;
  *hpm_out = hpm;
  return 0;
}

bool hpm_connected(void) {
  io_service_t port, hpm;
  if (find_port(&port, &hpm, NULL, 0)) return false;
  IOObjectRelease(hpm);
  IOObjectRelease(port);
  return true;
}

int hpm_open(HpmClient **output, char *error, size_t size) {
  *output = NULL;
  HpmClient *client = calloc(1, sizeof(*client));
  if (!client) return fail(error, size, "out of memory");
  if (find_port(&client->port, &client->hpm, error, size)) {
    free(client);
    return -1;
  }
  SInt32 score = 0;
  IOReturn result = IOCreatePlugInInterfaceForService(
      client->hpm, plugin_type(), kIOCFPlugInInterfaceID, &client->plugin, &score);
  if (result != kIOReturnSuccess || !client->plugin) {
    hpm_close(client);
    return fail(error, size, "cannot open the AppleHPM plugin (0x%08x)",
                (unsigned)(result ? result : kIOReturnError));
  }
  HRESULT status = (*client->plugin)
                       ->QueryInterface(client->plugin, CFUUIDGetUUIDBytes(interface_type()),
                                        (LPVOID *)&client->interface);
  if (status != S_OK || !client->interface) {
    hpm_close(client);
    return fail(error, size, "cannot open the AppleHPM interface (0x%08x)", (unsigned)status);
  }
  *output = client;
  return 0;
}

void hpm_close(HpmClient *client) {
  if (!client) return;
  if (client->interface) (*client->interface)->Release(client->interface);
  if (client->plugin) IODestroyPlugInInterface(client->plugin);
  if (client->hpm) IOObjectRelease(client->hpm);
  if (client->port) IOObjectRelease(client->port);
  free(client);
}

static int transfer(HpmClient *client, uint16_t offset, const void *input, size_t input_length,
                    void *output, size_t output_length, size_t *received, char *error,
                    size_t size) {
  bool read = output != NULL;
  size_t length = read ? output_length : input_length;
  /* Reads: version, security, and diagnostic offsets. Writes: diagnostic only. */
  bool allowed =
      read ? offset == 0x2800 || offset == 0x3c00 || offset == 0x5000 : input && offset == 0x5000;
  if (!client || !allowed || length < 4 || length > 24 || length % 4)
    return fail(error, size, "unsupported cable request");
  if (!property_is_true(client->port, CFSTR("ConnectionActive")))
    return fail(error, size, "the MagSafe cable is disconnected");

  /* SOP' (cable) target, byte count, a flag set on every observed request,
   * the read flag, and the offset. */
  uint32_t command = ATOMIC_COMMAND;
  uint32_t extension = (UINT32_C(1) << 30) | ((uint32_t)length << 24) | (UINT32_C(1) << 18) |
                       (read ? UINT32_C(1) << 16 : 0) | offset;
  uint32_t response = 0;
  if (read) memset(output, 0, output_length);
  IOReturn result =
      (*client->interface)
          ->atomic(client->interface, 1, (unsigned long long)(uintptr_t)&command,
                   (unsigned long long)(uintptr_t)input, (unsigned long long)(uintptr_t)&extension,
                   (unsigned long long)(uintptr_t)output, (unsigned long long)(uintptr_t)&response,
                   (unsigned short)input_length, (unsigned short)output_length, REQUEST_TIMEOUT, 0,
                   0);
  if (result) return fail(error, size, "cable request failed (0x%08x)", (unsigned)result);
  if (response & (UINT32_C(1) << 23))
    return fail(error, size, "cable returned error %u (response 0x%08x)", (response >> 18) & 31u,
                response);
  /* Valid responses repeat the SOP' target, the read flag, and the offset. */
  if ((response >> 30) != 1 || (response & 0xffffu) != offset ||
      ((response >> 16) & 1u) != (unsigned)read)
    return fail(error, size, "unexpected cable response 0x%08x", response);
  if (read) {
    size_t count = (response >> 24) & 31u;
    if (count < 4 || count > output_length || count % 4)
      return fail(error, size, "unexpected cable response length %zu", count);
    *received = count;
  }
  return 0;
}

int hpm_read(HpmClient *client, uint16_t offset, void *output, size_t capacity, size_t *received,
             char *error, size_t size) {
  *received = 0;
  return transfer(client, offset, NULL, 0, output, capacity, received, error, size);
}

int hpm_write(HpmClient *client, uint16_t offset, const void *input, size_t length, char *error,
              size_t size) {
  return transfer(client, offset, input, length, NULL, 0, NULL, error, size);
}
