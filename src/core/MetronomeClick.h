#pragma once

#include <cmath>
#include <atomic>

namespace retrospect {

/// Synthesizes a short percussive click for the metronome.
/// Produces a decaying sine wave (~30ms) — higher pitch on downbeats.
class MetronomeClick {
public:
    explicit MetronomeClick(double sampleRate = 44100.0)
        : sampleRate_(sampleRate) {}

    /// Start a click. Downbeats get higher frequency and slightly more volume.
    void trigger(bool isDownbeat) {
        if (!enabled_) return;
        phase_ = 0.0;
        sampleIndex_ = 0;
        active_ = true;
        freq_ = isDownbeat ? 1000.0 : 800.0;
        clickGain_ = isDownbeat ? 1.0f : 0.75f;
    }

    /// Return the next sample of the click (0.0f when inactive).
    float nextSample() {
        if (!active_) return 0.0f;

        double t = static_cast<double>(sampleIndex_) / sampleRate_;
        if (t >= duration_) {
            active_ = false;
            return 0.0f;
        }

        // Exponential decay envelope
        float envelope = std::exp(static_cast<float>(-t / decayTau_));

        // Sine oscillator
        float sample = std::sin(static_cast<float>(phase_)) * envelope;
        phase_ += 2.0 * M_PI * freq_ / sampleRate_;

        ++sampleIndex_;
        return sample * volume_ * clickGain_;
    }

    void setEnabled(bool on) { enabled_ = on; }
    bool isEnabled() const { return enabled_; }

    void setVolume(float v) { volume_ = v; }
    float volume() const { return volume_; }

    void setSampleRate(double sr) { sampleRate_ = sr; }

private:
    double sampleRate_;
    bool enabled_ = true;
    float volume_ = 0.5f;

    // Click state
    bool active_ = false;
    double phase_ = 0.0;
    double freq_ = 1000.0;
    float clickGain_ = 1.0f;
    int sampleIndex_ = 0;

    // ~30ms click duration, fast exponential decay
    static constexpr double duration_ = 0.03;
    static constexpr double decayTau_ = 0.006; // time constant for decay
};

} // namespace retrospect
