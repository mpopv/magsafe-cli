# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

[Unreleased]: https://github.com/mpopv/magsafe-cli/compare/v0.6.0...HEAD
[0.6.0]: https://github.com/mpopv/magsafe-cli/releases/tag/v0.6.0
