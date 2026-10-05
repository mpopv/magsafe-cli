#!/bin/sh
# Command-line tests that need no cable and no administrator access.
# Hardware commands always use --dry-run, so nothing reaches sudo or the device.
# Usage: tests/cli.sh [path/to/magsafe]

bin=${1:-build/magsafe}
failures=0
tests=0

# expect STATUS OUTPUT ARGS...: compare the exit status and standard output.
# A trailing * in OUTPUT matches any remaining text.
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

# expect_error MESSAGE ARGS...: a usage error with the given JSON message.
expect_error() {
  message=$1
  shift
  expect 2 "{\"ok\":false,\"error\":\"$message\"}" --json --dry-run "$@"
}

# Help, version, and capabilities
expect 0 'magsafe [0-9]* - control the light*'
expect 0 'magsafe [0-9]* - control the light*' --help
expect 0 'magsafe [0-9]* - control the light*' -h
expect 0 'magsafe [0-9]* - control the light*' help
expect 0 'magsafe [0-9]* - control the light*' status --help
expect 0 'magsafe [0-9]*.[0-9]*.[0-9]*' --version
expect 0 'magsafe [0-9]*.[0-9]*.[0-9]*' version
expect 0 '{"ok":true,"cli_version":"*","diagnostic_firmware":"3.2.0","supported":*"blink"*"unavailable":*"firmware-flash"*}' \
  --json capabilities

# Dry runs
dry='"ok":true,"command"'
expect 0 "{$dry:\"status\",\"dry_run\":true,\"device_calls\":0}" --json -n status
expect 0 "{$dry:\"reset\",\"dry_run\":true,\"device_calls\":0}" --json -n reset
expect 0 "{$dry:\"firmware calibration\",\"dry_run\":true,\"device_calls\":0}" \
  --json -n firmware calibration
expect 0 "{$dry:\"led set\",\"dry_run\":true,\"device_calls\":0,\"color\":\"off\"}" \
  --json -n led set off
expect 0 "{$dry:\"led brightness\",\"dry_run\":true,\"device_calls\":0,\"color\":\"amber\",\"percent\":40}" \
  --json -n led brightness amber 40
expect 0 "{$dry:\"led blink\",\"dry_run\":true,\"device_calls\":0,\"color\":\"green\",\"count\":\"infinite\",\"interval_ms\":500,\"preparation_ms\":450,\"duration_ms\":null}" \
  --json -n led blink green
expect 0 "{$dry:\"led blink\",\"dry_run\":true,\"device_calls\":0,\"color\":\"green\",\"count\":4,\"interval_ms\":500,\"preparation_ms\":450,\"duration_ms\":4450}" \
  --json -n led blink green -c 4
expect 0 "{$dry:\"led blink\",\"dry_run\":true,\"device_calls\":0,\"color\":\"alternate\",\"count\":6,\"interval_ms\":250,\"preparation_ms\":2700,\"duration_ms\":5700}" \
  --json -n led blink alternate -c 6 -i 250
expect 0 "{$dry:\"led blink\",\"dry_run\":true,\"device_calls\":0,\"color\":\"alternate\",\"count\":\"infinite\",\"interval_ms\":500,\"preparation_ms\":null,\"duration_ms\":null}" \
  --json -n led blink alternate --count infinite
expect 0 "{$dry:\"led fade-in\",\"dry_run\":true,\"device_calls\":0,\"color\":\"amber\",\"count\":4,\"interval_ms\":500,\"fade_ms\":2000,\"preparation_ms\":450,\"duration_ms\":10450}" \
  --json -n led fade-in amber --count 4 --duration-ms 2000
expect 0 "{$dry:\"led fade-out\",\"dry_run\":true,\"device_calls\":0,\"color\":\"green\",\"count\":2,\"interval_ms\":500,\"fade_ms\":1000,\"preparation_ms\":450,\"duration_ms\":3450}" \
  --json --count=2 -n led fade-out green
expect 0 "{$dry:\"led fade\",\"dry_run\":true,\"device_calls\":0,\"color\":\"amber\",\"count\":4,\"interval_ms\":500,\"fade_ms\":2000,\"preparation_ms\":450,\"duration_ms\":18450}" \
  led fade amber -c 4 -d 2000 --json -n
expect 0 "{$dry:\"led fade\",\"dry_run\":true,\"device_calls\":0,\"color\":\"green\",\"count\":\"infinite\",\"interval_ms\":500,\"fade_ms\":1000,\"preparation_ms\":450,\"duration_ms\":null}" \
  --json -n led fade green -c infinite
expect 0 'dry_run:      true
device_calls: 0
color:        amber
percent:      40' -n led brightness amber 40
expect 0 "{$dry:\"led stream\",\"dry_run\":true,\"device_calls\":0,\"color\":\"green\",\"max_rate_hz\":40,\"preparation_ms\":450,\"duration_ms\":null}" \
  --json -n led stream green
expect 0 "{$dry:\"visualizer\",\"dry_run\":true,\"device_calls\":0,\"color\":\"green\",\"preview\":false,\"frame_rate_hz\":30,\"preparation_ms\":450,\"duration_ms\":null}" \
  --json -n visualizer
expect 0 "{$dry:\"visualizer\",\"dry_run\":true,\"device_calls\":0,\"color\":\"amber\",\"preview\":true,\"frame_rate_hz\":30}" \
  --json -n visualizer amber --preview
expect 0 '{"ok":true,*"supported":*"brightness-stream","visualizer","timer"*"unavailable":*}' --json capabilities
expect 0 "{$dry:\"timer\",\"dry_run\":true,\"device_calls\":0,\"timer_ms\":1500000,\"warning_ms\":300000,\"count\":\"infinite\",\"interval_ms\":500,\"preparation_ms\":450,\"duration_ms\":null}" \
  --json -n timer 25m
expect 0 "{$dry:\"timer\",\"dry_run\":true,\"device_calls\":0,\"timer_ms\":5400000,\"warning_ms\":300000,\"count\":10,\"interval_ms\":250,\"preparation_ms\":450,\"duration_ms\":5405000}" \
  --json -n timer 1h30m -c 10 -i 250
expect 0 "{$dry:\"timer\",*\"timer_ms\":2700000,*}" --json -n timer 45
expect 0 "{$dry:\"timer\",*\"timer_ms\":90000,\"warning_ms\":18000,*}" --json -n timer 90s

# Usage errors
expect_error "unknown command 'spin'" spin
expect_error "'led' needs a subcommand" led
expect_error "unknown command 'led spin'" led spin
expect_error "usage: magsafe status" status extra
expect_error "usage: magsafe led brightness <color> <percent>" led brightness green
expect_error "invalid mode 'purple' (expected auto, off, green, or amber)" led set purple
expect_error "invalid color 'red' (expected green or amber)" led brightness red 40
expect_error "invalid percent '101' (expected 0-100)" led brightness green 101
expect_error "invalid percent '+4' (expected 0-100)" led brightness green +4
expect_error "invalid color 'alternate' (expected green or amber)" led fade alternate
expect_error "invalid color 'blue' (expected green, amber, or alternate)" led blink blue
expect_error "invalid --count '0' (expected 1-300 or infinite)" led blink green -c 0
expect_error "invalid --interval-ms '50' (expected 100-10000)" led blink green -i 50
expect_error "invalid --duration-ms '100' (expected 500-60000)" led fade green -d 100
expect_error "planned run time is 60450 ms; finite effects are limited to 60000 ms (omit --count to run until stopped)" \
  led blink green -c 60
expect_error "--duration-ms applies only to fade commands" led blink green -d 1000
expect_error "blink, fade, and timer options do not apply to 'status'" status -c 3
expect_error "--count given more than once" led blink green -c 3 --count 4
expect_error "option '--count' needs a value" led blink green --count
expect_error "usage: magsafe led stream <color>" led stream
expect_error "invalid color 'alternate' (expected green or amber)" led stream alternate
expect_error "blink, fade, and timer options do not apply to 'led stream'" led stream green -i 200
expect_error "invalid color 'red' (expected green or amber)" visualizer red
# Brackets are escaped because expected output is a shell pattern.
expect_error "usage: magsafe visualizer \\[<color>\\] \\[--preview\\]" visualizer green amber
expect_error "blink, fade, and timer options do not apply to 'visualizer'" visualizer -c 3
expect_error "--preview applies only to 'visualizer'" led stream green --preview
expect_error "--preview applies only to 'visualizer'" status --preview
expect_error "invalid duration '5s' (expected 10s-24h, such as 25m, 90s, or 1h30m)" timer 5s
expect_error "invalid duration '30m1h' (expected 10s-24h, such as 25m, 90s, or 1h30m)" timer 30m1h
expect_error "invalid duration '25h' (expected 10s-24h, such as 25m, 90s, or 1h30m)" timer 25h
expect_error "usage: magsafe timer <duration> \\[-c <n>\\] \\[-i <ms>\\]" timer
expect_error "--duration-ms applies only to fade commands" timer 25m -d 1000
expect_error "planned alarm time is 400000 ms; finite alarms are limited to 60000 ms (omit --count to flash until stopped)" \
  timer 25m -c 200 -i 1000
expect_error "invalid option '--bogus'" --bogus status
expect 2 '{"ok":false,"error":"invalid option '"'"'--bogus'"'"'"}' -n --bogus status --json
expect 2 '' -n spin

if [ "$failures" -eq 0 ]; then
  echo "All $tests tests passed."
else
  echo "$failures of $tests tests failed."
  exit 1
fi
