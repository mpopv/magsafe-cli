// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "protocol.h"
#include "error.h"
#include <string.h>

/* Copy text into a fixed field, always NUL-terminated. */
static void copy(char *field, size_t capacity, const char *text) {
  size_t length = strnlen(text, capacity - 1);
  memcpy(field, text, length);
  memset(field + length, 0, capacity - length);
}

int protocol_request(DaemonRequest *request, const char *version, int argc, char *const *argv,
                     char *bytes, size_t capacity, char *error, size_t size) {
  size_t used = 0;
  if (argc < 0 || (unsigned)argc > DAEMON_ARGS_MAX)
    return fail(error, size, "too many arguments for the daemon");
  for (int i = 0; i < argc; ++i) {
    size_t length = strlen(argv[i]) + 1;
    if (used + length > capacity || used + length > DAEMON_BYTES_MAX)
      return fail(error, size, "arguments are too long for the daemon");
    memcpy(bytes + used, argv[i], length);
    used += length;
  }
  memset(request, 0, sizeof(*request));
  request->magic = DAEMON_MAGIC;
  request->protocol = DAEMON_PROTOCOL;
  copy(request->version, sizeof(request->version), version);
  request->argc = (uint32_t)argc;
  request->length = (uint32_t)used;
  return 0;
}

int protocol_check(const DaemonRequest *request, char *error, size_t size) {
  if (request->magic != DAEMON_MAGIC) return fail(error, size, "not a magsafe request");
  if (request->protocol != DAEMON_PROTOCOL)
    return fail(error, size, "unsupported protocol %u", request->protocol);
  if (request->argc > DAEMON_ARGS_MAX || request->length > DAEMON_BYTES_MAX ||
      (request->argc == 0) != (request->length == 0))
    return fail(error, size, "request too large or malformed");
  if (!memchr(request->version, '\0', sizeof(request->version)))
    return fail(error, size, "malformed version");
  return 0;
}

int protocol_arguments(const DaemonRequest *request, char *bytes, char **argv, char *error,
                       size_t size) {
  size_t at = 0;
  for (uint32_t i = 0; i < request->argc; ++i) {
    char *end = at < request->length ? memchr(bytes + at, '\0', request->length - at) : NULL;
    if (!end) return fail(error, size, "malformed arguments");
    argv[i] = bytes + at;
    at = (size_t)(end - bytes) + 1;
  }
  if (at != request->length) return fail(error, size, "malformed arguments");
  argv[request->argc] = NULL;
  return 0;
}

int protocol_compare_versions(const char *a, const char *b) {
  while (*a || *b) {
    unsigned long x = 0, y = 0;
    for (; *a >= '0' && *a <= '9'; ++a) x = x * 10 + (unsigned long)(*a - '0');
    for (; *b >= '0' && *b <= '9'; ++b) y = y * 10 + (unsigned long)(*b - '0');
    if (x != y) return x < y ? -1 : 1;
    while (*a && *a != '.') ++a;
    while (*b && *b != '.') ++b;
    if (*a) ++a;
    if (*b) ++b;
  }
  return 0;
}

void protocol_reply(DaemonReply *reply, const char *version, DaemonStatus status,
                    const char *message) {
  memset(reply, 0, sizeof(*reply));
  reply->magic = DAEMON_MAGIC;
  reply->protocol = DAEMON_PROTOCOL;
  copy(reply->version, sizeof(reply->version), version);
  reply->status = (int32_t)status;
  copy(reply->message, sizeof(reply->message), message ? message : "");
}

int protocol_check_reply(const DaemonReply *reply, char *error, size_t size) {
  if (reply->magic != DAEMON_MAGIC || reply->protocol != DAEMON_PROTOCOL)
    return fail(error, size, "unexpected reply from the magsafe daemon");
  if (!memchr(reply->version, '\0', sizeof(reply->version)) ||
      !memchr(reply->message, '\0', sizeof(reply->message)) || reply->status < DAEMON_OK ||
      reply->status > DAEMON_FAILED)
    return fail(error, size, "malformed reply from the magsafe daemon");
  return 0;
}
