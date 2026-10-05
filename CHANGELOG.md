# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

[Unreleased]: https://github.com/mpopv/magsafe-cli/compare/v0.8.0...HEAD
[0.8.0]: https://github.com/mpopv/magsafe-cli/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/mpopv/magsafe-cli/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/mpopv/magsafe-cli/releases/tag/v0.6.0
