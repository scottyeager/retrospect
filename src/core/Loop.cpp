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

    if (scrambleActive_) {
        return processScrambleSample();
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

float Loop::processScrambleSample() {
    // Pick a new snippet if we've exhausted the current one
    if (scrambleSnippetPos_ >= scrambleSnippetLen_) {
        // Compute snippet length from (possibly updated) window duration
        scrambleSnippetLen_ = std::max(int64_t(1),
            static_cast<int64_t>(scrambleParams_.windowDuration * scrambleSamplesPerBeat_));
        scrambleFadeSamples_ = std::max(int64_t(0),
            static_cast<int64_t>(scrambleParams_.fadeDuration * scrambleSamplesPerBeat_));

        // Pick a random start position
        std::uniform_int_distribution<int64_t> dist(0, loopLength_ - 1);
        int64_t newStart;
        if (!scrambleParams_.allowRepeat && loopLength_ > 1) {
            // Try up to a few times to avoid the same start
            for (int attempt = 0; attempt < 8; ++attempt) {
                newStart = dist(scrambleRng_);
                if (newStart != scrambleLastStart_) break;
            }
        } else {
            newStart = dist(scrambleRng_);
        }
        scrambleReadStart_ = newStart;
        scrambleLastStart_ = newStart;
        scrambleSnippetPos_ = 0;
    }

    // Compute read position (wrapping within loop)
    int64_t readPos = (scrambleReadStart_ + scrambleSnippetPos_) % loopLength_;

    // Compute scramble envelope (fade in/out)
    float envelope = 1.0f;
    if (scrambleFadeSamples_ > 0) {
        // Fade in
        if (scrambleSnippetPos_ < scrambleFadeSamples_) {
            float fadeIn = static_cast<float>(scrambleSnippetPos_) /
                           static_cast<float>(scrambleFadeSamples_);
            envelope = std::min(envelope, fadeIn);
        }
        // Fade out
        int64_t distFromEnd = scrambleSnippetLen_ - 1 - scrambleSnippetPos_;
        if (distFromEnd < scrambleFadeSamples_) {
            float fadeOut = static_cast<float>(distFromEnd) /
                            static_cast<float>(scrambleFadeSamples_);
            envelope = std::min(envelope, fadeOut);
        }
    }

    float sample = getMixedSample(readPos) * envelope;

    // Advance
    scrambleSnippetPos_++;
    // Keep playPos_ roughly in sync for display purposes
    playPos_ = readPos;

    return sample;
}

void Loop::seek(int64_t samplePos) {
    if (loopLength_ <= 0) return;
    playPos_ = ((samplePos % loopLength_) + loopLength_) % loopLength_;
    fractionalPos_ = 0.0;

    // Reset stretch buffer if active
    if (isTimeStretchActive()) {
        stretchRawPos_ = playPos_;
        stretchBufRead_ = 0;
        stretchBufAvail_ = 0;
        if (stretcher_) stretcher_->reset();
    }
}

void Loop::scrambleOn(const ScrambleParams& params, double samplesPerBeat) {
    if (state_ == LoopState::Empty || loopLength_ <= 0) return;
    scrambleActive_ = true;
    scrambleParams_ = params;
    scrambleSamplesPerBeat_ = samplesPerBeat;
    // Force picking a new snippet immediately
    scrambleSnippetPos_ = scrambleSnippetLen_;
    scrambleLastStart_ = -1;
}

void Loop::scrambleOff() {
    scrambleActive_ = false;
    // playPos_ is already kept in sync during scramble
}

void Loop::setScrambleWindowDuration(double beats) {
    scrambleParams_.windowDuration = std::max(0.0625, beats);
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

void Loop::clear() {
    layers_.clear();
    state_ = LoopState::Empty;
    loopLength_ = 0;
    playPos_ = 0;
    fractionalPos_ = 0.0;
    reversed_ = false;
    speed_ = 1.0;
    lengthInBars_ = 0.0;

    // Clear scramble state
    scrambleActive_ = false;
    scrambleSnippetPos_ = 0;
    scrambleSnippetLen_ = 0;
    scrambleReadStart_ = 0;
    scrambleFadeSamples_ = 0;
    scrambleLastStart_ = -1;

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
