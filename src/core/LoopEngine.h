#pragma once

#include "core/Metronome.h"
#include "core/MetronomeClick.h"
#include "core/MidiSync.h"
#include "core/InputChannel.h"
#include "core/Loop.h"
#include "core/SpscQueue.h"

#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <optional>
#include <atomic>
#include <mutex>
#include <thread>

namespace retrospect {

/// Types of operations that can be quantized
enum class OpType {
    CaptureLoop,     // Capture from ring buffer and start playing
    Record,          // Start classic recording (sets loop length)
    StopRecord,      // Stop classic recording and start playback
    Mute,            // Mute a loop
    Unmute,          // Unmute a loop
    ToggleMute,      // Toggle mute state
    Reverse,         // Toggle reverse playback
    StartOverdub,    // Begin overdub recording
    StopOverdub,     // Stop overdub recording
    UndoLayer,       // Undo last overdub layer
    RedoLayer,       // Redo last undone layer
    SetSpeed,        // Change playback speed
    ClearLoop,       // Clear a loop
    Seek,            // Seek/retrigger to a position
    ScrambleOn,      // Enable scramble mode
    ScrambleOff,     // Disable scramble mode
    SetScrambleWindow // Change scramble window duration
};

/// Human-readable description for an OpType
std::string opTypeDescription(OpType type);

/// Callback for engine state changes (used by TUI)
struct EngineCallbacks {
    std::function<void()> onStateChanged;
    std::function<void(const std::string&)> onMessage;
    std::function<void(const MetronomePosition&)> onBeat;
    std::function<void(const MetronomePosition&)> onBar;
};

/// Snapshot of a channel index + ring buffer state for background reads.
struct ChannelSnap {
    int channelIndex;
    RingBuffer::Snapshot snap;
};

/// A background capture thread that reads ring buffer data and mixes channels
/// off the audio thread. Capture is two-phase:
///   Phase 1: spawned when capture is scheduled (copies bulk of audio)
///   Phase 2: signalled at quantize boundary (copies remaining tail)
/// The audio thread installs the result when done.
struct BackgroundCapture {
    std::thread thread;
    std::atomic<bool> done{false};
    std::atomic<bool> phase2Ready{false};
    std::atomic<bool> cancelled{false};
    std::vector<float> completedAudio;
    int loopIndex = -1;
    int64_t captureLen = 0;
    int64_t phase1Samples = 0;
    double bars = 0.0;
    double recordedBpm = 0.0;
    int crossfadeSamples = 256;
    int liveCount = 0;
    uint64_t activeChannelMask = 0;

    // Phase 2 data (written by audio thread before setting phase2Ready)
    std::vector<ChannelSnap> phase2Snaps;
    int64_t phase2SamplesAgo = 0;
};

/// An in-progress classic recording (accumulating per-channel input)
struct ActiveRecording {
    int loopIndex = -1;
    std::vector<std::vector<float>> channelBuffers; // per input channel
    uint64_t activeChannelMask = 0;                 // sticky: set when channel breaches threshold
    int64_t startSample = 0;
};

/// Command types for the TUI→Audio SPSC queue
enum class CommandType {
    ScheduleOp,     // Generic op (mute, reverse, overdub, undo, redo, clear)
    CaptureLoop,    // Capture from ring buffer
    Record,         // Start classic recording
    StopRecord,     // Stop classic recording
    SetSpeed,       // Change loop playback speed
    SetBpm,         // Change metronome BPM
    CancelPending,  // Cancel all pending ops
    SetMidiSync,    // Enable/disable MIDI sync (quantized)
    ScrambleOn,     // Enable scramble mode on a loop
    ScrambleOff,    // Disable scramble mode on a loop
    SetScrambleWindow, // Change scramble window duration
    Undo            // Universal undo: cancel pending if any, else undo layer
};

/// Command sent from TUI thread to audio thread
struct EngineCommand {
    CommandType commandType = CommandType::ScheduleOp;
    OpType opType = OpType::Mute;       // For ScheduleOp
    int loopIndex = -1;
    Quantize quantize = Quantize::Bar;
    double value = 0.0;                 // Speed, BPM, seek position, or window duration
    int lookbackBars = 1;               // For CaptureLoop
    ScrambleParams scrambleParams;      // For ScrambleOn
};

/// Central engine managing loops, ring buffer, metronome, and quantized operations.
///
/// In a real audio context, processBlock() is called from the audio callback.
/// For TUI testing, a simulation timer can drive it.
class LoopEngine {
public:
    /// Create the engine with given settings.
    /// Ring buffer is sized to hold maxLookbackBars at minBpm.
    /// @param maxLoops Maximum number of loops
    /// @param maxLookbackBars Maximum lookback in bars
    /// @param sampleRate Audio sample rate
    /// @param minBpm Minimum expected BPM (used to size ring buffer)
    /// @param numInputChannels Number of input channels (each gets a ring buffer)
    /// @param liveThreshold Activity threshold (0 = disabled, all channels pass)
    /// @param liveWindowMs Activity detection window in milliseconds
    LoopEngine(int maxLoops = 8, int maxLookbackBars = 8,
               double sampleRate = 44100.0, double minBpm = 60.0,
               int numInputChannels = 1, float liveThreshold = 0.0f,
               int liveWindowMs = 500);
    ~LoopEngine();

    /// Process a block of multi-channel audio.
    /// @param input Array of per-channel input buffers (may be nullptr for missing channels)
    /// @param numInputChannels Number of input channel pointers
    /// @param output Output audio buffer (mono, will be summed into)
    /// @param numSamples Number of samples in this block
    void processBlock(const float* const* input, int inputChannelCount,
                      float* output, int numSamples);

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

    /// Schedule classic record start (quantized to boundary)
    void scheduleRecord(int loopIndex, Quantize quantize);

    /// Schedule classic record stop (quantized to boundary)
    void scheduleStopRecord(int loopIndex, Quantize quantize);

    /// Schedule scramble mode on for a loop
    void scheduleScrambleOn(int loopIndex, Quantize quantize,
                            const ScrambleParams& params);

    /// Schedule scramble mode off for a loop
    void scheduleScrambleOff(int loopIndex, Quantize quantize);

    /// Set the scramble window duration on the fly
    void setScrambleWindowDuration(int loopIndex, double beats);

    /// Execute an operation immediately (no quantization)
    void executeOpNow(OpType type, int loopIndex = -1);

    /// Cancel all pending operations
    void cancelPending();

    /// Cancel pending operations for a specific loop
    void cancelPending(int loopIndex);

    /// Universal undo: cancel pending ops if any, otherwise undo a layer
    void undo(int loopIndex = -1);

    // Accessors
    Metronome& metronome() { return metronome_; }
    const Metronome& metronome() const { return metronome_; }

    /// Access a specific input channel
    InputChannel& inputChannel(int index) { return inputChannels_[static_cast<size_t>(index)]; }
    const InputChannel& inputChannel(int index) const { return inputChannels_[static_cast<size_t>(index)]; }

    /// Number of input channels
    int numInputChannels() const { return static_cast<int>(inputChannels_.size()); }

    Loop& loop(int index) { return loops_[static_cast<size_t>(index)]; }
    const Loop& loop(int index) const { return loops_[static_cast<size_t>(index)]; }

    int maxLoops() const { return static_cast<int>(loops_.size()); }
    int activeLoopCount() const;

    /// Enqueue a command from the TUI thread (lock-free)
    void enqueueCommand(const EngineCommand& cmd);

    /// Atomic recording state accessors (safe to read from TUI thread)
    bool isRecordingAtomic() const { return isRecordingAtomic_.load(std::memory_order_relaxed); }
    int recordingLoopIdxAtomic() const { return recordingLoopIdxAtomic_.load(std::memory_order_relaxed); }

    /// Default quantization mode for new operations
    Quantize defaultQuantize() const { return defaultQuantize_; }
    void setDefaultQuantize(Quantize q) { defaultQuantize_ = q; }

    /// Lookback duration in bars for capture
    int lookbackBars() const { return lookbackBars_; }
    /// Set lookback bars, clamped to [1, maxLookbackBars]. Returns actual value set.
    int setLookbackBars(int bars);

    /// Maximum lookback in bars (determines ring buffer size)
    int maxLookbackBars() const { return maxLookbackBars_; }

    /// Global crossfade in samples
    int crossfadeSamples() const { return crossfadeSamples_; }
    void setCrossfadeSamples(int samples) { crossfadeSamples_ = samples; }

    double sampleRate() const { return sampleRate_; }

    /// Default scramble parameters (used when not overridden per command)
    const ScrambleParams& defaultScrambleParams() const { return defaultScrambleParams_; }
    void setDefaultScrambleParams(const ScrambleParams& p) { defaultScrambleParams_ = p; }

    /// Latency compensation in samples (round-trip: output + input).
    /// When set, capture and recording operations offset their read positions
    /// to align recorded audio with the metronome's internal timeline.
    int64_t latencyCompensation() const { return latencyCompensation_; }
    void setLatencyCompensation(int64_t samples) { latencyCompensation_ = std::max(int64_t(0), samples); }

    /// Monitoring: pass-through input to output
    bool inputMonitoring() const { return inputMonitoring_; }
    void setInputMonitoring(bool on) { inputMonitoring_ = on; }

    /// Live channel detection threshold (0 = disabled, all channels pass).
    /// Channels with peak level below this threshold are considered inactive.
    float liveThreshold() const { return liveThreshold_; }
    void setLiveThreshold(float t) { liveThreshold_ = t; }

    /// Bitmask of which input channels are currently live (thread-safe).
    /// Bit N is set if channel N is live.
    uint64_t liveChannelMask() const { return liveChannelMask_.load(std::memory_order_relaxed); }

    /// Per-channel peak levels snapshot (mutex-protected, updated from audio thread)
    std::vector<float> channelPeaksSnapshot() const;

    /// Metronome click (audible beat indicator)
    bool metronomeClickEnabled() const { return click_.isEnabled(); }
    void setMetronomeClickEnabled(bool on) { click_.setEnabled(on); }
    float metronomeClickVolume() const { return click_.volume(); }
    void setMetronomeClickVolume(float v) { click_.setVolume(v); }

    /// MIDI sync output (24 PPQN clock)
    MidiSync& midiSync() { return midiSync_; }
    const MidiSync& midiSync() const { return midiSync_; }
    bool midiSyncEnabled() const { return midiSync_.isEnabled(); }
    /// Direct (unquantized) enable/disable — use for init and shutdown only.
    void setMidiSyncEnabled(bool on) { midiSync_.setEnabled(on); }
    /// Schedule MIDI sync enable/disable to fire at the next quantize boundary.
    void scheduleMidiSync(bool on, Quantize quantize);

    /// Whether a classic recording is in progress
    bool isRecording() const { return activeRecording_.has_value(); }
    int recordingLoopIndex() const;

    /// Set callbacks
    void setCallbacks(EngineCallbacks cb);

    /// Register a callback that fires when BPM changes at the audio level.
    /// Useful for propagating tempo changes to external systems (e.g. JACK transport).
    void setBpmChangedCallback(std::function<void(double)> cb) { bpmChangedCallback_ = std::move(cb); }

    /// Register a callback called at the start of each processBlock with the
    /// metronome's current totalSamples. Used to synchronize JACK transport BBT
    /// with our internal timeline.
    void setTransportPositionCallback(std::function<void(int64_t)> cb) { transportPositionCallback_ = std::move(cb); }

    /// Loop selection — bitmask of which loops are targeted by commands with loopIndex == -1
    void selectLoop(int idx);
    void deselectLoop(int idx);
    void toggleSelectLoop(int idx);
    void setSelectedLoopMask(uint64_t mask) { selectedLoopMask_.store(mask, std::memory_order_relaxed); }
    uint64_t selectedLoopMask() const { return selectedLoopMask_.load(std::memory_order_relaxed); }
    bool isLoopSelected(int idx) const { return (selectedLoopMask() >> idx) & 1; }

    /// Find the next available (empty) loop slot. Returns -1 if all full.
    int nextEmptySlot() const;

    /// Status message for TUI
    std::string statusMessage() const;

private:
    /// Execute pending ops for a loop that are due at currentSample
    void flushDueOps(Loop& lp, int64_t currentSample);

    /// Spawn background capture thread (phase 1: copy bulk of audio).
    /// Called from drainCommands when a capture is scheduled.
    void beginCapture(int loopIndex, int64_t captureLen, int64_t gap);

    /// Signal the background capture to grab remaining samples (phase 2).
    /// Called from flushDueOps when the quantize boundary fires.
    void signalCapturePhase2(int loopIndex);

    /// Detect which input channels were active during a capture window.
    /// Returns a bitmask and count of active channels.
    struct ActiveChannels {
        uint64_t mask = 0;
        int count = 0;
    };
    ActiveChannels detectActiveChannels(int64_t captureStartSample) const;

    /// Start a classic recording into a loop
    void fulfillRecord(Loop& lp);

    /// Stop a classic recording
    void fulfillStopRecord(Loop& lp);

    /// Check for completed background captures and swap results into loops
    void checkBackgroundCaptures();

    /// Cancel and join any background capture for the given loop index
    void cancelBackgroundCapture(int loopIndex);

    /// Drain commands from the SPSC queue into loop pending state (audio thread)
    void drainCommands();

    /// Compute executeSample for a given quantize mode (audio thread)
    int64_t computeExecuteSample(Quantize quantize) const;

    Metronome metronome_;
    MetronomeClick click_;
    MidiSync midiSync_;
    std::vector<InputChannel> inputChannels_;
    /// Per-channel: metronome sample when the threshold was last exceeded.
    /// Updated once per processBlock. Used by detectActiveChannels to decide
    /// channel inclusion in O(1) instead of scanning the captured segment.
    std::vector<int64_t> lastThresholdBreachSample_;
    std::vector<Loop> loops_;

    std::optional<ActiveRecording> activeRecording_;

    /// Background capture threads (one per loop slot, pre-allocated)
    std::vector<std::unique_ptr<BackgroundCapture>> bgCaptures_;

    /// Cancelled captures awaiting join (deferred off the audio thread)
    std::vector<std::unique_ptr<BackgroundCapture>> zombieCaptures_;

    /// Pending quantized MIDI sync toggle: {executeSample, enable}
    std::optional<std::pair<int64_t, bool>> pendingMidiSync_;

    /// Per-channel overdub accumulation (scoped per overdub layer)
    std::vector<std::vector<float>> overdubChannelBuffers_;
    uint64_t overdubActiveChannelMask_ = 0;
    int overdubLoopIndex_ = -1;

    Quantize defaultQuantize_ = Quantize::Bar;
    int lookbackBars_ = 1;
    int maxLookbackBars_;
    int crossfadeSamples_ = 256;
    double sampleRate_;
    int64_t latencyCompensation_ = 0;
    bool inputMonitoring_ = false;
    float liveThreshold_ = 0.0f;
    ScrambleParams defaultScrambleParams_;

    EngineCallbacks callbacks_;
    std::string lastMessage_;

    std::function<void(double)> bpmChangedCallback_;
    std::function<void(int64_t)> transportPositionCallback_;

    // Thread safety: TUI -> Audio command queue
    SpscQueue<EngineCommand, 256> commandQueue_;

    // Thread safety: Audio -> TUI display snapshot
    mutable std::mutex displayMutex_;
    std::vector<float> channelPeaksSnapshot_;
    std::atomic<bool> isRecordingAtomic_{false};
    std::atomic<int> recordingLoopIdxAtomic_{-1};
    std::atomic<uint64_t> liveChannelMask_{0};
    std::atomic<uint64_t> selectedLoopMask_{0x1};  // Loop 0 selected by default
};

} // namespace retrospect
