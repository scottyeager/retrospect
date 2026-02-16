#pragma once

#include <cstdint>
#include <functional>

namespace retrospect {

/// Quantization boundary for operations
enum class Quantize {
    Free,  // No quantization - execute immediately
    Beat,  // Snap to next beat boundary
    Bar    // Snap to next bar boundary
};

/// Position within the metronome's timeline
struct MetronomePosition {
    int64_t totalSamples = 0;   // Total samples elapsed since start
    int bar = 0;                // Current bar (0-indexed)
    int beat = 0;               // Current beat within bar (0-indexed)
    double beatFraction = 0.0;  // Fractional position within current beat [0, 1)

    /// Absolute beat number from start
    int64_t absoluteBeat() const { return static_cast<int64_t>(bar) * 4 + beat; }
};

/// Internal metronome that tracks tempo and provides beat/bar positions.
/// Designed to be advanced sample-by-sample from an audio callback or
/// simulation loop.
class Metronome {
public:
    using BeatCallback = std::function<void(const MetronomePosition&)>;
    using BarCallback = std::function<void(const MetronomePosition&)>;

    Metronome(double bpm = 120.0, int beatsPerBar = 4, double sampleRate = 44100.0);

    /// Advance the metronome by a number of samples. Fires callbacks on
    /// beat and bar boundaries crossed during this advance.
    void advance(int numSamples);

    /// Reset to the beginning
    void reset();

    /// Current position
    MetronomePosition position() const;

    /// Returns the sample index of the next beat boundary from the current position
    int64_t nextBeatSample() const;

    /// Returns the sample index of the next bar boundary from the current position
    int64_t nextBarSample() const;

    /// Returns samples remaining until the next quantization boundary
    int64_t samplesUntilBoundary(Quantize q) const;

    /// Number of samples per beat at current tempo
    double samplesPerBeat() const;

    /// Number of samples per bar at current tempo
    double samplesPerBar() const;

    // Configuration
    void setBpm(double bpm);
    double bpm() const { return bpm_; }

    void setBeatsPerBar(int beats);
    int beatsPerBar() const { return beatsPerBar_; }

    void setSampleRate(double rate);
    double sampleRate() const { return sampleRate_; }

    bool isRunning() const { return running_; }
    void setRunning(bool run) { running_ = run; }

    /// Register callbacks for beat and bar boundaries
    void onBeat(BeatCallback cb) { beatCallback_ = std::move(cb); }
    void onBar(BarCallback cb) { barCallback_ = std::move(cb); }

private:
    void recalculate();
    void updatePosition();

    double bpm_;
    int beatsPerBar_;
    double sampleRate_;
    bool running_ = true;

    double samplesPerBeat_ = 0;
    double samplesPerBar_ = 0;

    int64_t totalSamples_ = 0;

    BeatCallback beatCallback_;
    BarCallback barCallback_;
};

} // namespace retrospect
