# Contributing

## Development

You need the Xcode Command Line Tools (`xcode-select --install`).

```sh
make          # build build/magsafe
make test     # run unit and CLI tests (no cable, audio, or sudo needed)
make format   # format sources with clang-format
make clean    # remove build output
```

[CI](.github/workflows/ci.yml) builds with `-Werror`, runs the tests, and checks formatting and the man page on every push to `main` and every pull request. When you change a command or option, update the [man page](man/magsafe.1), the [zsh completion](completions/_magsafe), and the `Unreleased` section of [CHANGELOG.md](CHANGELOG.md).

### Visualizer analysis

To see what the visualizer does with a song, build the analysis tool and run it on audio files. It prints the light as a line of 30 characters per second, with statistics. Several files are looped and mixed, so that loops can stand in for a song, and `--limit` drives the mix into a limiter, as loud mastering does:

```sh
make build/analyze
build/analyze song.m4a
build/analyze --limit --seconds 10 drums.caf bass.caf synth.caf
```

The analysis looks for beats every 5 ms:

- **Beats:** sharp rises in three bass bands between 30 and 160 Hz. A rise that stands out from the song's own recent rises is a beat. Its flash depends on how much it stands out compared with the song's typical beat, so overall loudness doesn't matter.
- **Tempo:** the rises also give a tempo between 70 and 180 BPM and a grid of beats. When nearly every grid beat has a beat, kicks on the grid flash to full brightness, and other hits between them flash at about half. Syncopated beats keep their full flashes. Flashes fade over about a third of the beat period.
- **Glow:** between beats, a dim glow follows the bass level within its recent range. It grows when no beats come for a few seconds, as in a breakdown. Silence leaves the light dark.

The visualizer sends 30 frames a second, each held back by the output device's reported latency minus about 25 ms for the light itself.

## Source layout

| Source | Role |
| --- | --- |
| [`src/main.c`](src/main.c) | Argument parsing, sudo, locking, and output |
| [`src/led.c`](src/led.c) | Brightness, blink, fade, stream, timer, Morse, and reset |
| [`src/stream.c`](src/stream.c) | `led stream` input parsing |
| [`src/timer.c`](src/timer.c) | Timer durations and dimming schedule |
| [`src/morse.c`](src/morse.c) | Morse code table, text, and timing |
| [`src/visualizer.c`](src/visualizer.c) | Visualizer frame loop and latency delay |
| [`src/analysis.c`](src/analysis.c) | Beat detection, tempo grid, and glow |
| [`src/audio.m`](src/audio.m) | System audio capture with a Core Audio process tap (Objective-C) |
| [`src/firmware.c`](src/firmware.c) | Cable firmware commands |
| [`src/hpm.c`](src/hpm.c) | AppleHPM transport (private interface) |
| [`src/apple-smc.c`](src/apple-smc.c) | SMC light-mode key, an original byte-buffer implementation |
| [`tools/analyze.m`](tools/analyze.m) | Visualizer analysis tool (not installed) |

The visualizer's permission check uses private TCC functions, and is skipped if they are missing.

## Hardware testing

Hardware testing used an M3 Pro MacBook Pro (Mac15,7) running macOS 27.0 (26A428) with an A2363 cable reporting firmware 3.2.0.

Earlier versions of the code produced these results on that setup:

- Status, version, security, and calibration reads worked. Green maps to selector 2 and amber to selector 1.
- For both colors, 0%, 40%, and 100% brightness produced exactly the expected PWM values.
- A timed alternate blink completed. Afterwards, both scales were back at their 100% baseline and calibration was unchanged at `[535, 655, 502, 813]`.
- In 340 preparation timing trials, the 450 ms dark preparation passed its PWM checks.

The current code has passed only the dry-run and unit tests. Not yet verified on hardware:

- complete blink and fade sequences
- `led stream` and the visualizer driving the cable, including how fast the cable accepts brightness changes
- the timer and Morse code on the cable; their sequences, timing, and signal handling were tested only against stand-in firmware
- sudo passing the visualizer's pipe through to `led stream`
- signal cleanup
- visible light output

In a user's run of the visualizer, macOS prompted for System Audio Recording permission and the visualizer worked after the prompts were accepted, with no changes in System Settings. The parent and helper process handling was also tested with a stand-in helper. The kick-keyed analysis in 0.8.0 was tuned and tested on limited mixes of Apple Loops, not yet on released songs.

## Releasing

1. Set `VERSION` in `src/main.c`, and move the `Unreleased` notes in `CHANGELOG.md` under the new version.
2. Commit, then tag and push: `git tag v0.7.0 && git push origin main v0.7.0`.

The [release workflow](.github/workflows/release.yml) then tests the tag and checks that it matches `magsafe --version`. It publishes a GitHub release with the changelog notes and points the Homebrew formula in [mpopv/homebrew-tap](https://github.com/mpopv/homebrew-tap) at the new tag.
