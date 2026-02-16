#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace retrospect {

/// Circular buffer for continuous audio recording.
/// Stores mono float samples. Continuously overwrites oldest data.
class RingBuffer {
public:
    /// Create a ring buffer with the given capacity in samples
    explicit RingBuffer(int64_t capacitySamples);

    /// Write samples into the ring buffer
    void write(const float* data, int numSamples);

    /// Read the most recent `numSamples` samples into `dest`.
    /// If numSamples exceeds what's available, earlier samples are zero-filled.
    void readMostRecent(float* dest, int numSamples) const;

    /// Read `numSamples` starting from `samplesAgo` samples before the write head.
    /// samplesAgo=0 means the most recently written sample.
    void readFromPast(float* dest, int numSamples, int64_t samplesAgo) const;

    /// Copy a range of the ring buffer into a new vector.
    /// Captures the most recent `numSamples` samples.
    std::vector<float> capture(int numSamples) const;

    /// Total samples written since creation/reset
    int64_t totalWritten() const { return totalWritten_; }

    /// Capacity in samples
    int64_t capacity() const { return static_cast<int64_t>(buffer_.size()); }

    /// How many valid samples are available (min of totalWritten and capacity)
    int64_t available() const { return std::min(totalWritten_, capacity()); }

    /// Clear the buffer
    void clear();

private:
    std::vector<float> buffer_;
    int64_t writePos_ = 0;
    int64_t totalWritten_ = 0;
};

} // namespace retrospect
