#pragma once

#include <memory>

namespace retrospect {

/// Wrapper around Signalsmith Stretch for pitch-preserving time stretching.
/// Uses pimpl to isolate the Signalsmith header from consumers.
class TimeStretcher {
public:
    TimeStretcher();
    ~TimeStretcher();

    // Non-copyable, movable
    TimeStretcher(const TimeStretcher&) = delete;
    TimeStretcher& operator=(const TimeStretcher&) = delete;
    TimeStretcher(TimeStretcher&&) noexcept;
    TimeStretcher& operator=(TimeStretcher&&) noexcept;

    /// Configure for mono audio at the given sample rate.
    /// Uses the "cheaper" preset for lower CPU and latency.
    void configure(double sampleRate);

    /// Process a block of audio through the stretcher.
    /// The ratio of inputSamples to outputSamples determines the time stretch.
    /// More input than output = speed up; less input = slow down.
    /// Pitch is preserved regardless of the ratio.
    void process(const float* input, int inputSamples,
                 float* output, int outputSamples);

    /// Reset internal state. Call when the input stream is discontinuous
    /// (e.g., loop wrap-around or stretch activation).
    void reset();

    /// Whether configure() has been called
    bool isConfigured() const { return configured_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool configured_ = false;
};

} // namespace retrospect
