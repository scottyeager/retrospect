# Retrospect

A live audio looper with a special feature: it's always recording and a loop can always be created from what just happened before.

## Building

Requires CMake 3.22+, a C++20 compiler, and ncurses development libraries.

On Debian/Ubuntu:

```
sudo apt install build-essential cmake libncurses-dev
```

Build:

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

## Running

```
./build/retrospect
```

The application starts in simulation mode with a sine wave as input. This is for testing the quantization and control logic without audio hardware.

## TUI Controls

| Key | Action |
|-----|--------|
| 1-8 | Select loop slot |
| Space | Capture from ring buffer (lookback) |
| R | Record / stop recording (classic mode) |
| m | Mute / unmute |
| r | Reverse playback |
| o | Start overdub |
| O | Stop overdub |
| u | Undo last overdub layer |
| U | Redo last undone layer |
| c | Clear loop |
| [ / ] | Half / double speed |
| Tab | Cycle quantize mode (Free / Beat / Bar) |
| + / - | BPM +/- 5 |
| B / b | Lookback bars +/- 1 |
| Esc | Cancel all pending operations |
| q | Quit |

## Architecture

All operations are quantized to the internal metronome. When quantize mode is set to Bar or Beat, actions like capture, mute, and reverse are scheduled and execute at the next boundary.

**Core components:**

- **Metronome** - BPM, time signature, sample-accurate beat/bar tracking
- **RingBuffer** - Continuous circular recording, sized for max lookback at minimum BPM (static allocation)
- **Loop** - Multi-layer audio with overdub undo/redo, reverse, speed, crossfade
- **LoopEngine** - Quantized operation scheduling, ring buffer capture, classic record

**Two ways to create a loop:**

- **Capture** (Space) - Grab the last N bars from the ring buffer. The lookback is always recording, so you never miss a moment.
- **Record** (R) - Traditional looper behavior. Press to start at the next quantize boundary, press again to stop. Loop length is set by the recording duration.

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

## JUCE Audio Backend

The core logic is independent of JUCE. To build with real audio I/O (requires JUCE dependencies):

```
cmake .. -DRETROSPECT_USE_JUCE=ON
```

## License

MIT
