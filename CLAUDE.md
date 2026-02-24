# CLAUDE.md

## Project Overview

Retrospect is a live audio looper with an "always recording" ring buffer. Loops can be captured retroactively from what just happened, or recorded traditionally. It runs on Linux with JACK or ALSA audio backends via JUCE, controlled through an ncurses TUI, and optionally via OSC for remote/networked control.

## Build & Run

**Prerequisites** (Debian/Ubuntu):
```
sudo apt install build-essential cmake libncurses-dev pkg-config liblo-dev
```

**Build:**
```
make build
```
This runs CMake (Debug, compile_commands.json enabled) and builds with `make -j$(nproc)`. Always use `make build` to verify changes compile.

**Run:**
```
make run          # builds first, then runs
./build/retrospect_artefacts/Debug/retrospect   # run directly
```

**Clean:**
```
make clean
```

## Directory Structure

```
src/
  audio_main.cpp          # Entry point: JUCE audio I/O setup, device management
  core/                   # Pure C++20 audio engine (no framework dependencies)
    Metronome.h/cpp       # BPM, time signature, sample-accurate beat/bar tracking
    MetronomeClick.h      # Synthesized click sound (header-only)
    MidiSync.h/cpp        # MIDI clock output at 24 PPQN
    RingBuffer.h/cpp      # Circular buffer for always-on lookback recording
    InputChannel.h/cpp    # Per-channel ring buffer + live activity detection
    Loop.h/cpp            # Single loop: multi-layer overdub, undo/redo, reverse, speed
    LoopEngine.h/cpp      # Central engine: orchestrates loops, scheduling, quantization
    EngineCommand.h       # Command types (forwarding header)
    SpscQueue.h           # Lock-free single-producer single-consumer queue
  client/                 # Engine interface abstraction
    EngineClient.h        # Abstract interface + EngineSnapshot data types
    LocalEngineClient.h/cpp   # In-process direct engine access
    OscEngineClient.h/cpp     # Remote OSC-based engine client
  server/
    OscServer.h/cpp       # liblo-based OSC server for remote control
  tui/
    Tui.h/cpp             # ncurses terminal UI: display + keyboard input
  config/
    Config.h/cpp          # TOML config file loading + CLI argument parsing
clients/
  python/                 # Python OSC client (standalone remote controller)
    retrospect_client.py  # Client library with dataclass state model
    retro_cli.py          # Interactive ANSI terminal controller
    pyproject.toml        # Package metadata
```

## Architecture

### Threading Model

- **Audio thread** (`processBlock`): Sample-by-sample processing, no locks or allocations. Drains commands from the SPSC queue, advances metronome/MIDI sync, mixes loops, writes ring buffers.
- **TUI thread** (main): Keyboard input, display refresh. Sends commands via lock-free `SpscQueue`. Reads state via mutex-protected snapshots (non-blocking `try_lock`).
- **OSC server thread**: liblo threaded server receives commands, forwards to engine. Pushes state to subscribers at ~30Hz.

### Command Flow

```
TUI/OSC input
  → enqueueCommand(EngineCommand) → SpscQueue (lock-free)
  → Audio thread: drainCommands() in processBlock()
  → Compute execution sample (now + samplesUntilBoundary)
  → Store in per-loop pending state slots
  → flushDueOps() executes when sample count reached
  → Callbacks: onMessage, onStateChanged, onBeat, onBar
```

### Core Components

- **LoopEngine**: Central coordinator. Manages N loops (default 8, max 64), input channels, metronome, MIDI sync. All audio processing happens here.
- **Loop**: Individual loop with layers. States: `Empty`, `Playing`, `Muted`, `Recording`. Supports overdub layers with undo/redo, reverse, variable speed (0.25x-4x), crossfade at boundaries.
- **Metronome**: Sample-accurate BPM/time-signature tracking. Computes samples until next beat/bar for quantized scheduling.
- **RingBuffer**: Statically-sized circular buffer (sized for max lookback at min BPM). No dynamic allocation in audio path.
- **InputChannel**: Wraps RingBuffer with per-channel live detection via block-based peak tracking.

### Quantization

Operations can be scheduled with three modes:
- `Free` — execute immediately
- `Beat` — snap to next beat boundary
- `Bar` — snap to next bar boundary

Each loop has independent pending slots per operation type. Last-wins semantics for conflicting commands. Clear cancels all other pending ops on that loop.

### Two Recording Modes

- **Capture** (Space): Grabs last N bars from the ring buffer retroactively. The ring buffer is always recording.
- **Record** (R): Traditional looper — press to start at next quantize boundary, press again to stop.

## Build System

CMake 3.22+, C++20. Five static libraries linked into one executable:

| Library | Purpose | Dependencies |
|---------|---------|-------------|
| `retrospect_config` | TOML config + CLI parsing | toml++ |
| `retrospect_core` | Audio engine (pure C++) | none |
| `retrospect_client` | EngineClient implementations | core, liblo |
| `retrospect_server` | OSC server | core, liblo |
| `retrospect_tui` | ncurses UI | client, ncurses |

External dependencies fetched via CMake `FetchContent`: JUCE 8.0.4, toml++ v3.4.0. System dependencies: ncurses, liblo (pkg-config).

## Code Conventions

**Naming:**
- Classes/enums: `PascalCase` (`LoopEngine`, `OpType`)
- Methods: `camelCase` (`processBlock`, `toggleMute`)
- Member variables: `snake_case_` with trailing underscore (`metronome_`, `playPos_`)
- Constants: prefixed k or descriptive (`kBlockSize`, `kPPQN`)

**File organization:** One class per file. Header (.h) for interface, source (.cpp) for implementation. Everything in the `retrospect` namespace.

**Compiler warnings:** `-Wall -Wextra -Wpedantic` on all targets.

**Audio thread safety:**
- No allocations, no locks, no I/O in the audio callback
- Lock-free SPSC queue for cross-thread commands
- Atomic variables for lightweight shared state
- `try_lock` for non-blocking snapshot updates
- Static buffer sizing at initialization

**Comments:** Use `///` for public API docs, `//` for implementation notes. Explain "why", not "what".

## Configuration

Config file at `~/.config/retrospect/config.toml` (or `$XDG_CONFIG_HOME/retrospect/config.toml`).

Sections: `[audio]`, `[engine]`, `[input]`, `[metronome]`, `[midi]`, `[osc]`, `[tui]`.

### Output Routing (`[audio]`)

For ALSA backends with multi-channel interfaces, output routing controls where audio goes:

- `output_mode`: `"stereo"` (default) mixes all loops to a stereo pair; `"multichannel"` maps input channels to corresponding output channels with loop playback on the main pair.
- `main_outputs`: 1-based channel indices for the main mix (default: `[1, 2]`).
- `metronome_outputs`: 1-based channel indices for the metronome click. Omit to include in the main mix. Example: `[3, 4]` sends click to headphones.

JACK ignores these settings — it creates virtual ports that are patched externally, using mono output on a single port (old behavior).

CLI flags: `--headless` (OSC server only), `--connect HOST:PORT` (remote TUI), `--list-midi`.

## OSC Interface

Default port: 7770. Commands use `/retro/` prefix. Key endpoints:

- `/retro/loop/capture` (iii) — loopIdx, quantize, lookbackBars
- `/retro/loop/record` (ii), `/retro/loop/stop_record` (ii)
- `/retro/loop/mute`, `/retro/loop/toggle_mute`, `/retro/loop/reverse` (ii)
- `/retro/loop/overdub/start`, `/retro/loop/overdub/stop` (ii)
- `/retro/loop/undo`, `/retro/loop/redo` (i)
- `/retro/loop/speed` (idi), `/retro/loop/clear` (i)
- `/retro/metronome/bpm` (d), `/retro/metronome/click` (i)
- `/retro/settings/quantize` (i), `/retro/settings/lookback_bars` (i)
- `/retro/settings/midi_sync` (i)
- `/retro/cancel_pending`

State push (server→client): `/retro/state/metronome`, `/retro/state/loop`, `/retro/state/recording`, `/retro/state/settings`, `/retro/state/pending_op`, `/retro/state/log`.

## Key Enums

```cpp
enum class LoopState  { Empty, Playing, Muted, Recording };
enum class Quantize   { Free, Beat, Bar };
enum class OutputMode { Stereo, Multichannel };
enum class OpType     { CaptureLoop, Record, StopRecord, Mute, Unmute, ToggleMute,
                        Reverse, StartOverdub, StopOverdub, UndoLayer, RedoLayer,
                        SetSpeed, ClearLoop };
```

## Adding New Source Files

When adding a `.cpp` file, add it to the appropriate `add_library()` call in `CMakeLists.txt`. Header-only files don't need CMake changes.

## Adding New Config Options

When adding a new config option, update all three places:

1. **`src/config/Config.h`** — Add the field with its default value to the `Config` struct.
2. **`src/config/Config.cpp`** — Parse the option from the TOML file in `Config::load()`.
3. **`config.toml`** — Add a commented-out entry with a description in the appropriate section. This file serves as the reference for all available options.
