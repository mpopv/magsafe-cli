# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

- After an upgrade, the first command that needs root updates the daemon
  itself: it prints `updating the daemon`, runs `sudo magsafe daemon install`,
  which asks for your password once, and then runs through the new daemon.
  There's no need to reinstall by hand. If the update fails, as without a
  terminal, the command uses sudo, and a daemon newer than `magsafe` is never
  replaced. This was tested with a stand-in for sudo, not yet on hardware.

## [0.11.0] - 2026-10-05

- `sudo magsafe daemon install` adds a root launchd daemon that runs `magsafe`
  commands without a password, for root, the user at the screen, and admins.
  Commands behave as through sudo, with the same output, exit codes, and
  Ctrl-C. The daemon runs from a root-owned copy of `magsafe`, so reinstall it
  after upgrading; until then, commands use sudo. `daemon uninstall` and
  `daemon status` remove and check it, and `MAGSAFE_NO_DAEMON=1` skips it.
- `settings dim <percent>` and `settings color <mode>` save the brightness and
  color mode that the light returns to. Every reset, including the one after
  each effect, now returns to them instead of 100% and macOS control. The
  daemon applies them again at boot, on plug-in, and on wake.
- The JSON fields `brightness_percent` and `system_color_control_requested`
  show the settings that the light returned to.
- The visualizer starts its light helper as a plain `magsafe led stream`, so
  the helper uses the daemon when one is installed.
- The daemon has not yet run under launchd on hardware. It was tested as a
  user on a private socket, with stand-in commands for Ctrl-C and exit codes.

## [0.10.0] - 2026-10-04

- `morse <text>` sends text in green, in international Morse code with
  standard timing. A unit is 150 ms by default, set with `--interval-ms`. It
  repeats until Ctrl-C unless `--count` is given, and a terminal shows each
  character as it is sent. Text may have up to 200 letters, digits, and common
  punctuation.
- Not yet run on the cable; its sequence, timing, and signal handling were
  tested against stand-in firmware.

## [0.9.0] - 2026-10-04

- `timer <duration>` counts down on the light: green dimming as time runs out,
  amber for the last fifth (at most 5 minutes), then amber flashes until Ctrl-C
  or `--count` flashes. Durations such as `25m`, `90s`, and `1h30m` run from
  10 seconds to 24 hours, and a plain number is minutes. A terminal shows the
  time left. Ctrl-C during the alarm exits with 0.
- Not yet run on the cable; its sequence and signal handling were tested
  against stand-in firmware.

## [0.8.0] - 2026-10-04

- The visualizer flashes with the kick drum and falls to a dim glow between
  kicks, even in loud, heavily limited songs, which used to keep the light
  near full brightness. It finds beats as rises in three bass bands that stand
  out from the song's own recent rises, scales each flash to the song's typical
  beat, and follows the tempo: with a kick on nearly every beat, other hits
  between the kicks flash dimmer, and flashes fade with the beat period.
- `make build/analyze` builds a development tool that shows what the visualizer
  does with audio files.
- The new analysis was tuned and tested on limited mixes of Apple Loops, not
  yet on released songs.

## [0.7.0] - 2026-10-04

- `visualizer` pulses the light to whatever the Mac is playing. It follows the
  bass, flashes on kick drums and other sharp rises, and adjusts to quiet and
  loud music. It captures system audio with a Core Audio process tap (macOS
  14.2 or later) and needs System Audio Recording permission for the terminal
  app. Audio capture runs without root; only brightness values reach a root
  `led stream` helper.
- `visualizer --preview` shows the levels in the terminal, without the cable or
  sudo.
- `led stream <color>` sets brightness from percentages read one per line on
  standard input, at most 40 times a second, and resets at the end of input.
- `capabilities` lists `brightness-stream` and `visualizer`, and no longer lists
  `arbitrary-patterns` as unavailable.
- `make test` also runs unit tests of the visualizer's analysis and the stream
  input parser.
- Not yet verified on the cable or with captured audio. See Compatibility in the
  README.

## [0.6.0] - 2026-10-04

First release.

- Set the light to auto, off, green, or amber, and set green or amber brightness
  from 0 to 100% with PWM readback.
- Blink, fade-in, fade-out, and fade effects. They repeat until Ctrl-C unless
  `--count` is given, and always end by restoring 100% brightness and macOS
  color control.
- `reset` to restore 100% brightness and macOS color control.
- Read firmware version, security flags, calibration, and light state.
- `--json` output, `--dry-run` plans, and distinct exit codes for scripting.
- Man page and zsh completion.

[Unreleased]: https://github.com/mpopv/magsafe-cli/compare/v0.11.0...HEAD
[0.11.0]: https://github.com/mpopv/magsafe-cli/compare/v0.10.0...v0.11.0
[0.10.0]: https://github.com/mpopv/magsafe-cli/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/mpopv/magsafe-cli/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/mpopv/magsafe-cli/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/mpopv/magsafe-cli/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/mpopv/magsafe-cli/releases/tag/v0.6.0
