# Retrospect

A live audio looper with a special feature: it's always recording and a loop can always be created from what just happened before. Runs on Linux with JACK or ALSA audio via JUCE, controlled through an ncurses TUI and optionally via OSC.

## Building

Requires CMake 3.22+, a C++20 compiler, ncurses, and liblo (OSC).

On Debian/Ubuntu:

```
sudo apt install build-essential cmake libncurses-dev pkg-config liblo-dev
```

Build:

```
make build
```

## Running

```
make run
```

Or run the binary directly:

```
./build/retrospect_artefacts/Debug/retrospect
```

**CLI flags:**

| Flag | Description |
|------|-------------|
| `--jack` | Use JACK audio backend |
| `--alsa` | Use ALSA audio backend |
| `--headless` | Run without TUI (OSC server only) |
| `--connect HOST:PORT` | Connect as a remote TUI client |
| `--list-midi` | List available MIDI output devices |

## TUI Controls

| Key | Action |
|-----|--------|
| 1-8 | Select loop slot |
| Up / Down | Navigate loop slots |
| Space | Capture from ring buffer (lookback) |
| r | Record / stop recording (classic mode) |
| m | Mute / unmute |
| v | Reverse playback |
| o | Start overdub |
| O | Stop overdub |
| u | Undo last overdub layer |
| U | Redo last undone layer |
| c | Clear loop |
| [ / ] | Half / double speed |
| Tab | Cycle quantize mode (Free / Beat / Bar) |
| + / - | BPM +/- 5 |
| t | Tap tempo |
| B / b | Lookback bars +/- 1 |
| M | Toggle metronome click |
| S | Toggle MIDI sync |
| Esc | Cancel all pending operations |
| q | Quit |

## Configuration

Copy `config.toml` to `~/.config/retrospect/config.toml` (or `$XDG_CONFIG_HOME/retrospect/config.toml`) and uncomment the options you want to change.

Sections: `[audio]`, `[engine]`, `[input]`, `[metronome]`, `[midi]`, `[osc]`, `[tui]`. See `config.toml` for all available options and defaults.

## Architecture

All operations are quantized to the internal metronome. When quantize mode is set to Bar or Beat, actions like capture, mute, and reverse are scheduled and execute at the next boundary.

**Core components:**

- **Metronome** - BPM, time signature, sample-accurate beat/bar tracking
- **RingBuffer** - Continuous circular recording, sized for max lookback at minimum BPM (static allocation)
- **Loop** - Multi-layer audio with overdub undo/redo, reverse, variable speed, crossfade
- **LoopEngine** - Quantized operation scheduling, ring buffer capture, classic record
- **InputChannel** - Per-channel ring buffer with live activity detection
- **TimeStretcher** - Tempo-aware pitch-preserving time stretch (Signalsmith Stretch) — loops automatically adjust when BPM changes
- **MidiSync** - MIDI clock output at 24 PPQN
- **JACK Transport** - Acts as JACK timebase master, broadcasting BBT position and tempo

**Two ways to create a loop:**

- **Capture** (Space) - Grab the last N bars from the ring buffer. The lookback is always recording, so you never miss a moment.
- **Record** (r) - Traditional looper behavior. Press to start at the next quantize boundary, press again to stop. Loop length is set by the recording duration.

Audio round-trip latency is automatically compensated so captures and recordings are sample-aligned.

## Remote Control (OSC)

Retrospect includes an OSC server (default port 7770) for remote control. Run with `--headless` for OSC-only operation, or use `--connect HOST:PORT` to attach a remote TUI.

A Python OSC client is available in `clients/python/` with both a library (`retrospect_client.py`) and an interactive terminal controller (`retro_cli.py`).

## Cross-compiling for ARM64

Build an ARM64 binary using a containerized Debian cross-toolchain (works from any host distro). Requires Docker or Podman.

```
make cross-arm64-extract
```

This builds inside a container and copies the binary to `build/arm64/retrospect`. The Makefile auto-detects Podman, falling back to Docker.

To just build the container image without extracting:

```
make cross-arm64
```

## License

MIT
