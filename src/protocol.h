// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_PROTOCOL_H
#define MAGSAFE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Messages between magsafe and its daemon over a local socket:
 *
 * 1. The client sends a DaemonRequest, then the arguments of a magsafe
 *    command line as NUL-terminated strings. With arguments, the first
 *    message also carries the client's standard input, output, and error.
 *    A request without arguments asks only for the daemon's version.
 * 2. The daemon answers with a DaemonReply. Unless it is DAEMON_OK, the
 *    daemon closes the connection.
 * 3. While the command runs, each byte from the client is a signal number
 *    to pass on: SIGINT, SIGTERM, or SIGHUP.
 * 4. When the command ends, the daemon sends its exit code as an int32_t. */

#define DAEMON_MAGIC UINT32_C(0x4653474d) /* "MGSF" */
#define DAEMON_PROTOCOL 1u
#define DAEMON_VERSION_MAX 16u
#define DAEMON_ARGS_MAX 80u
#define DAEMON_BYTES_MAX 16384u
#define DAEMON_MESSAGE_MAX 200u

typedef struct {
  uint32_t magic, protocol;
  char version[DAEMON_VERSION_MAX]; /* the client's magsafe version */
  uint32_t argc, length;            /* arguments, and the bytes they take */
} DaemonRequest;

typedef enum {
  DAEMON_OK,       /* the command runs */
  DAEMON_MISMATCH, /* the daemon is another magsafe version */
  DAEMON_DENIED,   /* the user may not use the daemon */
  DAEMON_REFUSED,  /* the daemon does not run this command */
  DAEMON_FAILED,   /* the daemon could not start the command */
} DaemonStatus;

typedef struct {
  uint32_t magic, protocol;
  char version[DAEMON_VERSION_MAX]; /* the daemon's magsafe version */
  int32_t status;                   /* a DaemonStatus */
  char message[DAEMON_MESSAGE_MAX];
} DaemonReply;

/* Build a request for argv, writing the arguments to bytes. */
int protocol_request(DaemonRequest *request, const char *version, int argc, char *const *argv,
                     char *bytes, size_t capacity, char *error, size_t size);

/* Check a received request's header before reading its arguments. */
int protocol_check(const DaemonRequest *request, char *error, size_t size);

/* Split a request's argument bytes into argv, which holds DAEMON_ARGS_MAX + 1
 * pointers and ends with a null one. */
int protocol_arguments(const DaemonRequest *request, char *bytes, char **argv, char *error,
                       size_t size);

/* Compare versions such as "0.11.0" part by part: negative, zero, or positive
 * as a is older than, the same as, or newer than b. A missing or non-numeric
 * part counts as 0. */
int protocol_compare_versions(const char *a, const char *b);

void protocol_reply(DaemonReply *reply, const char *version, DaemonStatus status,
                    const char *message);
int protocol_check_reply(const DaemonReply *reply, char *error, size_t size);

#endif
