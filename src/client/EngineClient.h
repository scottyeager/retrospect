#pragma once

#include "core/Metronome.h"
#include "core/Loop.h"

#include <string>
#include <vector>
#include <cstdint>

namespace retrospect {

// Forward
enum class OpType;

/// Snapshot of metronome state for display
struct MetronomeSnapshot {
    int bar = 0;
    int beat = 0;
    double beatFraction = 0.0;
    double bpm = 120.0;
    int beatsPerBar = 4;
    bool running = true;
};

/// Snapshot of a single loop for display
struct LoopSnapshot {
    LoopState state = LoopState::Empty;
    double lengthInBars = 0.0;
    int layers = 0;
    int activeLayers = 0;
    double speed = 1.0;
    bool reversed = false;
    int64_t playPosition = 0;
    int64_t lengthSamples = 0;

    bool isEmpty() const { return state == LoopState::Empty; }
    bool isMuted() const { return state == LoopState::Muted; }
    bool isPlaying() const { return state == LoopState::Playing; }
    bool isRecording() const { return state == LoopState::Recording; }
};

/// Snapshot of a pending operation for display
struct PendingOpSnapshot {
    int loopIndex = -1;
    Quantize quantize = Quantize::Bar;
    std::string description;
};

/// Snapshot of a single input channel for display
struct InputChannelSnapshot {
    float peakLevel = 0.0f;
    bool live = false;
};

/// Complete engine state snapshot, updated once per TUI frame
struct EngineSnapshot {
    MetronomeSnapshot metronome;
    std::vector<LoopSnapshot> loops;
    std::vector<PendingOpSnapshot> pendingOps;
    std::vector<InputChannelSnapshot> inputChannels;

    bool isRecording = false;
    int recordingLoopIndex = -1;

    Quantize defaultQuantize = Quantize::Bar;
    int lookbackBars = 1;
    bool clickEnabled = true;
    bool midiSyncEnabled = false;
    bool midiOutputAvailable = false;
    float liveThreshold = 0.0f;
    double sampleRate = 44100.0;
    int maxLoops = 8;
    int activeLoopCount = 0;

    /// Messages received since last poll
    std::vector<std::string> messages;
};

/// Quantize enum to/from integer encoding (0=Free, 1=Beat, 2=Bar)
inline int quantizeToInt(Quantize q) {
    switch (q) {
        case Quantize::Free: return 0;
        case Quantize::Beat: return 1;
        case Quantize::Bar:  return 2;
    }
    return 2;
}

inline Quantize intToQuantize(int v) {
    switch (v) {
        case 0: return Quantize::Free;
        case 1: return Quantize::Beat;
        case 2: return Quantize::Bar;
    }
    return Quantize::Bar;
}

/// LoopState enum to/from integer encoding
inline int loopStateToInt(LoopState s) {
    switch (s) {
        case LoopState::Empty:     return 0;
        case LoopState::Playing:   return 1;
        case LoopState::Muted:     return 2;
        case LoopState::Recording: return 3;
    }
    return 0;
}

inline LoopState intToLoopState(int v) {
    switch (v) {
        case 0: return LoopState::Empty;
        case 1: return LoopState::Playing;
        case 2: return LoopState::Muted;
        case 3: return LoopState::Recording;
    }
    return LoopState::Empty;
}

/// Abstract interface for controlling the loop engine.
/// The TUI uses this instead of LoopEngine& directly.
class EngineClient {
public:
    virtual ~EngineClient() = default;

    // --- Commands ---
    virtual void scheduleCaptureLoop(int loopIndex, Quantize quantize,
                                     int lookbackBars = 0) = 0;
    virtual void scheduleRecord(int loopIndex, Quantize quantize) = 0;
    virtual void scheduleStopRecord(int loopIndex, Quantize quantize) = 0;
    virtual void scheduleOp(OpType type, int loopIndex, Quantize quantize) = 0;
    virtual void scheduleSetSpeed(int loopIndex, double speed,
                                  Quantize quantize) = 0;
    virtual void executeOpNow(OpType type, int loopIndex) = 0;
    virtual void cancelPending() = 0;

    // --- Settings ---
    virtual void setDefaultQuantize(Quantize q) = 0;
    virtual int setLookbackBars(int bars) = 0;
    virtual void setMetronomeClickEnabled(bool on) = 0;
    virtual void setMidiSyncEnabled(bool on) = 0;
    virtual void setBpm(double bpm) = 0;

    // --- State ---
    virtual const EngineSnapshot& snapshot() const = 0;

    /// Update the snapshot from the engine. Called once per TUI frame.
    virtual void poll() = 0;
};

} // namespace retrospect
