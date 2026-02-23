#include "core/Loop.h"
#include "core/TimeStretcher.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace retrospect {

Loop::Loop() = default;
Loop::~Loop() = default;
Loop::Loop(Loop&&) noexcept = default;
Loop& Loop::operator=(Loop&&) noexcept = default;

void Loop::loadFromCapture(std::vector<float> audio) {
    clear();
    loopLength_ = static_cast<int64_t>(audio.size());
    layers_.push_back({std::move(audio), 1.0f, true});
    state_ = LoopState::Playing;
    playPos_ = 0;
    fractionalPos_ = 0.0;

    // Pre-allocate stretch resources so we don't allocate during playback
    stretcher_ = std::make_unique<TimeStretcher>();
    stretcher_->configure(sampleRate_);
    stretchBuf_.resize(static_cast<size_t>(kStretchBufCapacity), 0.0f);
    stretchInputWork_.resize(static_cast<size_t>(kMaxStretchInput), 0.0f);
    stretchOutputWork_.resize(static_cast<size_t>(kStretchBlockSize), 0.0f);
    stretchBufRead_ = 0;
    stretchBufAvail_ = 0;
    stretchRawPos_ = 0;
}

void Loop::addLayer(std::vector<float> audio) {
    if (loopLength_ == 0) return;
    // Resize to match loop length
    audio.resize(static_cast<size_t>(loopLength_), 0.0f);
    layers_.push_back({std::move(audio), 1.0f, true});
}

void Loop::undoLayer() {
    // Deactivate the most recent active layer (excluding the base layer)
    for (int i = static_cast<int>(layers_.size()) - 1; i > 0; --i) {
        if (layers_[static_cast<size_t>(i)].active) {
            layers_[static_cast<size_t>(i)].active = false;
            return;
        }
    }
}

void Loop::redoLayer() {
    // Reactivate the earliest inactive layer
    for (size_t i = 1; i < layers_.size(); ++i) {
        if (!layers_[i].active) {
            layers_[i].active = true;
            return;
        }
    }
}

float Loop::getMixedSample(int64_t pos) const {
    if (pos < 0 || pos >= loopLength_) return 0.0f;

    float mix = 0.0f;
    for (const auto& layer : layers_) {
        if (layer.active) {
            mix += layer.audio[static_cast<size_t>(pos)] * layer.gain;
        }
    }
    return mix;
}

float Loop::crossfadeGain(int64_t pos) const {
    if (crossfadeSamples_ <= 0 || loopLength_ <= crossfadeSamples_ * 2) {
        return 1.0f;
    }

    // Fade in at start of loop
    if (pos < crossfadeSamples_) {
        return static_cast<float>(pos) / static_cast<float>(crossfadeSamples_);
    }
    // Fade out at end of loop
    int64_t distFromEnd = loopLength_ - 1 - pos;
    if (distFromEnd < crossfadeSamples_) {
        return static_cast<float>(distFromEnd) / static_cast<float>(crossfadeSamples_);
    }
    return 1.0f;
}

float Loop::processSample() {
    if (state_ == LoopState::Empty || state_ == LoopState::Muted) {
        return 0.0f;
    }

    if (isTimeStretchActive()) {
        return processStretchedSample();
    }
    return processDirectSample();
}

float Loop::processDirectSample() {
    int64_t readPos;
    if (reversed_) {
        readPos = loopLength_ - 1 - playPos_;
    } else {
        readPos = playPos_;
    }

    float sample = getMixedSample(readPos) * crossfadeGain(readPos);

    // Advance position
    fractionalPos_ += speed_;
    int64_t advance = static_cast<int64_t>(fractionalPos_);
    fractionalPos_ -= static_cast<double>(advance);
    playPos_ = (playPos_ + advance) % loopLength_;

    return sample;
}

float Loop::processStretchedSample() {
    // Ensure we have enough stretched samples in the buffer.
    // At max speed (4x), we consume up to 4 samples per call.
    int needed = static_cast<int>(std::ceil(speed_)) + 1;
    while (stretchBufAvail_ < needed) {
        fillStretchBuffer();
    }

    // Read from stretch buffer
    float sample = stretchBuf_[static_cast<size_t>(stretchBufRead_)];

    // Advance through stretch buffer at the user's speed_ rate.
    // This is where speed_ affects both speed and pitch (on top of stretching).
    fractionalPos_ += speed_;
    int advance = static_cast<int>(fractionalPos_);
    fractionalPos_ -= static_cast<double>(advance);

    stretchBufRead_ = (stretchBufRead_ + advance) % kStretchBufCapacity;
    stretchBufAvail_ -= advance;

    // Update playPos_ for display purposes (approximate raw loop position)
    playPos_ = stretchRawPos_ % loopLength_;

    return sample;
}

void Loop::fillStretchBuffer() {
    if (!stretcher_ || !stretcher_->isConfigured()) return;
    if (recordedBpm_ <= 0.0 || currentBpm_ <= 0.0) return;

    // Tempo ratio: >1.0 means current tempo is faster, need more input per output
    double tempoRatio = std::clamp(currentBpm_ / recordedBpm_, 0.25, 4.0);

    // How many raw input samples we need to produce kStretchBlockSize output samples
    int inputNeeded = static_cast<int>(std::ceil(kStretchBlockSize * tempoRatio));
    inputNeeded = std::clamp(inputNeeded, 1, kMaxStretchInput);

    // Read raw samples from loop layers into pre-allocated work buffer
    for (int i = 0; i < inputNeeded; ++i) {
        int64_t pos;
        if (reversed_) {
            // When reversed, read backwards through the loop
            int64_t rawMod = stretchRawPos_ % loopLength_;
            pos = loopLength_ - 1 - rawMod;
        } else {
            pos = stretchRawPos_ % loopLength_;
        }
        stretchInputWork_[static_cast<size_t>(i)] =
            getMixedSample(pos) * crossfadeGain(pos);
        stretchRawPos_ = (stretchRawPos_ + 1) % loopLength_;
    }

    // Process through stretcher (no allocation)
    stretcher_->process(stretchInputWork_.data(), inputNeeded,
                        stretchOutputWork_.data(), kStretchBlockSize);

    // Write to circular output buffer
    for (int i = 0; i < kStretchBlockSize; ++i) {
        int writeIdx = (stretchBufRead_ + stretchBufAvail_ + i) % kStretchBufCapacity;
        stretchBuf_[static_cast<size_t>(writeIdx)] = stretchOutputWork_[static_cast<size_t>(i)];
    }
    stretchBufAvail_ += kStretchBlockSize;
}

void Loop::processBlock(float* output, int numSamples) {
    for (int i = 0; i < numSamples; ++i) {
        output[i] += processSample();
    }
}

void Loop::recordSample(float input) {
    if (state_ != LoopState::Recording || layers_.empty()) return;

    auto& recordLayer = layers_.back();
    int64_t pos;
    if (isTimeStretchActive()) {
        // During overdub with stretching, record at the raw position the
        // stretcher is consuming from, so the overdub aligns with the raw loop data
        int64_t rawMod = stretchRawPos_ % loopLength_;
        pos = reversed_ ? (loopLength_ - 1 - rawMod) : rawMod;
    } else {
        pos = reversed_ ? (loopLength_ - 1 - playPos_) : playPos_;
    }
    if (pos >= 0 && pos < loopLength_) {
        recordLayer.audio[static_cast<size_t>(pos)] += input;
    }
}

void Loop::play() {
    if (state_ != LoopState::Empty) {
        state_ = LoopState::Playing;
    }
}

void Loop::mute() {
    if (state_ != LoopState::Empty) {
        state_ = LoopState::Muted;
    }
}

void Loop::toggleMute() {
    if (state_ == LoopState::Playing) {
        state_ = LoopState::Muted;
    } else if (state_ == LoopState::Muted) {
        state_ = LoopState::Playing;
    }
}

void Loop::startOverdub() {
    if (state_ == LoopState::Empty || loopLength_ == 0) return;
    // Create a new empty layer for recording
    std::vector<float> newLayer(static_cast<size_t>(loopLength_), 0.0f);
    layers_.push_back({std::move(newLayer), 1.0f, true});
    state_ = LoopState::Recording;
}

void Loop::stopOverdub() {
    if (state_ == LoopState::Recording) {
        state_ = LoopState::Playing;
    }
}

void Loop::toggleReverse() {
    reversed_ = !reversed_;
}

void Loop::setSpeed(double spd) {
    speed_ = std::max(0.25, std::min(spd, 4.0));
}

void Loop::setCurrentBpm(double bpm) {
    bool wasActive = isTimeStretchActive();
    currentBpm_ = bpm;
    bool nowActive = isTimeStretchActive();

    if (!wasActive && nowActive) {
        // Transitioning from direct to stretched mode
        stretchRawPos_ = playPos_;
        stretchBufRead_ = 0;
        stretchBufAvail_ = 0;
        fractionalPos_ = 0.0;
        if (stretcher_) stretcher_->reset();
    } else if (wasActive && !nowActive) {
        // Transitioning back to direct mode
        playPos_ = stretchRawPos_ % loopLength_;
        fractionalPos_ = 0.0;
    }
}

bool Loop::isTimeStretchActive() const {
    return !isEmpty() && recordedBpm_ > 0.0 && currentBpm_ > 0.0 &&
           std::abs(currentBpm_ - recordedBpm_) > 0.5;
}

int64_t Loop::playPosition() const {
    if (isTimeStretchActive()) {
        return stretchRawPos_ % loopLength_;
    }
    return playPos_;
}

void Loop::setPlayPosition(int64_t pos) {
    if (loopLength_ <= 0) return;
    playPos_ = pos % loopLength_;
    stretchRawPos_ = playPos_;
    fractionalPos_ = 0.0;
}

void Loop::clear() {
    layers_.clear();
    state_ = LoopState::Empty;
    loopLength_ = 0;
    playPos_ = 0;
    fractionalPos_ = 0.0;
    reversed_ = false;
    speed_ = 1.0;
    lengthInBars_ = 0.0;

    // Clear stretch state
    stretcher_.reset();
    stretchBuf_.clear();
    stretchInputWork_.clear();
    stretchOutputWork_.clear();
    stretchBufRead_ = 0;
    stretchBufAvail_ = 0;
    stretchRawPos_ = 0;
    recordedBpm_ = 0.0;
}

int Loop::activeLayerCount() const {
    int count = 0;
    for (const auto& layer : layers_) {
        if (layer.active) ++count;
    }
    return count;
}

} // namespace retrospect
