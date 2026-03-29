#pragma once

#include "core/Metronome.h"  // For Quantize

#include <atomic>
#include <vector>
#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <random>

namespace retrospect {

class StretchWorker;

/// State of a loop
enum class LoopState {
    Empty,     // No audio loaded
    Playing,   // Playing back
    Muted,     // Has audio but not outputting
    Recording  // Overdubbing a new layer
};

/// Operation types for scheduling
enum class OpType {
    CaptureLoop,
    Record,
    StopRecord,
    Mute,
    Unmute,
    ToggleMute,
    Reverse,
    StartOverdub,
    StopOverdub,
    UndoLayer,
    RedoLayer,
    SetSpeed,
    ClearLoop,
    Seek,
    ScrambleOn,
    ScrambleOff,
    SetScrambleWindow
};

/// Direction for undo/redo pending operations
enum class UndoDirection { Undo, Redo };

/// Parameters for scramble mode
struct ScrambleParams {
    double windowDuration = 1.0;   // Length of each snippet in beats
    double fadeDuration = 0.125;   // Fade in/out duration in beats
    bool allowRepeat = true;       // Whether the same start position can repeat
};

/// A single pending operation waiting for a quantization boundary.
/// Each loop has at most one pending operation at a time.
struct PendingOp {
    OpType opType = OpType::Mute;
    int64_t executeSample = 0;
    Quantize quantize = Quantize::Bar;

    // Op-specific payload (only the relevant field is used based on opType)
    double speed = 1.0;              // SetSpeed
    int64_t lookbackSamples = 0;     // CaptureLoop
    int undoCount = 1;               // UndoLayer/RedoLayer
    ScrambleParams scrambleParams;   // ScrambleOn
};

/// A single layer of audio in a loop (one overdub pass)
struct LoopLayer {
    std::vector<float> audio;
    float gain = 1.0f;
    bool active = true;  // Can be toggled for undo
};

/// Represents a single loop with multiple layers and playback controls.
/// The loop length is determined by the first layer captured.
class Loop {
public:
    Loop();
    ~Loop();

    // Move-only (due to unique_ptr<StretchWorker> and atomic members)
    Loop(Loop&&) noexcept;
    Loop& operator=(Loop&&) noexcept;
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    /// Initialize the loop with audio captured from the ring buffer.
    /// This sets the loop length and creates the first layer.
    void loadFromCapture(std::vector<float> audio);

    /// Add an overdub layer. Must match the loop length.
    void addLayer(std::vector<float> audio);

    /// Undo the most recent active layer
    void undoLayer();

    /// Redo the most recently undone layer
    void redoLayer();

    /// Get the mixed output sample at the current playback position,
    /// then advance the position. Returns 0 if empty/muted.
    float processSample();

    /// Get mixed output for a block, advancing position.
    void processBlock(float* output, int numSamples);

    /// Record a sample into the current overdub layer (if recording).
    /// The sample is mixed (added) to the new layer.
    void recordSample(float input);

    /// Access the audio buffer of the current recording layer (last layer).
    /// Used by LoopEngine to write mixed overdub audio at stop time.
    std::vector<float>& recordLayerAudio() { return layers_.back().audio; }

    // State
    LoopState state() const { return state_; }
    bool isEmpty() const { return state_ == LoopState::Empty; }
    bool isPlaying() const { return state_ == LoopState::Playing; }
    bool isMuted() const { return state_ == LoopState::Muted; }
    bool isRecording() const { return state_ == LoopState::Recording; }

    // Controls
    void play();
    void mute();
    void toggleMute();
    void startOverdub();
    void stopOverdub();
    void toggleReverse();
    void setSpeed(double speed);
    void clear();

    /// Seek playback to an arbitrary sample position within the loop
    void seek(int64_t samplePos);

    /// Enable scramble mode with the given parameters
    void scrambleOn(const ScrambleParams& params, double samplesPerBeat);

    /// Disable scramble mode, resuming normal playback
    void scrambleOff();

    /// Set the window duration while scramble is active (takes effect next snippet)
    void setScrambleWindowDuration(double beats);

    /// Whether scramble mode is currently active
    bool isScrambling() const { return scrambleActive_; }

    // Properties
    int64_t lengthSamples() const { return loopLength_; }
    int64_t playPosition() const;
    bool isReversed() const { return reversed_.load(std::memory_order_relaxed); }
    double speed() const { return speed_; }
    int layerCount() const { return static_cast<int>(layers_.size()); }
    int activeLayerCount() const;
    int id() const { return id_; }
    void setId(int id) { id_ = id; }

    /// Length in bars (set when captured with quantization)
    double lengthInBars() const { return lengthInBars_; }
    void setLengthInBars(double bars) { lengthInBars_ = bars; }

    /// Crossfade length in samples (applied at loop boundaries)
    int crossfadeSamples() const { return crossfadeSamples_; }
    void setCrossfadeSamples(int samples) { crossfadeSamples_ = samples; }

    // --- Pending state ---
    const std::optional<PendingOp>& pendingOp() const { return pending_; }
    std::optional<PendingOp>& pendingOp() { return pending_; }
    bool hasPendingOps() const { return pending_.has_value(); }
    void clearPendingOps() { pending_.reset(); }

    // --- Time stretching ---

    /// Set the BPM at which this loop was recorded.
    /// Called once when the loop is captured/recorded.
    void setRecordedBpm(double bpm) { recordedBpm_ = bpm; }
    double recordedBpm() const { return recordedBpm_; }

    /// Set the current global BPM. When this differs from recordedBpm,
    /// time stretching activates to keep the loop in sync with the new tempo
    /// while preserving pitch.
    void setCurrentBpm(double bpm);
    double currentBpm() const { return currentBpm_; }

    /// Set the sample rate and pre-allocate the stretch worker.
    /// Must be called once during initialization before audio processing.
    void setSampleRate(double sr);

    /// Whether time stretching is currently active
    bool isTimeStretchActive() const;

private:
    float getMixedSample(int64_t pos) const;
    float crossfadeGain(int64_t pos) const;

    /// Process one sample in direct (non-stretched) mode
    float processDirectSample();

    /// Process one sample in time-stretched mode
    float processStretchedSample();

    /// Process one sample in scramble mode
    float processScrambleSample();

    /// Activate the pre-allocated stretch worker with a new feed callback
    void startStretchWorker();

    /// Deactivate the stretch worker (thread stays alive for reuse)
    void stopStretchWorker();

    std::vector<LoopLayer> layers_;
    LoopState state_ = LoopState::Empty;
    int64_t loopLength_ = 0;
    int64_t playPos_ = 0;
    std::atomic<bool> reversed_{false};
    double speed_ = 1.0;
    double fractionalPos_ = 0.0;  // For non-integer speed ratios
    int crossfadeSamples_ = 256;
    double lengthInBars_ = 0.0;
    int id_ = -1;
    std::optional<PendingOp> pending_;

    // Time stretch state
    double recordedBpm_ = 0.0;
    double currentBpm_ = 0.0;
    double sampleRate_ = 44100.0;

    std::unique_ptr<StretchWorker> stretchWorker_;

    /// Raw read position for feeding the stretcher (worker thread writes,
    /// audio thread reads for overdub alignment and display).
    std::atomic<int64_t> stretchRawPos_{0};

    // Scramble mode state
    bool scrambleActive_ = false;
    ScrambleParams scrambleParams_;
    double scrambleSamplesPerBeat_ = 0.0;
    int64_t scrambleSnippetPos_ = 0;     // Position within current snippet
    int64_t scrambleSnippetLen_ = 0;     // Length of current snippet in samples
    int64_t scrambleReadStart_ = 0;      // Start read position in loop for current snippet
    int64_t scrambleFadeSamples_ = 0;    // Fade duration in samples
    int64_t scrambleLastStart_ = -1;     // Last chosen start (for allow_repeat=false)
    std::mt19937 scrambleRng_{42};       // RNG for random position selection
};

} // namespace retrospect
