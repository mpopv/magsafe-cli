# magsafe

Control the light on Apple's USB-C to MagSafe 3 cable (A2363) from the macOS command line. Set its color, dim it, blink or fade it, pulse it to whatever is playing, run a timer on it, send Morse code, keep it dim, and read the cable's firmware state.

```sh
magsafe led set amber                 # solid amber
magsafe led brightness green 25       # dim green
magsafe led blink alternate -c 6      # flash green and amber in turn
magsafe led fade green                # breathe until Ctrl-C
magsafe visualizer                    # pulse to the music until Ctrl-C
magsafe timer 25m                     # dim as time runs out, then flash
magsafe morse sos                     # ... --- ... until Ctrl-C
magsafe settings dim 10               # keep the light at 10%
magsafe reset                         # back to your settings
sudo magsafe daemon install           # no more password prompts
```

> [!WARNING]
> `magsafe` uses private Apple interfaces and has been tested on one Mac with one cable firmware version. See [Compatibility](#compatibility). It changes only the cable's RAM brightness and the Mac's light color setting. It never writes firmware or stored calibration.

## Install

You need a Mac with a MagSafe 3 port and an A2363 cable, plugged into a power adapter.

```sh
brew install mpopv/tap/magsafe
```

Or build from source with the Xcode Command Line Tools (`xcode-select --install`):

```sh
git clone https://github.com/mpopv/magsafe-cli.git
cd magsafe-cli
make install
```

This installs `magsafe` in `~/.local/bin`, the man page in `~/.local/share/man/man1`, and the zsh completion in `~/.local/share/zsh/site-functions`. Make sure the first is on your `PATH` and the last is in your `fpath`. To install elsewhere, set `PREFIX`, for example `make && sudo make install PREFIX=/usr/local`. Run `make uninstall` with the same `PREFIX` to remove it.

## Usage

```text
magsafe [options] <command> [<args>]
```

| Command | Description |
| --- | --- |
| `status` | Show firmware version, security flags, and light state |
| `reset` | Return to the saved brightness and color mode (100% and macOS by default) |
| `led set <mode>` | Set the light to `auto`, `off`, `green`, or `amber` |
| `led brightness <color> <percent>` | Light `green` or `amber` at 0–100% brightness |
| `led blink <color>` | Blink `green`, `amber`, or `alternate` |
| `led fade-in <color>` | Ramp up, then switch off |
| `led fade-out <color>` | Switch on, then ramp down |
| `led fade <color>` | Ramp up, then ramp down |
| `led stream <color>` | Set `green` or `amber` brightness from lines on standard input |
| `led get` | Show the light mode and PWM output |
| `firmware version` | Show the cable firmware version |
| `firmware security` | Show the configuration-setter lock and signature-skip flags |
| `firmware calibration` | Show the four stored light calibration values |
| `visualizer [<color>]` | Pulse `green` (the default) or `amber` to the audio that is playing |
| `timer <duration>` | Count down on the light, then flash amber |
| `morse <text>` | Send text in green Morse code |
| `settings` | Show the saved brightness and color mode |
| `settings dim <percent>` | Save the brightness the light returns to: 0–100 |
| `settings color <mode>` | Save the color mode: `auto`, `off`, `green`, or `amber` |
| `settings reset` | Return to 100% brightness and macOS color control |
| `daemon install` | Run commands without a password, and apply settings at plug-in |
| `daemon uninstall` | Remove the daemon |
| `daemon status` | Show whether the daemon runs, and its version |
| `capabilities` | List supported and unavailable functions |
| `version` | Show the `magsafe` version |
| `help` | Show help |

| Option | Description |
| --- | --- |
| `--json` | Print the result, or the error, as one JSON object on standard output |
| `-n`, `--dry-run` | Check the command and print its plan without touching hardware |
| `-c`, `--count <n>` | Blink or fade cycles, timer alarm flashes, or Morse repetitions: 1–300 or `infinite` (default) |
| `-i`, `--interval-ms <ms>` | Blink or alarm on/off time, dark time after each fade, or Morse unit: 100–10000 (default 500, or 150 for Morse) |
| `-d`, `--duration-ms <ms>` | Time for each fade ramp: 500–60000 (default 1000) |
| `--preview` | Visualizer only: show the levels in the terminal instead of on the light |
| `-h`, `--help` | Show help |
| `--version` | Show the version |

Options can go before or after the command, as `--count 4`, `--count=4`, or `-c 4`. See `man magsafe` for the full reference.

**sudo:** commands that touch hardware need root, so `magsafe` checks the command and then runs itself again through `sudo`. Don't type `sudo` yourself. Help, `version`, `capabilities`, dry runs, and invalid commands never ask for a password. With the [daemon](#daemon) installed, no command asks for one.

**One at a time:** only one hardware command can run at once. A second fails with `another magsafe command is running` instead of waiting.

### Color and brightness

```sh
magsafe led set green
magsafe led set off
magsafe led set auto              # macOS chooses: amber while charging, green when full
magsafe led brightness amber 40
```

A mode stays until macOS or another command changes it. Brightness is kept in the cable's RAM until the cable loses power or you run `magsafe reset`. `led set auto` doesn't change brightness. `led brightness` waits up to 2 seconds for the cable's output to match, and the current percentage can't be read back.

### Blink and fade

```sh
magsafe led blink green                   # until Ctrl-C: 500 ms on, 500 ms off
magsafe led blink alternate -c 6 -i 250   # 6 flashes, green and amber in turn
magsafe led fade-in amber -c 3 -d 2000    # 3 cycles: 2-second rise, then off
magsafe led fade green                    # up and down until Ctrl-C
```

`-i` sets the blink time, or the dark pause after each fade. `-d` sets the length of each fade ramp.

- Effects repeat until Ctrl-C unless you give `--count`. A finite effect must fit in 60 seconds. `--dry-run` shows the planned time.
- Each effect selects its color while the light is off and waits 450 ms for the cable's own color transition. Alternate blink does this before every flash.
- When an effect finishes, fails, or is interrupted, it runs `reset`, which returns the light to your [settings](#settings).

### Timer

```sh
magsafe timer 25m                       # a Pomodoro
magsafe timer 1h30m
magsafe timer 90s -c 10 -i 250          # 10 quick flashes at the end, then exit
magsafe timer 5 && say "break's over"   # a plain number is minutes
```

The light starts green at full brightness and dims as time runs out. For the last fifth of the time, at most 5 minutes, it turns amber. At zero it flashes amber until Ctrl-C or for `--count` flashes, then runs `reset`. In a terminal, `magsafe` shows the time left.

A duration is 10 seconds to 24 hours, written as `25m`, `90s`, `1h`, or a combination such as `1h30m`. The time keeps running while the Mac sleeps. Ctrl-C during the alarm exits with 0, so `&&` works after a timer. Ctrl-C during the countdown exits with 130.

### Morse code

```sh
magsafe morse sos                        # until Ctrl-C
magsafe morse hello world -c 1           # once
magsafe morse "what's up?" -c 3 -i 100   # three times, faster
```

Text is sent in green, in international Morse code with standard timing. `-i` sets the unit, the length of a dot, from 100 ms. The default of 150 ms is about 8 words a minute. Text may have up to 200 letters, digits, and the punctuation `. , ? ' ! / ( ) & : ; = + - _ " $ @`. Quote text that the shell would change, and put `--` before text that starts with `-`. Like an effect, it repeats until Ctrl-C unless you give `--count`.

### Visualizer

```sh
magsafe visualizer              # green, until Ctrl-C
magsafe visualizer amber
magsafe visualizer --preview    # levels in the terminal; no cable or sudo
```

The light flashes with the kick drum of whatever the Mac is playing and falls to a dim glow between kicks, even in loud, heavily limited songs. It's held back by the output device's latency so that it changes when you hear the sound, including over Bluetooth.

The visualizer needs macOS 14.2 or later. It captures system audio with a Core Audio process tap, without a virtual audio driver, and analyzes it as you, not as root. Only brightness values go to `led stream`, which runs through sudo.

macOS grants System Audio Recording permission to your terminal app and asks on the first run. If you deny it, or no prompt appears, allow the app, or add it under **System Audio Recording Only**, in System Settings > Privacy & Security > Screen & System Audio Recording. After 5 seconds without any audio, `magsafe` prints a reminder, because nothing playing looks the same as blocked capture.

### Stream

```sh
# Ramp green up over 2 seconds; the light resets when the input ends.
for p in $(seq 0 5 100); do echo "$p"; sleep 0.1; done | magsafe led stream green
```

`led stream` reads brightness percentages from standard input, one per line. It applies the newest value at most 40 times a second and skips the rest, so pace the input yourself. It runs `reset` at the end of input, on Ctrl-C, on an error, or at an invalid line, which exits with code 2.

### Reset

```sh
magsafe reset
```

Sets both brightness scales and the color mode to your [settings](#settings): 100% and color control by macOS, unless you changed them. Each step runs even if another fails, so the color mode is set even when the cable is missing. This is not a factory reset.

### Settings

```sh
magsafe settings                  # show them
magsafe settings dim 10           # the light returns to 10%
magsafe settings color green      # and stays green, instead of macOS choosing
magsafe settings reset            # 100% and macOS color control again
```

Settings are the brightness and color mode that the light returns to after every command. Changing one applies it at once, and saves it in `/Library/Application Support/magsafe/settings`. The cable forgets its brightness when it loses power, as when you unplug it: the [daemon](#daemon) applies your settings again at boot, on plug-in, and on wake. Without the daemon, they apply at the next reset.

### Daemon

```sh
sudo magsafe daemon install       # once
magsafe daemon status
sudo magsafe daemon uninstall
```

The daemon is a root background service, started by launchd at boot, that runs `magsafe` commands for you without a password. Commands behave exactly as through sudo: the same output, `--json`, exit codes, and Ctrl-C. It serves root, the user at the Mac's screen, and admins.

`daemon install` copies `magsafe` to `/Library/PrivilegedHelperTools/com.mpopv.magsafe`, owned by root, because a root service must not run a file that your user account can change, as it can under Homebrew. Only someone who can use sudo may replace that copy, so the daemon doesn't update itself.

After `brew upgrade`, the first command that needs root notices that the daemon is older, prints `updating the daemon`, and runs `sudo magsafe daemon install` for you. sudo asks for your password once, and the command then runs through the new daemon. If the update fails, as without a terminal for the password, the command uses sudo. A daemon newer than `magsafe` is never replaced this way.

Without the daemon, or with `MAGSAFE_NO_DAEMON=1`, commands use sudo as before. The daemon logs to `/Library/Logs/magsafe-daemon.log`.

### Reading state

```console
$ magsafe led get
firmware:       3.2.0
version_word:   0x03020000
smc_control:    3 (green)
pwm0:           8215
pwm3:           13306
color_selector: 2 (green)
```

- `smc_control` is the Mac's light mode: `0` auto, `1` off, `3` green, `4` amber. Right after a change it can still show the previous mode.
- `pwm0` and `pwm3` are the cable's two PWM outputs. They measure electrical drive, not visible light, and are not RGB channels.
- `color_selector` is the color the cable is driving: `0` off, `1` amber, `2` green.
- `status` adds `security_word` and the two flags decoded from it, `configuration_setter_locked` and `signature_skip_active`. These don't cover every security or debug path.
- `firmware calibration` shows `[amber pwm0, amber pwm3, green pwm0, green pwm3]`.

## Scripting

With `--json`, every command except help and version prints exactly one JSON object on standard output, including errors:

```console
$ magsafe --json led get
{"ok":true,"command":"led get","firmware":"3.2.0","version_word":"0x03020000","smc_control":3,"pwm0":8215,"pwm3":13306,"color_selector":2}
$ magsafe --json led brightness red 40
{"ok":false,"error":"invalid color 'red' (expected green or amber)"}
```

Read commands use the same field names in both modes. Without `--json`, commands that change the light print nothing on success.

<details>
<summary>JSON fields by command</summary>

| Command | Fields besides `ok` and `command` |
| --- | --- |
| `firmware version` | `firmware`, `version_word` |
| `firmware security` | `firmware`, `version_word`, `security_word`, `configuration_setter_locked`, `signature_skip_active` |
| `firmware calibration` | `firmware`, `version_word`, `calibration` |
| `led get` | `firmware`, `version_word`, `smc_control`, `pwm0`, `pwm3`, `color_selector` |
| `status` | All fields of `firmware security` and `led get` |
| `led set` | `color` (the mode) |
| `led brightness` | `firmware`, `version_word`, `pwm0`, `pwm3`, `color_selector`, `color`, `percent`, `pwm_verified` |
| `led blink` | `firmware`, `version_word`, `color`, `flashes`, `brightness_percent`, `system_color_control_requested` |
| `led fade-in`, `led fade-out`, `led fade` | `firmware`, `version_word`, `color`, `fades`, `fade_ms`, `interval_ms`, `pwm_verified`, `brightness_percent`, `system_color_control_requested` |
| `led stream` | `firmware`, `version_word`, `color`, `values`, `writes`, `brightness_percent`, `system_color_control_requested` |
| `timer` | `firmware`, `version_word`, `timer_ms`, `flashes`, `brightness_percent`, `system_color_control_requested` |
| `morse` | `firmware`, `version_word`, `text`, `code`, `repetitions`, `unit_ms`, `brightness_percent`, `system_color_control_requested` |
| `reset` | `firmware`, `version_word`, `brightness_percent`, `system_color_control_requested` |
| `settings` | `dim`, `color`, and, after a change, `applied` |
| `daemon status` | `installed`, `running`, `daemon_version`, `cli_version`, `current`, `socket` |
| `daemon install` | `helper`, `plist`, `socket`, `daemon_version` |
| `daemon uninstall` | `removed` |
| `capabilities` | `cli_version`, `diagnostic_firmware`, `supported`, `unavailable` (no `command`) |

`version_word` and `security_word` are hexadecimal strings. On success, `pwm_verified` is always `true`, and `brightness_percent` and `system_color_control_requested` show the settings that the light returned to. An effect that is stopped reports an error, not a result, so the visualizer always reports an error.

</details>

`--dry-run` checks the command and prints its plan, including the planned time, without any device calls or password. It doesn't check whether a cable is connected:

```console
$ magsafe --json --dry-run led blink alternate -c 6 -i 250
{"ok":true,"command":"led blink","dry_run":true,"device_calls":0,"color":"alternate","count":6,"interval_ms":250,"preparation_ms":2700,"duration_ms":5700}
```

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success, including a timer alarm stopped with Ctrl-C |
| `1` | Device, permission, or lock error |
| `2` | Invalid command, option, or value, including an invalid `led stream` line |
| `128 + n` | Effect, stream, visualizer, or timer countdown stopped by signal `n` (`130` for Ctrl-C) |

A failed command can leave a change partly applied. Run `magsafe reset` to restore your settings. Errors that sudo reports itself are plain text on standard error, even with `--json`.

## How it works

- **Color:** the Mac's SMC key `ACLC` selects auto, off, green, or amber.
- **Brightness and diagnostics:** fixed messages go to the cable through the MagSafe port's AppleHPM controller. The transport accepts only the cable's version, security, and diagnostic addresses.
- **Firmware check:** diagnostics, brightness, and effects run only when the cable reports firmware exactly 3.2.0. `firmware version` and `led set` work with any version.
- **Daemon:** the client sends the daemon its command line and its standard input, output, and error over a local socket. The daemon checks the user and the command, runs `magsafe` as a root child with those streams, passes on Ctrl-C, and returns the exit code.

There is no firmware flashing, no raw memory or PWM access, no security or calibration writes, and no RGB colors or factory reset.

## Compatibility

Hardware testing used an M3 Pro MacBook Pro (Mac15,7) running macOS 27.0 (26A428) with an A2363 cable reporting firmware 3.2.0. Other Macs and macOS versions are unverified, and the private interfaces can change with macOS updates.

Reads, brightness, and a timed alternate blink have worked on that setup. The current code has passed only the dry-run and unit tests, and these are not yet verified on hardware:

- complete blink and fade sequences
- `led stream` and the visualizer driving the cable
- the timer and Morse code on the cable
- the daemon as launchd runs it, and settings applied at plug-in and wake
- signal cleanup and visible light output

See [CONTRIBUTING.md](CONTRIBUTING.md#hardware-testing) for the full test record.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for building, testing, the source layout, and releasing.

## License

[MIT](LICENSE)
