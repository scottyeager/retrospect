#include "core/Loop.h"
#include "core/StretchWorker.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace retrospect {

Loop::Loop() = default;
Loop::~Loop() {
    stopStretchWorker();
}

Loop::Loop(Loop&& other) noexcept
    : layers_(std::move(other.layers_))
    , preRecordSnapshot_(std::move(other.preRecordSnapshot_))
    , state_(other.state_)
    , loopLength_(other.loopLength_)
    , playPos_(other.playPos_)
    , reversed_(other.reversed_.load(std::memory_order_relaxed))
    , speed_(other.speed_)
    , fractionalPos_(other.fractionalPos_)
    , crossfadeSamples_(other.crossfadeSamples_)
    , lengthInBars_(other.lengthInBars_)
    , id_(other.id_)
    , pending_(std::move(other.pending_))
    , recordedBpm_(other.recordedBpm_)
    , currentBpm_(other.currentBpm_)
    , sampleRate_(other.sampleRate_)
    , stretchRawPos_(other.stretchRawPos_.load(std::memory_order_relaxed))
    , scrambleActive_(other.scrambleActive_)
    , scrambleParams_(other.scrambleParams_)
    , scrambleSamplesPerBeat_(other.scrambleSamplesPerBeat_)
    , scrambleSnippetPos_(other.scrambleSnippetPos_)
    , scrambleSnippetLen_(other.scrambleSnippetLen_)
    , scrambleReadStart_(other.scrambleReadStart_)
    , scrambleFadeSamples_(other.scrambleFadeSamples_)
    , scrambleLastStart_(other.scrambleLastStart_)
    , scrambleRng_(other.scrambleRng_)
{
    // Stop the worker before moving ownership so the feed callback's captured
    // `this` pointer (pointing at `other`) is no longer in use.
    other.stopStretchWorker();
    stretchWorker_ = std::move(other.stretchWorker_);
    other.state_ = LoopState::Empty;
    other.loopLength_ = 0;
}

Loop& Loop::operator=(Loop&& other) noexcept {
    if (this != &other) {
        stopStretchWorker();
        layers_ = std::move(other.layers_);
        preRecordSnapshot_ = std::move(other.preRecordSnapshot_);
        state_ = other.state_;
        loopLength_ = other.loopLength_;
        playPos_ = other.playPos_;
        reversed_.store(other.reversed_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        speed_ = other.speed_;
        fractionalPos_ = other.fractionalPos_;
        crossfadeSamples_ = other.crossfadeSamples_;
        lengthInBars_ = other.lengthInBars_;
        id_ = other.id_;
        pending_ = std::move(other.pending_);
        recordedBpm_ = other.recordedBpm_;
        currentBpm_ = other.currentBpm_;
        sampleRate_ = other.sampleRate_;
        other.stopStretchWorker();
        stretchWorker_ = std::move(other.stretchWorker_);
        stretchRawPos_.store(other.stretchRawPos_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        scrambleActive_ = other.scrambleActive_;
        scrambleParams_ = other.scrambleParams_;
        scrambleSamplesPerBeat_ = other.scrambleSamplesPerBeat_;
        scrambleSnippetPos_ = other.scrambleSnippetPos_;
        scrambleSnippetLen_ = other.scrambleSnippetLen_;
        scrambleReadStart_ = other.scrambleReadStart_;
        scrambleFadeSamples_ = other.scrambleFadeSamples_;
        scrambleLastStart_ = other.scrambleLastStart_;
        scrambleRng_ = other.scrambleRng_;
        other.state_ = LoopState::Empty;
        other.loopLength_ = 0;
    }
    return *this;
}

void Loop::loadFromCapture(std::vector<float> audio) {
    clear();
    loopLength_ = static_cast<int64_t>(audio.size());
    layers_.push_back({std::move(audio), 1.0f, true});
    state_ = LoopState::Playing;
    playPos_ = 0;
    fractionalPos_ = 0.0;
    stretchRawPos_.store(0, std::memory_order_relaxed);
}

void Loop::addLayer(std::vector<float> audio) {
    if (loopLength_ == 0) return;
    // Resize to match loop length
    audio.resize(static_cast<size_t>(loopLength_), 0.0f);
    layers_.push_back({std::move(audio), 1.0f, true});
}

bool Loop::undoLayer() {
    // Deactivate the most recent active layer (excluding the base layer)
    for (int i = static_cast<int>(layers_.size()) - 1; i > 0; --i) {
        if (layers_[static_cast<size_t>(i)].active) {
            layers_[static_cast<size_t>(i)].active = false;
            return true;
        }
    }
    return false;
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

void Loop::savePreRecordSnapshot() {
    if (state_ == LoopState::Empty) return;
    PreRecordSnapshot snap;
    snap.layers = layers_;
    snap.loopLength = loopLength_;
    snap.lengthInBars = lengthInBars_;
    snap.recordedBpm = recordedBpm_;
    snap.state = (state_ == LoopState::Muted) ? LoopState::Muted : LoopState::Playing;
    preRecordSnapshot_ = std::move(snap);
}

void Loop::restorePreRecordSnapshot() {
    if (!preRecordSnapshot_) return;
    stopStretchWorker();

    auto& snap = *preRecordSnapshot_;
    layers_ = std::move(snap.layers);
    loopLength_ = snap.loopLength;
    lengthInBars_ = snap.lengthInBars;
    recordedBpm_ = snap.recordedBpm;
    state_ = snap.state;

    // Reset playback position
    playPos_ = 0;
    fractionalPos_ = 0.0;
    stretchRawPos_.store(0, std::memory_order_relaxed);

    // Clear scramble state
    scrambleActive_ = false;
    scrambleSnippetPos_ = 0;
    scrambleSnippetLen_ = 0;
    scrambleReadStart_ = 0;
    scrambleFadeSamples_ = 0;
    scrambleLastStart_ = -1;

    preRecordSnapshot_.reset();
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
    if (reversed_.load(std::memory_order_relaxed)) {
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
    if (!stretchWorker_) return 0.0f;
    if (!stretchWorker_->isActive()) {
        startStretchWorker();
    }

    // Update tempo ratio for the worker
    double tempoRatio = std::clamp(currentBpm_ / recordedBpm_, 0.25, 4.0);
    stretchWorker_->setTempoRatio(tempoRatio);

    // Read from the worker's lock-free buffer
    int avail = stretchWorker_->available();
    int needed = static_cast<int>(std::ceil(speed_)) + 1;

    if (avail < needed) {
        // Buffer underrun — worker hasn't produced enough yet.
        // Signal it and output the last available sample or silence.
        stretchWorker_->requestMore();
        if (avail == 0) return 0.0f;
    }

    float sample = stretchWorker_->readSample();

    // Advance through stretch buffer at the user's speed_ rate
    fractionalPos_ += speed_;
    int advance = static_cast<int>(fractionalPos_);
    fractionalPos_ -= static_cast<double>(advance);

    // Skip additional samples if speed > 1
    for (int i = 1; i < advance; ++i) {
        if (stretchWorker_->available() > 0) {
            stretchWorker_->readSample();
        }
    }

    // Signal worker if buffer is getting low
    if (stretchWorker_->available() < StretchWorker::kFillThreshold) {
        stretchWorker_->requestMore();
    }

    // Update playPos_ for display purposes (approximate raw loop position)
    playPos_ = stretchRawPos_.load(std::memory_order_relaxed) % loopLength_;

    return sample;
}

void Loop::setSampleRate(double sr) {
    sampleRate_ = sr;
    if (!stretchWorker_) {
        stretchWorker_ = std::make_unique<StretchWorker>(sr);
    }
}

void Loop::startStretchWorker() {
    if (!stretchWorker_ || stretchWorker_->isActive()) return;
    if (loopLength_ <= 0) return;

    // Capture state needed by the feed callback. The callback runs on the
    // worker thread and reads loop layer data (benign race — layer audio is
    // immutable after creation, active flag toggles cause at most one brief
    // stretch block of slightly stale mix).
    auto* self = this;
    auto feedCb = [self](float* output, int count) {
        int64_t rawPos = self->stretchRawPos_.load(std::memory_order_relaxed);
        bool rev = self->reversed_.load(std::memory_order_relaxed);
        int64_t len = self->loopLength_;

        for (int i = 0; i < count; ++i) {
            int64_t pos;
            if (rev) {
                int64_t rawMod = rawPos % len;
                pos = len - 1 - rawMod;
            } else {
                pos = rawPos % len;
            }
            output[i] = self->getMixedSample(pos) * self->crossfadeGain(pos);
            rawPos = (rawPos + 1) % len;
        }

        self->stretchRawPos_.store(rawPos, std::memory_order_relaxed);
    };

    stretchWorker_->start(std::move(feedCb));
}

void Loop::stopStretchWorker() {
    if (stretchWorker_) {
        stretchWorker_->stop();
    }
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

    // Reset stretch worker if active
    if (isTimeStretchActive() && stretchWorker_) {
        stretchRawPos_.store(playPos_, std::memory_order_relaxed);
        stopStretchWorker();
        // Will be restarted on next processStretchedSample()
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
    bool rev = reversed_.load(std::memory_order_relaxed);
    if (isTimeStretchActive()) {
        // During overdub with stretching, record at the raw position the
        // stretcher is consuming from, so the overdub aligns with the raw loop data
        int64_t rawMod = stretchRawPos_.load(std::memory_order_relaxed) % loopLength_;
        pos = rev ? (loopLength_ - 1 - rawMod) : rawMod;
    } else {
        pos = rev ? (loopLength_ - 1 - playPos_) : playPos_;
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
    reversed_.store(!reversed_.load(std::memory_order_relaxed), std::memory_order_relaxed);
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
        stretchRawPos_.store(playPos_, std::memory_order_relaxed);
        fractionalPos_ = 0.0;
        // Worker will be started lazily on first processStretchedSample()
    } else if (wasActive && !nowActive) {
        // Transitioning back to direct mode
        playPos_ = stretchRawPos_.load(std::memory_order_relaxed) % loopLength_;
        fractionalPos_ = 0.0;
        stopStretchWorker();
    }
    // If stretching stays active, the worker picks up the new ratio via atomic
}

bool Loop::isTimeStretchActive() const {
    return !isEmpty() && recordedBpm_ > 0.0 && currentBpm_ > 0.0 &&
           std::abs(currentBpm_ - recordedBpm_) > 0.5;
}

int64_t Loop::playPosition() const {
    if (isTimeStretchActive()) {
        return stretchRawPos_.load(std::memory_order_relaxed) % loopLength_;
    }
    return playPos_;
}

void Loop::clear() {
    stopStretchWorker();

    layers_.clear();
    preRecordSnapshot_.reset();
    state_ = LoopState::Empty;
    loopLength_ = 0;
    playPos_ = 0;
    fractionalPos_ = 0.0;
    reversed_.store(false, std::memory_order_relaxed);
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
    stretchRawPos_.store(0, std::memory_order_relaxed);
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
