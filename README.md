# magsafe

Control the light on Apple's USB-C to MagSafe 3 cable (A2363) from the macOS command line. Set its color, dim it, blink or fade it, and read the cable's firmware state.

```sh
magsafe led set amber                 # solid amber
magsafe led brightness green 25       # dim green
magsafe led blink alternate -c 6      # flash green and amber in turn
magsafe led fade green               # breathe until Ctrl-C
magsafe reset                         # hand the light back to macOS
```

> [!WARNING]
> `magsafe` uses private Apple interfaces and has been tested on one Mac with one cable firmware version. See [Compatibility](#compatibility). It changes only the cable's RAM brightness and the Mac's light color setting. It never writes firmware or stored calibration.

## Install

You need a Mac with a MagSafe 3 port and an A2363 cable.

With [Homebrew](https://brew.sh):

```sh
brew install mpopv/tap/magsafe
```

From source, with the Xcode Command Line Tools (`xcode-select --install`):

```sh
git clone https://github.com/mpopv/magsafe-cli.git
cd magsafe-cli
make install
```

`make install` puts `magsafe` in `~/.local/bin`, which must be on your `PATH`. It also installs the man page in `~/.local/share/man/man1` and the zsh completion in `~/.local/share/zsh/site-functions`; add that directory to `fpath` to use it. To install somewhere else, set `PREFIX`, for example `make && sudo make install PREFIX=/usr/local`. Run `make uninstall` with the same `PREFIX` to remove it. Homebrew installs the man page and completion automatically.

Connect the cable's USB-C end to a power adapter and its MagSafe end to the Mac.

## Usage

```text
magsafe [options] <command> [<args>]
```

| Command | Description |
| --- | --- |
| `status` | Show firmware version, security flags, and light state |
| `reset` | Restore 100% brightness and return color control to macOS |
| `led set <mode>` | Set the light to `auto`, `off`, `green`, or `amber` |
| `led brightness <color> <percent>` | Light `green` or `amber` at 0–100% brightness |
| `led blink <color>` | Blink `green`, `amber`, or `alternate` |
| `led fade-in <color>` | Ramp up, then switch off |
| `led fade-out <color>` | Switch on, then ramp down |
| `led fade <color>` | Ramp up, then ramp down |
| `led get` | Show the light mode and PWM output |
| `firmware version` | Show the cable firmware version |
| `firmware security` | Show the configuration-setter lock and signature-skip flags |
| `firmware calibration` | Show the four stored light calibration values |
| `capabilities` | List supported and unavailable functions |
| `version` | Show the `magsafe` version |
| `help` | Show help |

| Option | Description |
| --- | --- |
| `--json` | Print the result, or the error, as one JSON object on standard output |
| `-n`, `--dry-run` | Check the command and print its plan without touching hardware |
| `-h`, `--help` | Show help |
| `--version` | Show the version |
| `-c`, `--count <n>` | Blink or fade cycles: 1–300 or `infinite` (default `infinite`) |
| `-i`, `--interval-ms <ms>` | Blink on and off time, or dark time after each fade cycle: 100–10000 (default 500) |
| `-d`, `--duration-ms <ms>` | Time for each fade ramp: 500–60000 (default 1000) |

Options can go before or after the command, and each can appear once. Write values as `--count 4`, `--count=4`, or `-c 4`.

Commands that touch hardware need root, so `magsafe` checks the command and then runs itself again through `sudo`. Don't type `sudo` yourself; sudo's normal password prompt and credential cache apply. Help, `version`, `capabilities`, dry runs, and invalid commands never ask for a password. A script without a terminal needs cached sudo credentials or its own sudo setup.

Only one hardware command can run at a time. A second one fails with `another magsafe command is running` instead of waiting.

### Color

```sh
magsafe led set green
magsafe led set off
magsafe led set auto    # macOS chooses again: amber while charging, green when full
```

The mode stays until macOS or another command changes it. `auto` doesn't change brightness. Use `reset` to restore that too.

### Brightness

```sh
magsafe led brightness amber 40
```

This sets the color's brightness scale in the cable's RAM, selects that color, and reads the cable's PWM output until it matches the expected value. The check waits up to 2 seconds. The setting lasts until the cable loses power or you run `magsafe reset`. You can't read back the current percentage. `led get` shows the raw PWM values.

### Blink and fade

```sh
magsafe led blink green                   # until Ctrl-C: 500 ms on, 500 ms off
magsafe led blink alternate -c 6 -i 250   # 6 flashes, green and amber in turn
magsafe led fade-in amber -c 3 -d 2000    # 3 cycles: 2-second rise, then off
magsafe led fade green                    # until Ctrl-C
```

| Command | One cycle | Colors |
| --- | --- | --- |
| `led blink` | 100% for the interval, then 0% for the interval | `green`, `amber`, `alternate` |
| `led fade-in` | Ramp up over the duration, switch to 0%, then stay dark for the interval | `green`, `amber` |
| `led fade-out` | Switch to 100%, ramp down over the duration, then stay dark for the interval | `green`, `amber` |
| `led fade` | Ramp up and down (each over the duration), then stay dark for the interval | `green`, `amber` |

Every effect does the following:

- Before the first cycle, it switches the light off, selects the color, and waits 450 ms so the cable's own color transition finishes while dark. Alternate blink does this before every flash, starting with green.
- Fades change brightness at most 20 times a second. They check the PWM output at the top and bottom of every cycle.
- When it finishes, fails, or receives Ctrl-C (`SIGINT`, `SIGTERM`, or `SIGHUP`), it runs `reset`. Any brightness you set before the effect is not restored.

An effect repeats until Ctrl-C (exit code 130) or a device error unless you give `--count`. A finite effect must be planned to take 60 seconds or less, including preparation:

| Command | Planned time (ms) |
| --- | --- |
| `led blink` | `count × 2 × interval + preparation` |
| `led fade-in`, `led fade-out` | `450 + count × (duration + interval)` |
| `led fade` | `450 + count × (2 × duration + interval)` |

Preparation is 450 ms, or `count × 450` ms for `alternate`. Device calls add some time beyond the plan. `--dry-run` shows the planned total. For a longer run, leave out `--count`.

### Reset

```sh
magsafe reset
```

This sets both brightness scales to 100% and returns color control to macOS. Each step runs even if another fails, so color control returns to macOS even when the cable is missing. If a step fails, the error lists the result of each step. This is not a factory reset.

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
- `color_selector` is the color the cable is driving: `0` off, `1` amber, `2` green. At 0% brightness the selector keeps the color while both PWM values are 0.
- `status` also shows `security_word` with two flags decoded from it, `configuration_setter_locked` and `signature_skip_active`. These two flags don't cover every security or debug path.
- `firmware calibration` shows `calibration` as `[amber pwm0, amber pwm3, green pwm0, green pwm3]`.

## Scripting

### JSON output

With `--json`, every command except help and version prints exactly one JSON object on standard output. Errors are included:

```console
$ magsafe --json led get
{"ok":true,"command":"led get","firmware":"3.2.0","version_word":"0x03020000","smc_control":3,"pwm0":8215,"pwm3":13306,"color_selector":2}
$ magsafe --json led brightness red 40
{"ok":false,"error":"invalid color 'red' (expected green or amber)"}
```

Without `--json`, commands that change the light print nothing on success. Read commands use the same field names in both modes.

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
| `reset` | `firmware`, `version_word`, `brightness_percent`, `system_color_control_requested` |
| `capabilities` | `cli_version`, `diagnostic_firmware`, `supported`, `unavailable` (no `command`) |

`version_word` and `security_word` are hexadecimal strings. On success, `brightness_percent` is always `100`, and `pwm_verified` and `system_color_control_requested` are always `true`. An effect that is stopped reports an error, not a result.

### Dry runs

`--dry-run` checks the command and its values and prints the plan. It makes no device calls and never asks for a password. It doesn't check whether a cable is connected.

```console
$ magsafe --json --dry-run led blink alternate -c 6 -i 250
{"ok":true,"command":"led blink","dry_run":true,"device_calls":0,"color":"alternate","count":6,"interval_ms":250,"preparation_ms":2700,"duration_ms":5700}
```

Effects add `count`, `interval_ms`, `fade_ms` (fades only), `preparation_ms`, and `duration_ms`, which is the planned total. For an infinite run, the default, `count` is `"infinite"` and unbounded times are `null`.

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success |
| `1` | Device, permission, or lock error |
| `2` | Invalid command, option, or value |
| `128 + n` | Effect stopped by signal `n` (`130` for Ctrl-C) |

A failed command can leave a change partly applied. Run `magsafe reset` to restore the defaults. Errors that sudo reports itself are plain text on standard error, even with `--json`.

## How it works

- **Color:** the Mac's SMC key `ACLC` selects auto, off, green, or amber.
- **Brightness and diagnostics:** fixed messages go to the cable through the MagSafe port's AppleHPM controller. The transport accepts only the cable's version, security, and diagnostic addresses. There is no raw command access.
- **Firmware check:** diagnostics, brightness, and effects run only when the cable reports firmware exactly 3.2.0. `firmware version` and `led set` work with any version.

There is no firmware flashing, no raw memory or PWM access, no security or calibration writes, and no arbitrary patterns, RGB colors, or factory reset.

| Source | Role |
| --- | --- |
| [`src/main.c`](src/main.c) | Argument parsing, sudo, locking, and output |
| [`src/led.c`](src/led.c) | Brightness, blink, fade, and reset |
| [`src/firmware.c`](src/firmware.c) | Cable firmware commands |
| [`src/hpm.c`](src/hpm.c) | AppleHPM transport (private interface) |
| [`src/apple-smc.c`](src/apple-smc.c) | SMC light-mode key, an original byte-buffer implementation |

## Compatibility

Hardware testing used an M3 Pro MacBook Pro (Mac15,7) running macOS 27.0 (26A428) with an A2363 cable reporting firmware 3.2.0. Other Macs and macOS versions are unverified, and the private interfaces can change with macOS updates.

Earlier versions of the code produced these results on that setup:

- Status, version, security, and calibration reads worked. Green maps to selector 2 and amber to selector 1.
- For both colors, 0%, 40%, and 100% brightness produced exactly the expected PWM values.
- A timed alternate blink completed. Afterwards, both scales were back at their 100% baseline and calibration was unchanged at `[535, 655, 502, 813]`.
- In 340 preparation timing trials, the 450 ms dark preparation passed its PWM checks.

The current code has passed only the dry-run tests. Not yet verified on hardware:

- complete blink and fade sequences
- signal cleanup
- visible light output

## Development

```sh
make          # build build/magsafe
make test     # run CLI tests (no cable or sudo needed)
make format   # format sources with clang-format
make clean    # remove build output
```

[CI](.github/workflows/ci.yml) builds with `-Werror`, runs the tests, and checks formatting and the man page on every push to `main` and every pull request.

### Releasing

1. Set `VERSION` in `src/main.c`, and move the `Unreleased` notes in `CHANGELOG.md` under the new version.
2. Commit, then tag and push: `git tag v0.7.0 && git push origin main v0.7.0`.

The [release workflow](.github/workflows/release.yml) then tests the tag and checks that it matches `magsafe --version`. It publishes a GitHub release with the changelog notes and points the Homebrew formula in [mpopv/homebrew-tap](https://github.com/mpopv/homebrew-tap) at the new tag.

## License

[MIT](LICENSE)
