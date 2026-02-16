#include "core/RingBuffer.h"
#include <algorithm>
#include <cstring>

namespace retrospect {

RingBuffer::RingBuffer(int64_t capacitySamples)
    : buffer_(static_cast<size_t>(capacitySamples), 0.0f)
{
}

void RingBuffer::write(const float* data, int numSamples) {
    if (numSamples <= 0) return;

    int64_t cap = capacity();

    if (numSamples >= cap) {
        // Writing more than the buffer can hold: only keep the tail
        const float* start = data + (numSamples - cap);
        std::memcpy(buffer_.data(), start, static_cast<size_t>(cap) * sizeof(float));
        writePos_ = 0;
    } else {
        int64_t spaceToEnd = cap - writePos_;
        if (numSamples <= spaceToEnd) {
            std::memcpy(buffer_.data() + writePos_, data,
                        static_cast<size_t>(numSamples) * sizeof(float));
        } else {
            // Wrap around
            std::memcpy(buffer_.data() + writePos_, data,
                        static_cast<size_t>(spaceToEnd) * sizeof(float));
            int64_t remaining = numSamples - spaceToEnd;
            std::memcpy(buffer_.data(), data + spaceToEnd,
                        static_cast<size_t>(remaining) * sizeof(float));
        }
        writePos_ = (writePos_ + numSamples) % cap;
    }

    totalWritten_ += numSamples;
}

void RingBuffer::readMostRecent(float* dest, int numSamples) const {
    readFromPast(dest, numSamples, static_cast<int64_t>(numSamples));
}

void RingBuffer::readFromPast(float* dest, int numSamples, int64_t samplesAgo) const {
    if (numSamples <= 0) return;

    int64_t cap = capacity();
    int64_t avail = available();

    // Clamp to available data
    if (samplesAgo > avail) samplesAgo = avail;
    if (numSamples > samplesAgo) {
        // Zero-fill the beginning, then copy what we have
        int64_t zeroCount = numSamples - samplesAgo;
        std::memset(dest, 0, static_cast<size_t>(zeroCount) * sizeof(float));
        dest += zeroCount;
        numSamples = static_cast<int>(samplesAgo);
    }

    // Start reading from (writePos_ - samplesAgo), wrapping around
    int64_t readStart = (writePos_ - samplesAgo + cap * 2) % cap;

    int64_t spaceToEnd = cap - readStart;
    if (numSamples <= spaceToEnd) {
        std::memcpy(dest, buffer_.data() + readStart,
                    static_cast<size_t>(numSamples) * sizeof(float));
    } else {
        std::memcpy(dest, buffer_.data() + readStart,
                    static_cast<size_t>(spaceToEnd) * sizeof(float));
        int64_t remaining = numSamples - spaceToEnd;
        std::memcpy(dest + spaceToEnd, buffer_.data(),
                    static_cast<size_t>(remaining) * sizeof(float));
    }
}

std::vector<float> RingBuffer::capture(int numSamples) const {
    std::vector<float> result(static_cast<size_t>(numSamples), 0.0f);
    readMostRecent(result.data(), numSamples);
    return result;
}

void RingBuffer::clear() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    writePos_ = 0;
    totalWritten_ = 0;
}

} // namespace retrospect
