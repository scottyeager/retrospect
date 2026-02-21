#include "core/InputChannel.h"

#include <algorithm>
#include <cmath>

namespace retrospect {

InputChannel::InputChannel(int64_t ringCapacity, int activityWindowSamples)
    : ringBuffer_(ringCapacity)
    , blockPeaks_(static_cast<size_t>(
          std::max(1, activityWindowSamples / kBlockSize)), 0.0f)
{
}

void InputChannel::writeSample(float sample) {
    ringBuffer_.write(&sample, 1);

    float absSample = std::abs(sample);
    if (absSample > currentBlockPeak_) {
        currentBlockPeak_ = absSample;
    }

    if (++sampleInBlock_ >= kBlockSize) {
        // Store completed block's peak
        blockPeaks_[static_cast<size_t>(blockWritePos_)] = currentBlockPeak_;
        blockWritePos_ = (blockWritePos_ + 1) %
                         static_cast<int>(blockPeaks_.size());

        // Recompute cached peak from all stored block peaks
        cachedPeak_ = 0.0f;
        for (float p : blockPeaks_) {
            if (p > cachedPeak_) cachedPeak_ = p;
        }

        currentBlockPeak_ = 0.0f;
        sampleInBlock_ = 0;
    }
}

float InputChannel::peakLevel() const {
    return std::max(cachedPeak_, currentBlockPeak_);
}

bool InputChannel::isLive(float threshold) const {
    if (threshold <= 0.0f) return true;
    return peakLevel() > threshold;
}

} // namespace retrospect
