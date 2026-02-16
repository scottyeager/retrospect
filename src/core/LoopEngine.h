#pragma once

#include "core/Metronome.h"
#include "core/RingBuffer.h"
#include "core/Loop.h"

#include <vector>
#include <memory>
#include <functional>
#include <deque>
#include <string>

namespace retrospect {

/// Types of operations that can be quantized
enum class OpType {
    CaptureLoop,     // Capture from ring buffer and start playing
    Mute,            // Mute a loop
    Unmute,          // Unmute a loop
    ToggleMute,      // Toggle mute state
    Reverse,         // Toggle reverse playback
    StartOverdub,    // Begin overdub recording
    StopOverdub,     // Stop overdub recording
    UndoLayer,       // Undo last overdub layer
    RedoLayer,       // Redo last undone layer
    SetSpeed,        // Change playback speed
    ClearLoop        // Clear a loop
};

/// A pending operation waiting for its quantization boundary
struct PendingOp {
    OpType type;
    int loopIndex = -1;         // Target loop (-1 for new/next available)
    int64_t executeSample = 0;  // Sample at which to execute
    Quantize quantize = Quantize::Bar;
    double speedValue = 1.0;    // For SetSpeed

    /// Lookback duration in samples for CaptureLoop
    int64_t lookbackSamples = 0;

    /// Human-readable description
    std::string description() const;
};

/// Callback for engine state changes (used by TUI)
struct EngineCallbacks {
    std::function<void()> onStateChanged;
    std::function<void(const std::string&)> onMessage;
    std::function<void(const MetronomePosition&)> onBeat;
    std::function<void(const MetronomePosition&)> onBar;
};

/// Central engine managing loops, ring buffer, metronome, and quantized operations.
///
/// In a real audio context, processBlock() is called from the audio callback.
/// For TUI testing, a simulation timer can drive it.
class LoopEngine {
public:
    /// Create the engine with given settings.
    /// @param maxLoops Maximum number of loops
    /// @param lookbackSeconds Ring buffer lookback duration in seconds
    /// @param sampleRate Audio sample rate
    LoopEngine(int maxLoops = 8, double lookbackSeconds = 30.0,
               double sampleRate = 44100.0);

    /// Process a block of audio. In real use, called from audio callback.
    /// @param input Input audio buffer (mono)
    /// @param output Output audio buffer (mono, will be summed into)
    /// @param numSamples Number of samples in this block
    void processBlock(const float* input, float* output, int numSamples);

    /// Schedule a quantized operation. The operation will be executed
    /// at the next quantization boundary (beat or bar).
    void scheduleOp(OpType type, int loopIndex = -1,
                    Quantize quantize = Quantize::Bar);

    /// Schedule a capture with specific lookback
    void scheduleCaptureLoop(int loopIndex, Quantize quantize,
                             double lookbackBars = 0.0);

    /// Schedule a speed change
    void scheduleSetSpeed(int loopIndex, double speed,
                          Quantize quantize = Quantize::Beat);

    /// Execute an operation immediately (no quantization)
    void executeOpNow(OpType type, int loopIndex = -1);

    /// Cancel all pending operations
    void cancelPending();

    /// Cancel pending operations for a specific loop
    void cancelPending(int loopIndex);

    // Accessors
    Metronome& metronome() { return metronome_; }
    const Metronome& metronome() const { return metronome_; }

    RingBuffer& ringBuffer() { return ringBuffer_; }
    const RingBuffer& ringBuffer() const { return ringBuffer_; }

    Loop& loop(int index) { return loops_[static_cast<size_t>(index)]; }
    const Loop& loop(int index) const { return loops_[static_cast<size_t>(index)]; }

    int maxLoops() const { return static_cast<int>(loops_.size()); }
    int activeLoopCount() const;

    const std::deque<PendingOp>& pendingOps() const { return pendingOps_; }

    /// Default quantization mode for new operations
    Quantize defaultQuantize() const { return defaultQuantize_; }
    void setDefaultQuantize(Quantize q) { defaultQuantize_ = q; }

    /// Lookback duration in bars for capture (0 = auto-detect based on content)
    double lookbackBars() const { return lookbackBars_; }
    void setLookbackBars(double bars) { lookbackBars_ = bars; }

    /// Global crossfade in samples
    int crossfadeSamples() const { return crossfadeSamples_; }
    void setCrossfadeSamples(int samples) { crossfadeSamples_ = samples; }

    double sampleRate() const { return sampleRate_; }

    /// Monitoring: pass-through input to output
    bool inputMonitoring() const { return inputMonitoring_; }
    void setInputMonitoring(bool on) { inputMonitoring_ = on; }

    /// Set callbacks
    void setCallbacks(EngineCallbacks cb);

    /// Find the next available (empty) loop slot. Returns -1 if all full.
    int nextEmptySlot() const;

    /// Status message for TUI
    std::string statusMessage() const;

private:
    void executeOp(const PendingOp& op);
    void executeCaptureLoop(const PendingOp& op);

    Metronome metronome_;
    RingBuffer ringBuffer_;
    std::vector<Loop> loops_;
    std::deque<PendingOp> pendingOps_;

    Quantize defaultQuantize_ = Quantize::Bar;
    double lookbackBars_ = 1.0;
    int crossfadeSamples_ = 256;
    double sampleRate_;
    bool inputMonitoring_ = false;

    EngineCallbacks callbacks_;
    std::string lastMessage_;
};

} // namespace retrospect
