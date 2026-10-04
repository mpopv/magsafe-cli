# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

[Unreleased]: https://github.com/mpopv/magsafe-cli/compare/v0.7.0...HEAD
[0.7.0]: https://github.com/mpopv/magsafe-cli/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/mpopv/magsafe-cli/releases/tag/v0.6.0
