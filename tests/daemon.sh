#!/bin/sh
# End-to-end tests of the daemon, run as the current user on a private socket,
# so they need no cable or sudo. A command that the daemon runs fails at the
# root-only lock file, and its error comes back through the client: that shows
# that the command ran in the daemon, with the client's standard streams.
# Usage: tests/daemon.sh [path/to/magsafe]

bin=${1:-build/magsafe}
failures=0
tests=0
dir=$(mktemp -d /tmp/magsafe-daemon.XXXXXX)
MAGSAFE_SOCKET="$dir/sock"
export MAGSAFE_SOCKET

"$bin" daemon run 2>"$dir/log" &
daemon=$!
cleanup() {
  kill "$daemon" 2>/dev/null
  wait "$daemon" 2>/dev/null
  rm -f "$dir/log" "$dir/sock" "$dir/sock.lock"
  rmdir "$dir"
}
trap cleanup EXIT
tries=0
while [ ! -S "$MAGSAFE_SOCKET" ] && [ "$tries" -lt 50 ]; do
  sleep 0.1
  tries=$((tries + 1))
done

# expect STATUS OUTPUT ARGS...: compare the exit status and standard output.
expect() {
  want_status=$1 want_output=$2
  shift 2
  tests=$((tests + 1))
  output=$("$bin" "$@" 2>/dev/null)
  status=$?
  # shellcheck disable=SC2254
  case $output in
    $want_output) [ "$status" -eq "$want_status" ] && return ;;
  esac
  failures=$((failures + 1))
  printf 'FAIL: magsafe %s\n  expected (exit %s): %s\n  actual   (exit %s): %s\n' \
    "$*" "$want_status" "$want_output" "$status" "$output"
}

expect 0 '{"ok":true,"command":"daemon status",*"running":true,"daemon_version":"*","cli_version":"*","current":true,*}' \
  --json daemon status
expect 1 '{"ok":false,"error":"cannot open /var/run/magsafe-cli.lock: *"}' --json status
expect 1 '{"ok":false,"error":"cannot open /var/run/magsafe-cli.lock: *"}' --json settings dim 50
expect 1 '' status

# Stopping the daemon removes its socket.
kill "$daemon"
wait "$daemon" 2>/dev/null
tests=$((tests + 1))
if [ -e "$MAGSAFE_SOCKET" ]; then
  failures=$((failures + 1))
  echo "FAIL: the stopped daemon left its socket"
fi
expect 0 '{"ok":true,"command":"daemon status",*"running":false,*}' --json daemon status

if [ "$failures" -eq 0 ]; then
  echo "All $tests daemon tests passed."
else
  echo "$failures of $tests daemon tests failed."
  sed 's/^/  log: /' "$dir/log"
  exit 1
fi
