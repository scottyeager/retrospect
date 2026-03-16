#pragma once

#include "core/Metronome.h"  // For Quantize

#include <vector>
#include <cstdint>
#include <string>
#include <optional>
#include <memory>
#include <random>

namespace retrospect {

class TimeStretcher;

/// State of a loop
enum class LoopState {
    Empty,     // No audio loaded
    Playing,   // Playing back
    Muted,     // Has audio but not outputting
    Recording  // Overdubbing a new layer
};

/// Direction for undo/redo pending operations
enum class UndoDirection { Undo, Redo };

/// A single pending operation waiting for a quantization boundary.
/// Each field represents the execution sample at which the op should fire.
struct PendingTimedOp {
    int64_t executeSample = 0;
    Quantize quantize = Quantize::Bar;
};

/// Pending undo/redo with a count (last-wins for direction)
struct PendingUndo {
    int64_t executeSample = 0;
    Quantize quantize = Quantize::Bar;
    int count = 1;
    UndoDirection direction = UndoDirection::Undo;
};

/// Pending speed change with target value
struct PendingSpeed {
    int64_t executeSample = 0;
    Quantize quantize = Quantize::Bar;
    double speed = 1.0;
};

/// Pending capture with lookback duration
struct PendingCapture {
    int64_t executeSample = 0;
    Quantize quantize = Quantize::Bar;
    int64_t lookbackSamples = 0;
};

/// Parameters for scramble mode
struct ScrambleParams {
    double windowDuration = 1.0;   // Length of each snippet in beats
    double fadeDuration = 0.125;   // Fade in/out duration in beats
    bool allowRepeat = true;       // Whether the same start position can repeat
};

/// Pending scramble on/off with parameters
struct PendingScramble {
    int64_t executeSample = 0;
    Quantize quantize = Quantize::Bar;
    bool enable = true;            // true = turn on, false = turn off
    ScrambleParams params;
};

/// All pending state for a single loop, organized by independent slots.
/// Within each slot, only one operation can be pending (last-wins).
struct PendingState {
    std::optional<PendingTimedOp> mute;      // Mute/Unmute/ToggleMute
    std::optional<PendingTimedOp> overdub;    // StartOverdub/StopOverdub
    std::optional<PendingTimedOp> reverse;    // Reverse
    std::optional<PendingUndo>    undo;       // UndoLayer/RedoLayer
    std::optional<PendingSpeed>   speed;      // SetSpeed
    std::optional<PendingTimedOp> clear;      // ClearLoop
    std::optional<PendingCapture> capture;    // CaptureLoop
    std::optional<PendingTimedOp> record;     // Record/StopRecord
    std::optional<PendingScramble> scramble;  // ScrambleOn/ScrambleOff

    /// Which mute op: Mute, Unmute, or ToggleMute
    enum class MuteOp { Mute, Unmute, Toggle } muteOp = MuteOp::Toggle;

    /// Which overdub op: Start or Stop
    enum class OverdubOp { Start, Stop } overdubOp = OverdubOp::Start;

    /// Which record op: Start or Stop
    enum class RecordOp { Start, Stop } recordOp = RecordOp::Start;

    bool hasAny() const {
        return mute || overdub || reverse || undo || speed || clear || capture || record || scramble;
    }

    void clearAll() {
        mute.reset();
        overdub.reset();
        reverse.reset();
        undo.reset();
        speed.reset();
        clear.reset();
        capture.reset();
        record.reset();
        scramble.reset();
    }
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

    // Move-only (due to unique_ptr<TimeStretcher>)
    Loop(Loop&&) noexcept;
    Loop& operator=(Loop&&) noexcept;
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    /// Initialize the loop with audio captured from the ring buffer.
    /// This sets the loop length and creates the first layer.
    void loadFromCapture(std::vector<float> audio);

    /// Initialize the loop for chunked capture. Sets up a zero-filled buffer
    /// of the given length. Audio is filled progressively via addCaptureChunk().
    void initForCapture(int64_t lengthSamples);

    /// Accumulate a chunk of captured audio into the first layer.
    /// Called repeatedly during chunked capture to fill in audio progressively.
    void addCaptureChunk(const float* data, int64_t offset, int64_t count);

    /// Replace the first layer's audio buffer (O(1) pointer swap).
    /// Used to swap in a completed background capture.
    void replaceFirstLayerAudio(std::vector<float> audio);

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
    bool isReversed() const { return reversed_; }
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
    const PendingState& pendingState() const { return pending_; }
    PendingState& pendingState() { return pending_; }
    bool hasPendingOps() const { return pending_.hasAny(); }
    void clearPendingOps() { pending_.clearAll(); }

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

    /// Set the sample rate (needed for stretcher initialization)
    void setSampleRate(double sr) { sampleRate_ = sr; }

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

    /// Fill the stretch output buffer with another block of stretched audio
    void fillStretchBuffer();

    std::vector<LoopLayer> layers_;
    LoopState state_ = LoopState::Empty;
    int64_t loopLength_ = 0;
    int64_t playPos_ = 0;
    bool reversed_ = false;
    double speed_ = 1.0;
    double fractionalPos_ = 0.0;  // For non-integer speed ratios
    int crossfadeSamples_ = 256;
    double lengthInBars_ = 0.0;
    int id_ = -1;
    PendingState pending_;

    // Time stretch state
    double recordedBpm_ = 0.0;
    double currentBpm_ = 0.0;
    double sampleRate_ = 44100.0;

    std::unique_ptr<TimeStretcher> stretcher_;

    // Stretch output ring buffer
    std::vector<float> stretchBuf_;
    int stretchBufRead_ = 0;
    int stretchBufAvail_ = 0;

    // Raw read position for feeding the stretcher (tracks progress through loop)
    int64_t stretchRawPos_ = 0;

    // Pre-allocated work buffers (avoid allocation during processing)
    std::vector<float> stretchInputWork_;
    std::vector<float> stretchOutputWork_;

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

    static constexpr int kStretchBlockSize = 512;
    static constexpr int kStretchBufCapacity = 8192;
    static constexpr int kMaxStretchInput = kStretchBlockSize * 4;
};

} // namespace retrospect
