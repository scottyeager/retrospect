#include "core/LoopEngine.h"
#include "core/EngineCommand.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdio>

namespace retrospect {

// OpType description
std::string opTypeDescription(OpType type) {
    switch (type) {
        case OpType::CaptureLoop:  return "Capture Loop";
        case OpType::Record:       return "Record";
        case OpType::StopRecord:   return "Stop Record";
        case OpType::Mute:         return "Mute";
        case OpType::Unmute:       return "Unmute";
        case OpType::ToggleMute:   return "Toggle Mute";
        case OpType::Reverse:      return "Reverse";
        case OpType::StartOverdub: return "Start Overdub";
        case OpType::StopOverdub:  return "Stop Overdub";
        case OpType::UndoLayer:    return "Undo Layer";
        case OpType::RedoLayer:    return "Redo Layer";
        case OpType::SetSpeed:         return "Set Speed";
        case OpType::ClearLoop:        return "Clear";
        case OpType::Seek:             return "Seek";
        case OpType::ScrambleOn:       return "Scramble On";
        case OpType::ScrambleOff:      return "Scramble Off";
        case OpType::SetScrambleWindow: return "Set Scramble Window";
    }
    return "Unknown";
}

LoopEngine::LoopEngine(int maxLoops, int maxLookbackBars,
                       double sampleRate, double minBpm,
                       int numInputChannels, float liveThreshold,
                       int liveWindowMs)
    : metronome_(120.0, 4, sampleRate)
    , click_(sampleRate)
    , midiSync_(120.0, sampleRate)
    , loops_(static_cast<size_t>(maxLoops))
    , maxLookbackBars_(maxLookbackBars)
    , sampleRate_(sampleRate)
    , liveThreshold_(liveThreshold)
{
    // Size ring buffer for maxLookbackBars at the slowest expected tempo.
    // At minBpm, one beat = (60/minBpm) seconds, one bar = beatsPerBar beats.
    int64_t ringCapacity = static_cast<int64_t>(
        std::ceil(maxLookbackBars * 4 * (60.0 / minBpm) * sampleRate));
    int activityWindowSamples = static_cast<int>(
        sampleRate * static_cast<double>(liveWindowMs) / 1000.0);

    inputChannels_.reserve(static_cast<size_t>(numInputChannels));
    for (int i = 0; i < numInputChannels; ++i) {
        inputChannels_.emplace_back(ringCapacity, activityWindowSamples);
    }
    channelPeaksSnapshot_.resize(static_cast<size_t>(numInputChannels), 0.0f);
    lastThresholdBreachSample_.resize(static_cast<size_t>(numInputChannels), INT64_MIN);

    for (int i = 0; i < maxLoops; ++i) {
        loops_[static_cast<size_t>(i)].setId(i);
        loops_[static_cast<size_t>(i)].setCrossfadeSamples(crossfadeSamples_);
        loops_[static_cast<size_t>(i)].setSampleRate(sampleRate);
    }

    bgCaptures_.resize(static_cast<size_t>(maxLoops));

    // Wire metronome callbacks
    metronome_.onBeat([this](const MetronomePosition& pos) {
        click_.trigger(pos.beat == 0);
        if (callbacks_.onBeat) callbacks_.onBeat(pos);
    });
    metronome_.onBar([this](const MetronomePosition& pos) {
        if (callbacks_.onBar) callbacks_.onBar(pos);
    });
}

LoopEngine::~LoopEngine() {
    for (auto& bg : bgCaptures_) {
        if (bg && bg->thread.joinable()) {
            bg->cancelled.store(true, std::memory_order_release);
            bg->phase2Ready.store(true, std::memory_order_release);
            bg->thread.join();
        }
    }
    for (auto& bg : zombieCaptures_) {
        if (bg && bg->thread.joinable()) {
            bg->thread.join();
        }
    }
}

void LoopEngine::processBlock(const float* const* input, int inputChannelCount,
                              float* output, int numSamples) {
    // Drain commands from TUI thread at the start of each block
    drainCommands();

    // Update external transport (JACK BBT) with our metronome's position
    if (transportPositionCallback_) {
        transportPositionCallback_(metronome_.position().totalSamples);
    }

    int engineChannels = static_cast<int>(inputChannels_.size());

    for (int i = 0; i < numSamples; ++i) {
        // Write each input channel to its InputChannel and compute
        // an unfiltered input mix (all channels, no threshold).
        float inputMix = 0.0f;
        for (int ch = 0; ch < engineChannels; ++ch) {
            float sample = (ch < inputChannelCount && input && input[ch])
                ? input[ch][i] : 0.0f;
            inputChannels_[static_cast<size_t>(ch)].writeSample(sample);
            inputMix += sample;
        }

        // Accumulate per-channel audio into active classic recording
        if (activeRecording_) {
            for (int ch = 0; ch < engineChannels; ++ch) {
                float sample = (ch < inputChannelCount && input && input[ch])
                    ? input[ch][i] : 0.0f;
                activeRecording_->channelBuffers[static_cast<size_t>(ch)].push_back(sample);
            }
            // Sticky mask: once a channel breaches threshold, include it
            for (int ch = 0; ch < engineChannels && ch < 64; ++ch) {
                if (inputChannels_[static_cast<size_t>(ch)].isLive(liveThreshold_)) {
                    activeRecording_->activeChannelMask |= (uint64_t(1) << ch);
                }
            }
        }

        // Check each loop's pending state
        int64_t currentSample = metronome_.position().totalSamples;
        for (auto& lp : loops_) {
            if (lp.hasPendingOps()) {
                flushDueOps(lp, currentSample);
            }
        }

        // Apply pending MIDI sync toggle at the scheduled sample
        if (pendingMidiSync_ && pendingMidiSync_->first <= currentSample) {
            midiSync_.setEnabled(pendingMidiSync_->second);
            pendingMidiSync_.reset();
            if (callbacks_.onStateChanged) callbacks_.onStateChanged();
        }

        // Mix output from all playing loops
        float outSample = 0.0f;
        for (auto& lp : loops_) {
            if (!lp.isEmpty()) {
                outSample += lp.processSample();

                // Accumulate per-channel overdub audio
                if (lp.isRecording() && lp.id() == overdubLoopIndex_) {
                    int64_t pos = lp.playPosition();
                    if (pos >= 0 && pos < lp.lengthSamples()) {
                        for (int ch = 0; ch < engineChannels; ++ch) {
                            float sample = (ch < inputChannelCount && input && input[ch])
                                ? input[ch][i] : 0.0f;
                            overdubChannelBuffers_[static_cast<size_t>(ch)][static_cast<size_t>(pos)] += sample;
                        }
                        // Sticky per-layer mask
                        for (int ch = 0; ch < engineChannels && ch < 64; ++ch) {
                            if (inputChannels_[static_cast<size_t>(ch)].isLive(liveThreshold_)) {
                                overdubActiveChannelMask_ |= (uint64_t(1) << ch);
                            }
                        }
                    }
                }
            }
        }

        // Mix metronome click
        outSample += click_.nextSample();

        // Input monitoring (pass through all input channels)
        if (inputMonitoring_) {
            outSample += inputMix;
        }

        if (output) {
            output[i] = outSample;
        }

        // Advance metronome and MIDI sync by 1 sample
        metronome_.advance(1);
        midiSync_.advance(1);
    }

    // Check for completed background captures and swap results into loops
    checkBackgroundCaptures();

    // Update live channel bitmask and threshold breach timestamps
    {
        int64_t currentSample = metronome_.position().totalSamples;
        uint64_t mask = 0;
        for (int ch = 0; ch < engineChannels && ch < 64; ++ch) {
            if (inputChannels_[static_cast<size_t>(ch)].isLive(liveThreshold_)) {
                mask |= (uint64_t(1) << ch);
                lastThresholdBreachSample_[static_cast<size_t>(ch)] = currentSample;
            }
        }
        liveChannelMask_.store(mask, std::memory_order_relaxed);
    }

    // Update display snapshot (non-blocking)
    {
        std::unique_lock<std::mutex> lock(displayMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            for (int ch = 0; ch < engineChannels; ++ch) {
                channelPeaksSnapshot_[static_cast<size_t>(ch)] =
                    inputChannels_[static_cast<size_t>(ch)].peakLevel();
            }
        }
    }
}

void LoopEngine::flushDueOps(Loop& lp, int64_t currentSample) {
    auto& ps = lp.pendingState();

    // Clear — if due, execute and cancel everything else
    if (ps.clear && ps.clear->executeSample <= currentSample) {
        cancelBackgroundCapture(lp.id());
        lp.clear();
        lastMessage_ = "Loop " + std::to_string(lp.id()) + " cleared";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        ps.clearAll();
        if (callbacks_.onStateChanged) callbacks_.onStateChanged();
        return;
    }

    // Capture — signal phase 2 of the background capture thread
    if (ps.capture && ps.capture->executeSample <= currentSample) {
        ps.capture.reset();
        signalCapturePhase2(lp.id());
    }

    // Record start/stop
    if (ps.record && ps.record->executeSample <= currentSample) {
        auto recordOp = ps.recordOp;
        ps.record.reset();
        if (recordOp == PendingState::RecordOp::Start) {
            fulfillRecord(lp);
        } else {
            fulfillStopRecord(lp);
        }
    }

    // Mute
    if (ps.mute && ps.mute->executeSample <= currentSample) {
        auto muteOp = ps.muteOp;
        ps.mute.reset();
        switch (muteOp) {
            case PendingState::MuteOp::Mute:
                lp.mute();
                lastMessage_ = "Loop " + std::to_string(lp.id()) + " muted";
                break;
            case PendingState::MuteOp::Unmute:
                lp.play();
                lastMessage_ = "Loop " + std::to_string(lp.id()) + " unmuted";
                break;
            case PendingState::MuteOp::Toggle:
                lp.toggleMute();
                lastMessage_ = "Loop " + std::to_string(lp.id()) +
                              (lp.isMuted() ? " muted" : " unmuted");
                break;
        }
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        if (callbacks_.onStateChanged) callbacks_.onStateChanged();
    }

    // Overdub
    if (ps.overdub && ps.overdub->executeSample <= currentSample) {
        auto overdubOp = ps.overdubOp;
        ps.overdub.reset();
        if (overdubOp == PendingState::OverdubOp::Start) {
            lp.startOverdub();
            // Initialize per-channel overdub buffers
            int numCh = static_cast<int>(inputChannels_.size());
            overdubChannelBuffers_.resize(static_cast<size_t>(numCh));
            for (auto& buf : overdubChannelBuffers_) {
                buf.assign(static_cast<size_t>(lp.lengthSamples()), 0.0f);
            }
            overdubActiveChannelMask_ = 0;
            overdubLoopIndex_ = lp.id();
            lastMessage_ = "Loop " + std::to_string(lp.id()) + " overdub started";
        } else {
            // Mix down active channels into the overdub layer
            if (lp.id() == overdubLoopIndex_ && !overdubChannelBuffers_.empty()) {
                auto& layerAudio = lp.recordLayerAudio();
                for (size_t ch = 0; ch < overdubChannelBuffers_.size(); ++ch) {
                    if (liveThreshold_ <= 0.0f ||
                        (overdubActiveChannelMask_ & (uint64_t(1) << ch))) {
                        for (size_t j = 0; j < layerAudio.size() &&
                             j < overdubChannelBuffers_[ch].size(); ++j) {
                            layerAudio[j] += overdubChannelBuffers_[ch][j];
                        }
                    }
                }
            }
            // Reset per-layer overdub state
            overdubChannelBuffers_.clear();
            overdubActiveChannelMask_ = 0;
            overdubLoopIndex_ = -1;
            lp.stopOverdub();
            lastMessage_ = "Loop " + std::to_string(lp.id()) + " overdub stopped";
        }
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        if (callbacks_.onStateChanged) callbacks_.onStateChanged();
    }

    // Reverse
    if (ps.reverse && ps.reverse->executeSample <= currentSample) {
        ps.reverse.reset();
        lp.toggleReverse();
        lastMessage_ = "Loop " + std::to_string(lp.id()) +
                      (lp.isReversed() ? " reversed" : " forward");
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        if (callbacks_.onStateChanged) callbacks_.onStateChanged();
    }

    // Speed
    if (ps.speed && ps.speed->executeSample <= currentSample) {
        double spd = ps.speed->speed;
        ps.speed.reset();
        lp.setSpeed(spd);
        lastMessage_ = "Loop " + std::to_string(lp.id()) + " speed: " +
                      std::to_string(spd) + "x";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        if (callbacks_.onStateChanged) callbacks_.onStateChanged();
    }

    // Scramble on/off
    if (ps.scramble && ps.scramble->executeSample <= currentSample) {
        PendingScramble sc = *ps.scramble;
        ps.scramble.reset();
        if (sc.enable) {
            lp.scrambleOn(sc.params, metronome_.samplesPerBeat());
            lastMessage_ = "Loop " + std::to_string(lp.id()) + " scramble ON";
        } else {
            lp.scrambleOff();
            lastMessage_ = "Loop " + std::to_string(lp.id()) + " scramble OFF";
        }
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        if (callbacks_.onStateChanged) callbacks_.onStateChanged();
    }

    // Undo/Redo
    if (ps.undo && ps.undo->executeSample <= currentSample) {
        PendingUndo u = *ps.undo;
        ps.undo.reset();
        for (int n = 0; n < u.count; ++n) {
            if (u.direction == UndoDirection::Undo)
                lp.undoLayer();
            else
                lp.redoLayer();
        }
        std::string verb = (u.direction == UndoDirection::Undo) ? "undone" : "redone";
        lastMessage_ = "Loop " + std::to_string(lp.id()) + " " +
                      std::to_string(u.count) + " layer(s) " + verb;
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        if (callbacks_.onStateChanged) callbacks_.onStateChanged();
    }
}

LoopEngine::ActiveChannels LoopEngine::detectActiveChannels(int64_t captureStartSample) const {
    ActiveChannels result;
    int engineChannels = static_cast<int>(inputChannels_.size());
    for (int chIdx = 0; chIdx < engineChannels && chIdx < 64; ++chIdx) {
        bool hadActivity = (liveThreshold_ <= 0.0f) ||
            (lastThresholdBreachSample_[static_cast<size_t>(chIdx)] >= captureStartSample);
        if (hadActivity) {
            result.mask |= (uint64_t(1) << chIdx);
            ++result.count;
        }
    }
    return result;
}

void LoopEngine::beginCapture(int loopIndex, int64_t captureLen, int64_t gap) {
    // Determine which channels to include
    int64_t currentSample = metronome_.position().totalSamples;
    int64_t captureStartSample = currentSample - (captureLen - gap) - latencyCompensation_;
    auto active = detectActiveChannels(captureStartSample);

    if (active.count == 0) {
        lastMessage_ = "No live input channels to capture";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    cancelBackgroundCapture(loopIndex);

    // Phase 1 reads everything already in the ring buffer.
    // The total capture covers captureLen samples ending at the execution
    // boundary, but the execution boundary is `gap` samples in the future.
    // So right now, (captureLen - gap) samples are available.
    int64_t phase1Samples = captureLen - gap;
    int64_t phase1SamplesAgo = phase1Samples + latencyCompensation_;

    // Snapshot each active channel's ring buffer state
    int engineChannels = static_cast<int>(inputChannels_.size());
    std::vector<ChannelSnap> snaps;
    for (int ch = 0; ch < engineChannels && ch < 64; ++ch) {
        if (!(active.mask & (uint64_t(1) << ch))) continue;
        snaps.push_back({ch, inputChannels_[static_cast<size_t>(ch)].ringBuffer().snapshot()});
    }

    auto bg = std::make_unique<BackgroundCapture>();
    bg->loopIndex = loopIndex;
    bg->captureLen = captureLen;
    bg->phase1Samples = phase1Samples;
    bg->bars = static_cast<double>(captureLen) / metronome_.samplesPerBar();
    bg->recordedBpm = metronome_.bpm();
    bg->crossfadeSamples = crossfadeSamples_;
    bg->liveCount = active.count;
    bg->activeChannelMask = active.mask;

    auto* channelsPtr = inputChannels_.data();
    auto* bgPtr = bg.get();

    bg->thread = std::thread([channelsPtr, snaps = std::move(snaps),
                              phase1Samples, phase1SamplesAgo, bgPtr] {
        int64_t totalLen = bgPtr->captureLen;
        int numPhase1 = static_cast<int>(phase1Samples);
        std::vector<float> audio(static_cast<size_t>(totalLen), 0.0f);
        std::vector<float> chBuf(static_cast<size_t>(totalLen));

        // Phase 1: copy the bulk of the audio that's already in the ring buffer
        if (numPhase1 > 0) {
            for (const auto& cs : snaps) {
                channelsPtr[static_cast<size_t>(cs.channelIndex)].ringBuffer()
                    .readFromSnapshot(chBuf.data(), numPhase1, phase1SamplesAgo, cs.snap);
                for (int j = 0; j < numPhase1; ++j) {
                    audio[static_cast<size_t>(j)] += chBuf[static_cast<size_t>(j)];
                }
            }
        }

        // Phase 2: wait for the audio thread to signal remaining samples
        int64_t remaining = totalLen - phase1Samples;
        if (remaining > 0) {
            while (!bgPtr->phase2Ready.load(std::memory_order_acquire)) {
                if (bgPtr->cancelled.load(std::memory_order_acquire)) {
                    bgPtr->done.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }

            if (bgPtr->cancelled.load(std::memory_order_acquire)) {
                bgPtr->done.store(true, std::memory_order_release);
                return;
            }

            int numPhase2 = static_cast<int>(remaining);
            for (const auto& cs : bgPtr->phase2Snaps) {
                channelsPtr[static_cast<size_t>(cs.channelIndex)].ringBuffer()
                    .readFromSnapshot(chBuf.data(), numPhase2,
                                     bgPtr->phase2SamplesAgo, cs.snap);
                for (int j = 0; j < numPhase2; ++j) {
                    audio[static_cast<size_t>(phase1Samples + j)] +=
                        chBuf[static_cast<size_t>(j)];
                }
            }
        }

        bgPtr->completedAudio = std::move(audio);
        bgPtr->done.store(true, std::memory_order_release);
    });

    double bars = bg->bars;
    int liveCount = active.count;
    bgCaptures_[static_cast<size_t>(loopIndex)] = std::move(bg);

    std::ostringstream msg;
    msg << "Loop " << loopIndex << " capturing (" << static_cast<int>(std::round(bars))
        << " bars, " << liveCount << " ch)";
    lastMessage_ = msg.str();
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
}

void LoopEngine::signalCapturePhase2(int loopIndex) {
    auto& bg = bgCaptures_[static_cast<size_t>(loopIndex)];
    if (!bg || bg->done.load(std::memory_order_acquire)) return;

    int64_t remaining = bg->captureLen - bg->phase1Samples;
    int64_t samplesAgo = remaining + latencyCompensation_;

    // Snapshot the same channels that phase 1 used
    int engineChannels = static_cast<int>(inputChannels_.size());
    bg->phase2Snaps.clear();
    for (int ch = 0; ch < engineChannels && ch < 64; ++ch) {
        if (!(bg->activeChannelMask & (uint64_t(1) << ch))) continue;
        bg->phase2Snaps.push_back(
            {ch, inputChannels_[static_cast<size_t>(ch)].ringBuffer().snapshot()});
    }
    bg->phase2SamplesAgo = samplesAgo;

    bg->phase2Ready.store(true, std::memory_order_release);
}

void LoopEngine::checkBackgroundCaptures() {
    for (auto& bg : bgCaptures_) {
        if (!bg || !bg->done.load(std::memory_order_acquire)) continue;

        auto& lp = loops_[static_cast<size_t>(bg->loopIndex)];

        // Only install if the loop slot is still available.
        // If it was re-captured or filled by another operation, discard.
        if (!bg->cancelled.load(std::memory_order_relaxed) && lp.isEmpty()) {
            lp.loadFromCapture(std::move(bg->completedAudio));
            lp.setCrossfadeSamples(bg->crossfadeSamples);
            lp.setLengthInBars(bg->bars);
            lp.setRecordedBpm(bg->recordedBpm);
            lp.setCurrentBpm(bg->recordedBpm);

            std::ostringstream msg;
            msg << "Loop " << bg->loopIndex << " captured ("
                << static_cast<int>(std::round(bg->bars)) << " bars, "
                << bg->liveCount << " ch)";
            lastMessage_ = msg.str();
            if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
            if (callbacks_.onStateChanged) callbacks_.onStateChanged();
        }

        bg->thread.join();
        bg.reset();
    }

    // Reap cancelled captures that have finished
    zombieCaptures_.erase(
        std::remove_if(zombieCaptures_.begin(), zombieCaptures_.end(),
            [](std::unique_ptr<BackgroundCapture>& bg) {
                if (!bg) return true;
                if (!bg->done.load(std::memory_order_acquire)) return false;
                bg->thread.join();
                return true;
            }),
        zombieCaptures_.end());
}

void LoopEngine::cancelBackgroundCapture(int loopIndex) {
    auto& bg = bgCaptures_[static_cast<size_t>(loopIndex)];
    if (!bg) return;

    // Signal the thread to stop spinning and exit
    bg->cancelled.store(true, std::memory_order_release);
    bg->phase2Ready.store(true, std::memory_order_release);

    // Move to zombie list — the thread will be joined in checkBackgroundCaptures
    // to avoid blocking the audio thread with thread::join().
    zombieCaptures_.push_back(std::move(bg));
}

void LoopEngine::fulfillRecord(Loop& lp) {
    int idx = lp.id();

    if (activeRecording_) {
        lastMessage_ = "Already recording on Loop " +
                       std::to_string(activeRecording_->loopIndex);
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    // Clear the target loop if it has content
    lp.clear();

    // Start accumulating per-channel input
    ActiveRecording rec;
    rec.loopIndex = idx;
    rec.startSample = metronome_.position().totalSamples;
    int numCh = static_cast<int>(inputChannels_.size());
    rec.channelBuffers.resize(static_cast<size_t>(numCh));
    activeRecording_ = std::move(rec);

    isRecordingAtomic_.store(true, std::memory_order_relaxed);
    recordingLoopIdxAtomic_.store(idx, std::memory_order_relaxed);

    lastMessage_ = "Loop " + std::to_string(idx) + " recording...";
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::fulfillStopRecord(Loop& lp) {
    if (!activeRecording_) {
        lastMessage_ = "No active recording";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    int idx = activeRecording_->loopIndex;

    // Ignore if the stop targets a different loop than what's recording
    if (lp.id() != idx) {
        lastMessage_ = "Stop ignored: recording is on Loop " + std::to_string(idx);
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    // Mix down per-channel buffers, including only channels that breached
    // the threshold at any point during the recording (sticky inclusion).
    auto& rec = *activeRecording_;
    size_t len = rec.channelBuffers.empty() ? 0 : rec.channelBuffers[0].size();

    // Apply latency compensation: trim the first latencyCompensation_ samples
    // from each channel (audio from before the intended recording start).
    size_t trimFront = 0;
    if (latencyCompensation_ > 0 && static_cast<int64_t>(len) > latencyCompensation_) {
        trimFront = static_cast<size_t>(latencyCompensation_);
    }
    size_t mixLen = len - trimFront;

    if (mixLen == 0) {
        lastMessage_ = "No audio recorded";
        activeRecording_.reset();
        isRecordingAtomic_.store(false, std::memory_order_relaxed);
        recordingLoopIdxAtomic_.store(-1, std::memory_order_relaxed);
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    std::vector<float> mixed(mixLen, 0.0f);
    int liveCount = 0;
    for (size_t ch = 0; ch < rec.channelBuffers.size(); ++ch) {
        if (liveThreshold_ <= 0.0f ||
            (rec.activeChannelMask & (uint64_t(1) << ch))) {
            for (size_t j = 0; j < mixLen; ++j) {
                mixed[j] += rec.channelBuffers[ch][trimFront + j];
            }
            ++liveCount;
        }
    }

    if (liveCount == 0) {
        lastMessage_ = "No active channels recorded";
        activeRecording_.reset();
        isRecordingAtomic_.store(false, std::memory_order_relaxed);
        recordingLoopIdxAtomic_.store(-1, std::memory_order_relaxed);
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    // Load the mixed audio into the loop
    lp.loadFromCapture(std::move(mixed));
    lp.setCrossfadeSamples(crossfadeSamples_);

    double bars = static_cast<double>(lp.lengthSamples()) / metronome_.samplesPerBar();
    lp.setLengthInBars(bars);

    // Record the BPM at recording time for time stretching
    lp.setRecordedBpm(metronome_.bpm());
    lp.setCurrentBpm(metronome_.bpm());

    activeRecording_.reset();
    isRecordingAtomic_.store(false, std::memory_order_relaxed);
    recordingLoopIdxAtomic_.store(-1, std::memory_order_relaxed);

    std::ostringstream msg;
    msg << "Loop " << idx << " recorded ("
        << std::fixed << std::setprecision(1) << bars << " bars, "
        << liveCount << " ch)";
    lastMessage_ = msg.str();
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::scheduleOp(OpType type, int loopIndex, Quantize quantize) {
    EngineCommand cmd;
    cmd.commandType = CommandType::ScheduleOp;
    cmd.opType = type;
    cmd.loopIndex = loopIndex;
    cmd.quantize = quantize;
    enqueueCommand(cmd);

    // Generate message on TUI thread
    std::string msg = opTypeDescription(type);
    if (quantize != Quantize::Free) {
        msg += " (pending: ";
        msg += (quantize == Quantize::Beat ? "next beat" : "next bar");
        msg += ")";
    }
    if (callbacks_.onMessage) callbacks_.onMessage(msg);
}

void LoopEngine::scheduleCaptureLoop(int loopIndex, Quantize quantize,
                                     double lookbackBarsOverride) {
    int bars = lookbackBarsOverride > 0
        ? static_cast<int>(std::round(lookbackBarsOverride))
        : lookbackBars_;

    EngineCommand cmd;
    cmd.commandType = CommandType::CaptureLoop;
    cmd.loopIndex = loopIndex;
    cmd.quantize = quantize;
    cmd.lookbackBars = bars;
    enqueueCommand(cmd);

    std::ostringstream msg;
    msg << "Capture " << bars << " bar(s) -> "
        << (loopIndex < 0 ? "selected loops" : "Loop " + std::to_string(loopIndex));
    if (quantize != Quantize::Free) {
        msg << " (pending: " << (quantize == Quantize::Beat ? "next beat" : "next bar") << ")";
    }
    if (callbacks_.onMessage) callbacks_.onMessage(msg.str());
}

void LoopEngine::scheduleSetSpeed(int loopIndex, double speed, Quantize quantize) {
    EngineCommand cmd;
    cmd.commandType = CommandType::SetSpeed;
    cmd.loopIndex = loopIndex;
    cmd.quantize = quantize;
    cmd.value = speed;
    enqueueCommand(cmd);
}

void LoopEngine::scheduleRecord(int loopIndex, Quantize quantize) {
    EngineCommand cmd;
    cmd.commandType = CommandType::Record;
    cmd.loopIndex = loopIndex;
    cmd.quantize = quantize;
    enqueueCommand(cmd);

    std::ostringstream msg;
    msg << "Record -> "
        << (loopIndex < 0 ? "selected loops" : "Loop " + std::to_string(loopIndex));
    if (quantize != Quantize::Free) {
        msg << " (pending: " << (quantize == Quantize::Beat ? "next beat" : "next bar") << ")";
    }
    if (callbacks_.onMessage) callbacks_.onMessage(msg.str());
}

void LoopEngine::scheduleStopRecord(int loopIndex, Quantize quantize) {
    EngineCommand cmd;
    cmd.commandType = CommandType::StopRecord;
    cmd.loopIndex = loopIndex;
    cmd.quantize = quantize;
    enqueueCommand(cmd);

    std::string msg = "Stop Record";
    if (quantize != Quantize::Free) {
        msg += " (pending: ";
        msg += (quantize == Quantize::Beat ? "next beat" : "next bar");
        msg += ")";
    }
    if (callbacks_.onMessage) callbacks_.onMessage(msg);
}

void LoopEngine::scheduleScrambleOn(int loopIndex, Quantize quantize,
                                    const ScrambleParams& params) {
    EngineCommand cmd;
    cmd.commandType = CommandType::ScrambleOn;
    cmd.loopIndex = loopIndex;
    cmd.quantize = quantize;
    cmd.scrambleParams = params;
    enqueueCommand(cmd);

    std::string msg = "Scramble On";
    if (quantize != Quantize::Free) {
        msg += " (pending: ";
        msg += (quantize == Quantize::Beat ? "next beat" : "next bar");
        msg += ")";
    }
    if (callbacks_.onMessage) callbacks_.onMessage(msg);
}

void LoopEngine::scheduleScrambleOff(int loopIndex, Quantize quantize) {
    EngineCommand cmd;
    cmd.commandType = CommandType::ScrambleOff;
    cmd.loopIndex = loopIndex;
    cmd.quantize = quantize;
    enqueueCommand(cmd);

    std::string msg = "Scramble Off";
    if (quantize != Quantize::Free) {
        msg += " (pending: ";
        msg += (quantize == Quantize::Beat ? "next beat" : "next bar");
        msg += ")";
    }
    if (callbacks_.onMessage) callbacks_.onMessage(msg);
}

void LoopEngine::setScrambleWindowDuration(int loopIndex, double beats) {
    EngineCommand cmd;
    cmd.commandType = CommandType::SetScrambleWindow;
    cmd.loopIndex = loopIndex;
    cmd.quantize = Quantize::Free;
    cmd.value = beats;
    enqueueCommand(cmd);
}

void LoopEngine::executeOpNow(OpType type, int loopIndex) {
    if (type == OpType::CaptureLoop) {
        scheduleCaptureLoop(loopIndex, Quantize::Free);
    } else {
        scheduleOp(type, loopIndex, Quantize::Free);
    }
}

void LoopEngine::cancelPending() {
    EngineCommand cmd;
    cmd.commandType = CommandType::CancelPending;
    enqueueCommand(cmd);

    if (callbacks_.onMessage) callbacks_.onMessage("All pending ops cancelled");
}

void LoopEngine::cancelPending(int loopIndex) {
    if (loopIndex >= 0 && loopIndex < maxLoops()) {
        loops_[static_cast<size_t>(loopIndex)].clearPendingOps();
    }
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

int LoopEngine::activeLoopCount() const {
    int count = 0;
    for (const auto& lp : loops_) {
        if (!lp.isEmpty()) ++count;
    }
    return count;
}

void LoopEngine::selectLoop(int idx) {
    if (idx < 0 || idx >= maxLoops()) return;
    selectedLoopMask_.fetch_or(uint64_t(1) << idx, std::memory_order_relaxed);
}

void LoopEngine::deselectLoop(int idx) {
    if (idx < 0 || idx >= maxLoops()) return;
    selectedLoopMask_.fetch_and(~(uint64_t(1) << idx), std::memory_order_relaxed);
}

void LoopEngine::toggleSelectLoop(int idx) {
    if (idx < 0 || idx >= maxLoops()) return;
    selectedLoopMask_.fetch_xor(uint64_t(1) << idx, std::memory_order_relaxed);
}

int LoopEngine::nextEmptySlot() const {
    for (int i = 0; i < maxLoops(); ++i) {
        if (loops_[static_cast<size_t>(i)].isEmpty()) return i;
    }
    return -1;
}

int LoopEngine::setLookbackBars(int bars) {
    lookbackBars_ = std::max(1, std::min(bars, maxLookbackBars_));
    return lookbackBars_;
}

int LoopEngine::recordingLoopIndex() const {
    if (activeRecording_) return activeRecording_->loopIndex;
    return -1;
}

void LoopEngine::setCallbacks(EngineCallbacks cb) {
    callbacks_ = std::move(cb);

    // Re-wire metronome callbacks to include the new ones
    metronome_.onBeat([this](const MetronomePosition& pos) {
        click_.trigger(pos.beat == 0);
        if (callbacks_.onBeat) callbacks_.onBeat(pos);
    });
    metronome_.onBar([this](const MetronomePosition& pos) {
        if (callbacks_.onBar) callbacks_.onBar(pos);
    });
}

std::string LoopEngine::statusMessage() const {
    return lastMessage_;
}

void LoopEngine::scheduleMidiSync(bool on, Quantize quantize) {
    EngineCommand cmd;
    cmd.commandType = CommandType::SetMidiSync;
    cmd.quantize = quantize;
    cmd.value = on ? 1.0 : 0.0;
    commandQueue_.push(cmd);
}

void LoopEngine::enqueueCommand(const EngineCommand& cmd) {
    commandQueue_.push(cmd);
}

int64_t LoopEngine::computeExecuteSample(Quantize quantize) const {
    if (quantize == Quantize::Free) {
        return metronome_.position().totalSamples;
    }
    return metronome_.position().totalSamples +
           metronome_.samplesUntilBoundary(quantize);
}

void LoopEngine::drainCommands() {
    EngineCommand cmd;
    while (commandQueue_.pop(cmd)) {
        /// Invoke fn(loopIndex) for each targeted loop.
        /// loopIndex >= 0 targets that loop; -1 expands to all selected loops.
        auto forEachTarget = [&](int idx, auto fn) {
            if (idx >= 0 && idx < maxLoops()) {
                fn(idx);
            } else if (idx == -1) {
                uint64_t mask = selectedLoopMask_.load(std::memory_order_relaxed);
                for (int i = 0; i < maxLoops() && mask; ++i, mask >>= 1) {
                    if (mask & 1) fn(i);
                }
            }
        };

        switch (cmd.commandType) {
            case CommandType::ScheduleOp: {
                int64_t execSample = computeExecuteSample(cmd.quantize);
                forEachTarget(cmd.loopIndex, [&](int idx) {
                    auto& ps = loops_[static_cast<size_t>(idx)].pendingState();
                    switch (cmd.opType) {
                        case OpType::Mute:
                            ps.mute = PendingTimedOp{execSample, cmd.quantize};
                            ps.muteOp = PendingState::MuteOp::Mute;
                            break;
                        case OpType::Unmute:
                            ps.mute = PendingTimedOp{execSample, cmd.quantize};
                            ps.muteOp = PendingState::MuteOp::Unmute;
                            break;
                        case OpType::ToggleMute:
                            ps.mute = PendingTimedOp{execSample, cmd.quantize};
                            ps.muteOp = PendingState::MuteOp::Toggle;
                            break;
                        case OpType::Reverse:
                            ps.reverse = PendingTimedOp{execSample, cmd.quantize};
                            break;
                        case OpType::StartOverdub:
                            ps.overdub = PendingTimedOp{execSample, cmd.quantize};
                            ps.overdubOp = PendingState::OverdubOp::Start;
                            break;
                        case OpType::StopOverdub:
                            ps.overdub = PendingTimedOp{execSample, cmd.quantize};
                            ps.overdubOp = PendingState::OverdubOp::Stop;
                            break;
                        case OpType::UndoLayer:
                            if (ps.undo && ps.undo->direction == UndoDirection::Undo) {
                                ps.undo->count++;
                            } else {
                                ps.undo = PendingUndo{execSample, cmd.quantize, 1, UndoDirection::Undo};
                            }
                            break;
                        case OpType::RedoLayer:
                            if (ps.undo && ps.undo->direction == UndoDirection::Redo) {
                                ps.undo->count++;
                            } else {
                                ps.undo = PendingUndo{execSample, cmd.quantize, 1, UndoDirection::Redo};
                            }
                            break;
                        case OpType::ClearLoop:
                            ps.clear = PendingTimedOp{execSample, cmd.quantize};
                            break;
                        // These use dedicated CommandTypes, but handle gracefully
                        case OpType::CaptureLoop:
                        case OpType::Record:
                        case OpType::StopRecord:
                        case OpType::SetSpeed:
                        case OpType::Seek:
                        case OpType::ScrambleOn:
                        case OpType::ScrambleOff:
                        case OpType::SetScrambleWindow:
                            break;
                    }
                });
                break;
            }
            case CommandType::CaptureLoop: {
                int64_t execSample = computeExecuteSample(cmd.quantize);
                int64_t currentSample = metronome_.position().totalSamples;
                int64_t gap = execSample - currentSample;
                int64_t lookbackSamples = static_cast<int64_t>(
                    std::round(static_cast<double>(cmd.lookbackBars) *
                               metronome_.samplesPerBar()));
                // Clamp to minimum available across all input channels
                for (auto& ch : inputChannels_) {
                    lookbackSamples = std::min(lookbackSamples, ch.ringBuffer().available());
                }
                if (lookbackSamples <= 0) {
                    lastMessage_ = "No audio to capture";
                    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
                    break;
                }
                forEachTarget(cmd.loopIndex, [&](int idx) {
                    auto& ps = loops_[static_cast<size_t>(idx)].pendingState();
                    PendingCapture cap;
                    cap.executeSample = execSample;
                    cap.quantize = cmd.quantize;
                    cap.lookbackSamples = lookbackSamples;
                    ps.capture = cap;
                    // Spawn phase 1 immediately to start copying bulk audio
                    beginCapture(idx, lookbackSamples, gap);
                });
                break;
            }
            case CommandType::Record: {
                int64_t execSample = computeExecuteSample(cmd.quantize);
                forEachTarget(cmd.loopIndex, [&](int idx) {
                    auto& ps = loops_[static_cast<size_t>(idx)].pendingState();
                    ps.record = PendingTimedOp{execSample, cmd.quantize};
                    ps.recordOp = PendingState::RecordOp::Start;
                });
                break;
            }
            case CommandType::StopRecord: {
                int64_t execSample = computeExecuteSample(cmd.quantize);
                forEachTarget(cmd.loopIndex, [&](int idx) {
                    auto& ps = loops_[static_cast<size_t>(idx)].pendingState();
                    ps.record = PendingTimedOp{execSample, cmd.quantize};
                    ps.recordOp = PendingState::RecordOp::Stop;
                });
                break;
            }
            case CommandType::SetSpeed: {
                int64_t execSample = computeExecuteSample(cmd.quantize);
                forEachTarget(cmd.loopIndex, [&](int idx) {
                    auto& ps = loops_[static_cast<size_t>(idx)].pendingState();
                    ps.speed = PendingSpeed{execSample, cmd.quantize, cmd.value};
                });
                break;
            }
            case CommandType::SetBpm: {
                metronome_.setBpm(cmd.value);
                midiSync_.setBpm(cmd.value);
                if (bpmChangedCallback_) bpmChangedCallback_(cmd.value);
                // Propagate BPM change to all loops for time stretching
                double newBpm = metronome_.bpm();
                for (auto& lp : loops_) {
                    if (!lp.isEmpty()) {
                        lp.setCurrentBpm(newBpm);
                    }
                }
                break;
            }
            case CommandType::CancelPending: {
                for (auto& lp : loops_) {
                    lp.clearPendingOps();
                }
                break;
            }
            case CommandType::SetMidiSync: {
                pendingMidiSync_ = {computeExecuteSample(cmd.quantize), cmd.value != 0.0};
                break;
            }
            case CommandType::ScrambleOn: {
                int idx = cmd.loopIndex;
                if (idx < 0 || idx >= maxLoops()) break;
                Loop& lp = loops_[static_cast<size_t>(idx)];
                auto& ps = lp.pendingState();
                PendingScramble sc;
                sc.executeSample = computeExecuteSample(cmd.quantize);
                sc.quantize = cmd.quantize;
                sc.enable = true;
                sc.params = cmd.scrambleParams;
                ps.scramble = sc;
                break;
            }
            case CommandType::ScrambleOff: {
                int idx = cmd.loopIndex;
                if (idx < 0 || idx >= maxLoops()) break;
                Loop& lp = loops_[static_cast<size_t>(idx)];
                auto& ps = lp.pendingState();
                PendingScramble sc;
                sc.executeSample = computeExecuteSample(cmd.quantize);
                sc.quantize = cmd.quantize;
                sc.enable = false;
                ps.scramble = sc;
                break;
            }
            case CommandType::SetScrambleWindow: {
                int idx = cmd.loopIndex;
                if (idx < 0 || idx >= maxLoops()) break;
                Loop& lp = loops_[static_cast<size_t>(idx)];
                lp.setScrambleWindowDuration(cmd.value);
                break;
            }
        }
    }
}

std::vector<float> LoopEngine::channelPeaksSnapshot() const {
    std::lock_guard<std::mutex> lock(displayMutex_);
    return channelPeaksSnapshot_;
}

} // namespace retrospect
