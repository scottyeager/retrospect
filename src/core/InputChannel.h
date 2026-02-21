#pragma once

#include "core/RingBuffer.h"

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace retrospect {

/// An input channel with its own ring buffer and live-activity detection.
///
/// Activity is tracked using a block-based peak tracker: the activity window
/// is divided into small blocks, each storing the peak absolute sample value.
/// A channel is considered "live" if the peak over the entire window exceeds
/// a configurable threshold.
class InputChannel {
public:
    /// @param ringCapacity  Ring buffer capacity in samples
    /// @param activityWindowSamples  Activity detection window size in samples
    InputChannel(int64_t ringCapacity, int activityWindowSamples);

    /// Write a single sample. Updates the ring buffer and peak tracker.
    void writeSample(float sample);

    /// Current peak level over the activity window.
    float peakLevel() const;

    /// Is this channel live (peak exceeds threshold)?
    /// If threshold <= 0, always returns true (disabled).
    bool isLive(float threshold) const;

    /// Access the underlying ring buffer (for capture)
    RingBuffer& ringBuffer() { return ringBuffer_; }
    const RingBuffer& ringBuffer() const { return ringBuffer_; }

private:
    RingBuffer ringBuffer_;

    // Block-based peak tracking.
    // The activity window is divided into blocks of kBlockSize samples.
    // Each block stores the peak |sample| encountered during that block.
    static constexpr int kBlockSize = 64;
    std::vector<float> blockPeaks_;   // Circular buffer of block peaks
    int blockWritePos_ = 0;
    float currentBlockPeak_ = 0.0f;   // Peak of the current (partial) block
    int sampleInBlock_ = 0;
    float cachedPeak_ = 0.0f;         // Cached max of all stored block peaks
};

} // namespace retrospect
