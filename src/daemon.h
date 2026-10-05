// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#ifndef MAGSAFE_DAEMON_H
#define MAGSAFE_DAEMON_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>

/* The magsafe daemon: a root LaunchDaemon that runs magsafe commands for
 * allowed users without sudo, and applies the saved settings again at boot,
 * on plug-in, and on wake. Each command runs as a child of the daemon with
 * the client's standard input, output, and error, so it behaves as if run
 * through sudo. See protocol.h for the messages. */

#define DAEMON_LABEL "com.mpopv.magsafe"
#define DAEMON_HELPER "/Library/PrivilegedHelperTools/" DAEMON_LABEL
#define DAEMON_PLIST "/Library/LaunchDaemons/" DAEMON_LABEL ".plist"
#define DAEMON_LOG "/Library/Logs/magsafe-daemon.log"
#define DAEMON_SOCKET "/var/run/magsafe.sock"
#define DAEMON_CHILD_ENV "MAGSAFE_DAEMON_CHILD" /* set for commands the daemon runs */

/* The socket. A process that is not root may name another one with
 * MAGSAFE_SOCKET, to run or reach a test daemon. */
const char *daemon_socket_path(void);

typedef struct {
  /* Whether the daemon runs argv. Writes a reason when it does not. */
  bool (*allow)(int argc, char **argv, char *message, size_t size);
  /* Called every few seconds, and at once on start. */
  void (*maintain)(void);
} DaemonHooks;

/* Serve requests from clients of the same version, running commands with
 * executable, until terminated. Returns only if the daemon cannot start. */
int daemon_serve(const char *executable, const char *version, const DaemonHooks *hooks, char *error,
                 size_t size);

/* Write a timestamped line to the daemon's log, its standard error. */
void daemon_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define DAEMON_UNAVAILABLE 1 /* daemon_request: no usable daemon; run the command directly */

/* Run argv through the daemon, passing on stop signals, and set *exit_code.
 * Returns 0, DAEMON_UNAVAILABLE (with a reason in error when the user should
 * know it), or -1 if the connection failed while the command ran. */
int daemon_request(int argc, char **argv, const char *version, int *exit_code, char *error,
                   size_t size);

/* Ask a running daemon for its version. */
int daemon_ping(char version[DAEMON_VERSION_MAX], char *error, size_t size);

/* Copy executable to DAEMON_HELPER, write DAEMON_PLIST, and start the daemon.
 * Requires root. */
int daemon_install(const char *executable, const char *version, char *error, size_t size);
/* Stop the daemon and remove what daemon_install added. *removed is false if
 * nothing was installed. Requires root. */
int daemon_uninstall(bool *removed, char *error, size_t size);
bool daemon_installed(void);

#endif
