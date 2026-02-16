#include "core/Loop.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace retrospect {

void Loop::loadFromCapture(std::vector<float> audio) {
    clear();
    loopLength_ = static_cast<int64_t>(audio.size());
    layers_.push_back({std::move(audio), 1.0f, true});
    state_ = LoopState::Playing;
    playPos_ = 0;
    fractionalPos_ = 0.0;
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

void Loop::processBlock(float* output, int numSamples) {
    for (int i = 0; i < numSamples; ++i) {
        output[i] += processSample();
    }
}

void Loop::recordSample(float input) {
    if (state_ != LoopState::Recording || layers_.empty()) return;

    auto& recordLayer = layers_.back();
    int64_t pos = reversed_ ? (loopLength_ - 1 - playPos_) : playPos_;
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

void Loop::clear() {
    layers_.clear();
    state_ = LoopState::Empty;
    loopLength_ = 0;
    playPos_ = 0;
    fractionalPos_ = 0.0;
    reversed_ = false;
    speed_ = 1.0;
    lengthInBars_ = 0.0;
}

int Loop::activeLayerCount() const {
    int count = 0;
    for (const auto& layer : layers_) {
        if (layer.active) ++count;
    }
    return count;
}

} // namespace retrospect
