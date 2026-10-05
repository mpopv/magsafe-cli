// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matt Popovich

#include "daemon.h"
#include "error.h"
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <membership.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_JOBS 16
#define MAINTAIN_S 2
#define TIMEOUT_S 5 /* for a request or a reply */
#define START_TIMEOUT_MS 5000u

const char *daemon_socket_path(void) {
  const char *path = getenv("MAGSAFE_SOCKET");
  return path && *path && getuid() != 0 && geteuid() != 0 ? path : DAEMON_SOCKET;
}

void daemon_log(const char *format, ...) {
  char when[32];
  time_t now = time(NULL);
  strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
  fprintf(stderr, "%s magsafe daemon: ", when);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

static int read_all(int fd, void *buffer, size_t length) {
  for (size_t done = 0; done < length;) {
    ssize_t count = read(fd, (char *)buffer + done, length - done);
    if (count > 0) done += (size_t)count;
    else if (count < 0 && errno == EINTR) continue;
    else return -1;
  }
  return 0;
}

static int write_all(int fd, const void *buffer, size_t length) {
  for (size_t done = 0; done < length;) {
    ssize_t count = write(fd, (const char *)buffer + done, length - done);
    if (count > 0) done += (size_t)count;
    else if (count < 0 && errno == EINTR) continue;
    else return -1;
  }
  return 0;
}

static void set_timeout(int fd, int seconds) {
  struct timeval timeout = {seconds, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

/* Server */

typedef struct {
  int socket;
  pid_t pid;
  dispatch_source_t reader, exit;
  int sources; /* sources not yet cancelled */
  bool finished;
} Job;

static struct {
  const char *executable, *version, *socket;
  const DaemonHooks *hooks;
  Job *jobs[MAX_JOBS];
} server;

/* Root, the daemon's own user, the user at the Mac's screen, and admins. */
static bool allowed(uid_t uid) {
  if (uid == 0 || uid == geteuid()) return true;
  uid_t console = 0;
  CFStringRef name = SCDynamicStoreCopyConsoleUser(NULL, &console, NULL);
  if (name) {
    CFRelease(name);
    if (console == uid) return true;
  }
  struct group *admin = getgrnam("admin");
  uuid_t user, group;
  int member = 0;
  return admin && !mbr_uid_to_uuid(uid, user) && !mbr_gid_to_uuid(admin->gr_gid, group) &&
         !mbr_check_membership(user, group, &member) && member;
}

/* Receive a request header and the file descriptors sent with it. */
static int receive(int client, DaemonRequest *request, int fds[3], int *count) {
  char control[CMSG_SPACE(4 * sizeof(int))];
  *count = 0;
  for (size_t done = 0; done < sizeof(*request);) {
    struct iovec part = {(char *)request + done, sizeof(*request) - done};
    struct msghdr message = {.msg_iov = &part, .msg_iovlen = 1};
    if (!done) {
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
    }
    ssize_t received = recvmsg(client, &message, 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return -1;
    for (struct cmsghdr *c = done ? NULL : CMSG_FIRSTHDR(&message); c;
         c = CMSG_NXTHDR(&message, c)) {
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
      int *passed = (int *)(void *)CMSG_DATA(c);
      for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); ++i) {
        if (*count < 3) {
          fcntl(passed[i], F_SETFD, FD_CLOEXEC);
          fds[(*count)++] = passed[i];
        } else {
          close(passed[i]);
        }
      }
    }
    if (message.msg_flags & MSG_CTRUNC) return -1;
    done += (size_t)received;
  }
  return 0;
}

/* Run the command as a child in its own process group, with the client's
 * standard streams and a minimal environment. */
static pid_t spawn_command(char **arguments, const int fds[3], char *error, size_t size) {
  char *argv[DAEMON_ARGS_MAX + 2] = {(char *)server.executable};
  for (int i = 0; arguments[i]; ++i) argv[i + 1] = arguments[i];
  char *environment[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", DAEMON_CHILD_ENV "=1", NULL};
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  sigset_t all, none;
  sigfillset(&all);
  sigemptyset(&none);
  posix_spawn_file_actions_init(&actions);
  for (int i = 0; i < 3; ++i) posix_spawn_file_actions_adddup2(&actions, fds[i], i);
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setsigdefault(&attributes, &all);
  posix_spawnattr_setsigmask(&attributes, &none);
  posix_spawnattr_setpgroup(&attributes, 0);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF |
                                            POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
  pid_t pid = 0;
  int result = posix_spawn(&pid, server.executable, &actions, &attributes, argv, environment);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (result) return fail(error, size, "cannot start the command: %s", strerror(result));
  return pid;
}

static void release(Job *job) {
  if (--job->sources) return;
  close(job->socket);
  for (int i = 0; i < MAX_JOBS; ++i)
    if (server.jobs[i] == job) server.jobs[i] = NULL;
  free(job);
}

/* Send the exit code once the command has ended. */
static void finish(Job *job) {
  int status;
  if (job->finished || waitpid(job->pid, &status, WNOHANG) != job->pid) return;
  job->finished = true;
  int32_t code = WIFEXITED(status)     ? WEXITSTATUS(status)
                 : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                       : 1;
  write_all(job->socket, &code, sizeof(code));
  dispatch_source_cancel(job->reader);
  dispatch_source_cancel(job->exit);
}

/* Pass on stop signals. A closed connection stops the command too. */
static void listen_to(Job *job) {
  char bytes[16];
  ssize_t count = read(job->socket, bytes, sizeof(bytes));
  if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
  for (ssize_t i = 0; i < count && !job->finished; ++i)
    if (bytes[i] == SIGINT || bytes[i] == SIGTERM || bytes[i] == SIGHUP) kill(job->pid, bytes[i]);
  if (count <= 0) {
    if (!job->finished) kill(job->pid, SIGTERM);
    dispatch_source_cancel(job->reader);
  }
}

static void start_job(int client, pid_t pid) {
  Job *job = calloc(1, sizeof(*job));
  int slot = 0;
  while (slot < MAX_JOBS && server.jobs[slot]) ++slot;
  if (!job || slot == MAX_JOBS) { /* cannot happen: the caller checked */
    kill(pid, SIGTERM);
    close(client);
    free(job);
    return;
  }
  server.jobs[slot] = job;
  *job = (Job){.socket = client, .pid = pid, .sources = 2};
  fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK);
  dispatch_queue_t queue = dispatch_get_main_queue();
  job->reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)client, 0, queue);
  job->exit =
      dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT, queue);
  dispatch_source_set_event_handler(job->reader, ^{
    listen_to(job);
  });
  dispatch_source_set_event_handler(job->exit, ^{
    finish(job);
  });
  dispatch_source_set_cancel_handler(job->reader, ^{
    release(job);
  });
  dispatch_source_set_cancel_handler(job->exit, ^{
    release(job);
  });
  dispatch_resume(job->reader);
  dispatch_resume(job->exit);
  /* The command may have ended before the exit source was ready. */
  dispatch_async(queue, ^{
    finish(job);
  });
}

static int running_jobs(void) {
  int count = 0;
  for (int i = 0; i < MAX_JOBS; ++i) count += server.jobs[i] != NULL;
  return count;
}

static void serve(int client) {
  DaemonRequest request;
  DaemonReply reply;
  int fds[3], count = 0;
  uid_t uid;
  gid_t gid;
  char message[DAEMON_MESSAGE_MAX] = "";
  int one = 1;
  fcntl(client, F_SETFD, FD_CLOEXEC);
  setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  set_timeout(client, TIMEOUT_S);
  if (getpeereid(client, &uid, &gid) || receive(client, &request, fds, &count) ||
      protocol_check(&request, NULL, 0)) {
    for (int i = 0; i < count; ++i) close(fds[i]);
    close(client);
    return;
  }

  DaemonStatus status = DAEMON_OK;
  pid_t pid = -1;
  char bytes[DAEMON_BYTES_MAX], *arguments[DAEMON_ARGS_MAX + 1];
  if (request.argc == 0) {
    /* A version check. */
  } else if (!allowed(uid)) {
    status = DAEMON_DENIED;
    snprintf(message, sizeof(message), "the magsafe daemon does not serve user %u", uid);
  } else if (strcmp(request.version, server.version)) {
    status = DAEMON_MISMATCH;
  } else if (count != 3) {
    status = DAEMON_FAILED;
    snprintf(message, sizeof(message), "the request had no standard streams");
  } else if (read_all(client, bytes, request.length) ||
             protocol_arguments(&request, bytes, arguments, message, sizeof(message))) {
    status = DAEMON_FAILED;
  } else if (!server.hooks->allow((int)request.argc, arguments, message, sizeof(message))) {
    status = DAEMON_REFUSED;
  } else if (running_jobs() == MAX_JOBS) {
    status = DAEMON_FAILED;
    snprintf(message, sizeof(message), "too many magsafe commands are running");
  } else if ((pid = spawn_command(arguments, fds, message, sizeof(message))) < 0) {
    status = DAEMON_FAILED;
  }
  for (int i = 0; i < count; ++i) close(fds[i]);
  if (status != DAEMON_OK && request.argc)
    daemon_log("refused a request from user %u: %s", uid, *message ? message : "version mismatch");

  protocol_reply(&reply, server.version, status, message);
  bool sent = write_all(client, &reply, sizeof(reply)) == 0;
  if (pid > 0 && sent) {
    set_timeout(client, 0);
    start_job(client, pid);
    return;
  }
  if (pid > 0) kill(pid, SIGTERM);
  close(client);
}

/* On SIGTERM, as from launchctl, stop running commands, which reset the
 * light, and exit. */
static void stop(void) {
  for (int i = 0; i < MAX_JOBS; ++i)
    if (server.jobs[i] && !server.jobs[i]->finished) kill(server.jobs[i]->pid, SIGTERM);
  unlink(server.socket);
  daemon_log("stopped");
  exit(0);
}

int daemon_serve(const char *executable, const char *version, const DaemonHooks *hooks, char *error,
                 size_t size) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  const char *path = daemon_socket_path();
  if (strlen(path) >= sizeof(address.sun_path))
    return fail(error, size, "socket path %s is too long", path);
  strlcpy(address.sun_path, path, sizeof(address.sun_path));
  server.executable = executable;
  server.version = version;
  server.socket = path;
  server.hooks = hooks;

  /* One daemon at a time. */
  char lock_path[sizeof(address.sun_path) + 8];
  snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
  int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock < 0) return fail(error, size, "cannot open %s: %s", lock_path, strerror(errno));
  if (flock(lock, LOCK_EX | LOCK_NB))
    return fail(error, size, "the magsafe daemon is already running");

  struct stat st;
  if (!lstat(path, &st)) {
    if (!S_ISSOCK(st.st_mode)) return fail(error, size, "%s is not a socket", path);
    unlink(path);
  }
  int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
      chmod(path, 0666) || listen(listener, 16))
    return fail(error, size, "cannot listen on %s: %s", path, strerror(errno));
  fcntl(listener, F_SETFD, FD_CLOEXEC);
  fcntl(listener, F_SETFL, O_NONBLOCK);
  if (chdir("/")) return fail(error, size, "cannot change to /: %s", strerror(errno));
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  signal(SIGINT, SIG_IGN);

  dispatch_queue_t queue = dispatch_get_main_queue();
  dispatch_source_t accepting =
      dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)listener, 0, queue);
  dispatch_source_set_event_handler(accepting, ^{
    for (int client; (client = accept(listener, NULL, NULL)) >= 0;) serve(client);
  });
  dispatch_resume(accepting);
  dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
  dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), MAINTAIN_S * NSEC_PER_SEC,
                            NSEC_PER_SEC / 2);
  dispatch_source_set_event_handler(timer, ^{
    hooks->maintain();
  });
  dispatch_resume(timer);
  int stops[] = {SIGTERM, SIGINT};
  for (size_t i = 0; i < sizeof(stops) / sizeof(*stops); ++i) {
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)stops[i], 0, queue);
    dispatch_source_set_event_handler(source, ^{
      stop();
    });
    dispatch_resume(source);
  }
  daemon_log("version %s serving %s", version, path);
  dispatch_main();
}

/* Client */

static volatile sig_atomic_t client_socket = -1;

static void forward(int signal) {
  int fd = client_socket;
  char byte = (char)signal;
  if (fd >= 0) (void)!write(fd, &byte, 1);
}

static int connect_daemon(void) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  const char *path = daemon_socket_path();
  if (strlen(path) >= sizeof(address.sun_path)) return -1;
  strlcpy(address.sun_path, path, sizeof(address.sun_path));
  int fd = socket(AF_UNIX, SOCK_STREAM, 0), one = 1;
  if (fd < 0) return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
    close(fd);
    return -1;
  }
  set_timeout(fd, TIMEOUT_S);
  return fd;
}

/* Send a request, with the standard streams when there are arguments, and
 * read the reply. */
static int ask(int fd, const DaemonRequest *request, const char *bytes, DaemonReply *reply,
               char *error, size_t size) {
  int streams[3] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
  char control[CMSG_SPACE(sizeof(streams))];
  struct iovec part = {(void *)request, sizeof(*request)};
  struct msghdr message = {.msg_iov = &part, .msg_iovlen = 1};
  if (request->argc) {
    memset(control, 0, sizeof(control));
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(streams));
    memcpy(CMSG_DATA(c), streams, sizeof(streams));
  }
  if (sendmsg(fd, &message, 0) != (ssize_t)sizeof(*request) ||
      write_all(fd, bytes, request->length) || read_all(fd, reply, sizeof(*reply)))
    return fail(error, size, "the magsafe daemon did not answer");
  return protocol_check_reply(reply, error, size);
}

int daemon_request(int argc, char **argv, const char *version, int *exit_code, char *error,
                   size_t size) {
  DaemonRequest request;
  DaemonReply reply;
  char bytes[DAEMON_BYTES_MAX];
  if (protocol_request(&request, version, argc, argv, bytes, sizeof(bytes), NULL, 0))
    return DAEMON_UNAVAILABLE;
  int fd = connect_daemon();
  if (fd < 0) return DAEMON_UNAVAILABLE;
  if (ask(fd, &request, bytes, &reply, error, size)) {
    close(fd);
    return DAEMON_UNAVAILABLE;
  }
  if (reply.status != DAEMON_OK) {
    close(fd);
    if (reply.status == DAEMON_MISMATCH) {
      fail(error, size,
           "the magsafe daemon is version %s; run 'sudo magsafe daemon install' to update it",
           reply.version);
      return DAEMON_UNAVAILABLE;
    }
    if (reply.status == DAEMON_DENIED) {
      fail(error, size, "%s", reply.message);
      return DAEMON_UNAVAILABLE;
    }
    return fail(error, size, "%s", *reply.message ? reply.message : "the magsafe daemon failed");
  }

  /* The command runs. Pass on stop signals until it ends. */
  struct sigaction action = {.sa_handler = forward};
  sigemptyset(&action.sa_mask);
  set_timeout(fd, 0);
  client_socket = fd;
  signal(SIGPIPE, SIG_IGN);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGHUP, &action, NULL);
  int32_t code;
  int result = read_all(fd, &code, sizeof(code));
  client_socket = -1;
  close(fd);
  if (result) return fail(error, size, "lost the connection to the magsafe daemon");
  *exit_code = code;
  return 0;
}

int daemon_ping(char version[DAEMON_VERSION_MAX], char *error, size_t size) {
  DaemonRequest request;
  DaemonReply reply;
  protocol_request(&request, "", 0, NULL, NULL, 0, NULL, 0);
  int fd = connect_daemon();
  if (fd < 0) return fail(error, size, "the magsafe daemon is not running");
  int result = ask(fd, &request, NULL, &reply, error, size);
  close(fd);
  if (result) return -1;
  memcpy(version, reply.version, DAEMON_VERSION_MAX);
  return 0;
}

/* Installation */

static const char plist[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                            "<plist version=\"1.0\">\n"
                            "<dict>\n"
                            "  <key>Label</key>\n"
                            "  <string>" DAEMON_LABEL "</string>\n"
                            "  <key>ProgramArguments</key>\n"
                            "  <array>\n"
                            "    <string>" DAEMON_HELPER "</string>\n"
                            "    <string>daemon</string>\n"
                            "    <string>run</string>\n"
                            "  </array>\n"
                            "  <key>RunAtLoad</key>\n"
                            "  <true/>\n"
                            "  <key>KeepAlive</key>\n"
                            "  <true/>\n"
                            "  <key>StandardErrorPath</key>\n"
                            "  <string>" DAEMON_LOG "</string>\n"
                            "</dict>\n"
                            "</plist>\n";

/* Run launchctl with fixed arguments and return its exit status. A null
 * path leaves it out. */
static int launchctl(const char *verb, const char *domain, const char *path) {
  char *argv[] = {"/bin/launchctl", (char *)verb, (char *)domain, (char *)path, NULL};
  char *environment[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", NULL};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t pid;
  int result = posix_spawn(&pid, argv[0], &actions, NULL, argv, environment);
  posix_spawn_file_actions_destroy(&actions);
  int status;
  if (result) return -1;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* A directory that only root can change. */
static int check_directory(const char *path, char *error, size_t size) {
  struct stat st;
  if (mkdir(path, 0755) && errno != EEXIST)
    return fail(error, size, "cannot create %s: %s", path, strerror(errno));
  if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
      (st.st_mode & (S_IWGRP | S_IWOTH)))
    return fail(error, size, "%s must be a directory that only root can change", path);
  return 0;
}

/* Write a root-owned file in one step: to a temporary file, then renamed. */
static int write_file(const char *path, const char *data, size_t length, int input, mode_t mode,
                      char *error, size_t size) {
  char temporary[1024];
  snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
  int fd = mkstemp(temporary);
  if (fd < 0) return fail(error, size, "cannot write %s: %s", path, strerror(errno));
  bool written = true;
  if (data) {
    written = write_all(fd, data, length) == 0;
  } else {
    char buffer[65536];
    for (ssize_t count; written && (count = read(input, buffer, sizeof(buffer))) != 0;)
      written = count > 0 && write_all(fd, buffer, (size_t)count) == 0;
  }
  written = written && fchown(fd, 0, 0) == 0 && fchmod(fd, mode) == 0 && fsync(fd) == 0;
  int saved = errno;
  if (close(fd) || !written || rename(temporary, path)) {
    if (written) saved = errno;
    unlink(temporary);
    return fail(error, size, "cannot write %s: %s", path, strerror(saved));
  }
  return 0;
}

int daemon_install(const char *executable, const char *version, char *error, size_t size) {
  if (geteuid() != 0) return fail(error, size, "installing the daemon requires root");
  if (check_directory("/Library/PrivilegedHelperTools", error, size) ||
      check_directory("/Library/LaunchDaemons", error, size))
    return -1;
  /* A root copy that a user can't replace, as one could under Homebrew. */
  if (strcmp(executable, DAEMON_HELPER)) {
    int input = open(executable, O_RDONLY | O_CLOEXEC);
    if (input < 0) return fail(error, size, "cannot read %s: %s", executable, strerror(errno));
    int result = write_file(DAEMON_HELPER, NULL, 0, input, 0755, error, size);
    close(input);
    if (result) return -1;
  }
  if (write_file(DAEMON_PLIST, plist, strlen(plist), -1, 0644, error, size)) return -1;

  /* Replace a running daemon, which may take a moment to stop. */
  launchctl("bootout", "system/" DAEMON_LABEL, NULL);
  int started = -1;
  for (int attempt = 0; attempt < 20 && started; ++attempt) {
    if (attempt) usleep(250000);
    started = launchctl("bootstrap", "system", DAEMON_PLIST);
  }
  if (started) return fail(error, size, "launchctl could not start the daemon");
  char running[DAEMON_VERSION_MAX] = "";
  for (unsigned waited = 0; waited < START_TIMEOUT_MS; waited += 100) {
    if (!daemon_ping(running, NULL, 0) && !strcmp(running, version)) return 0;
    usleep(100000);
  }
  return fail(error, size, "the daemon did not start; see %s", DAEMON_LOG);
}

int daemon_uninstall(bool *removed, char *error, size_t size) {
  if (geteuid() != 0) return fail(error, size, "removing the daemon requires root");
  *removed = daemon_installed() || access(DAEMON_HELPER, F_OK) == 0;
  launchctl("bootout", "system/" DAEMON_LABEL, NULL);
  if ((unlink(DAEMON_PLIST) && errno != ENOENT) || (unlink(DAEMON_HELPER) && errno != ENOENT))
    return fail(error, size, "cannot remove the daemon: %s", strerror(errno));
  unlink(DAEMON_SOCKET);
  return 0;
}

bool daemon_installed(void) { return access(DAEMON_PLIST, F_OK) == 0; }
