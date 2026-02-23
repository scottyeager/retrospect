#include "core/LoopEngine.h"
#include "core/EngineCommand.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

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
        case OpType::SetSpeed:     return "Set Speed";
        case OpType::ClearLoop:    return "Clear";
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

    // Wire metronome callbacks
    metronome_.onBeat([this](const MetronomePosition& pos) {
        click_.trigger(pos.beat == 0);
        if (callbacks_.onBeat) callbacks_.onBeat(pos);
    });
    metronome_.onBar([this](const MetronomePosition& pos) {
        if (callbacks_.onBar) callbacks_.onBar(pos);
    });
}

void LoopEngine::processBlock(const float* const* input, int inputChannelCount,
                              float* output, int numSamples) {
    // Drain commands from TUI thread at the start of each block
    drainCommands();

    int engineChannels = static_cast<int>(inputChannels_.size());

    for (int i = 0; i < numSamples; ++i) {
        // Write each input channel to its InputChannel and compute
        // the mono mix of live channels.
        float liveMix = 0.0f;
        for (int ch = 0; ch < engineChannels; ++ch) {
            float sample = (ch < inputChannelCount && input && input[ch])
                ? input[ch][i] : 0.0f;
            inputChannels_[static_cast<size_t>(ch)].writeSample(sample);
            if (inputChannels_[static_cast<size_t>(ch)].isLive(liveThreshold_)) {
                liveMix += sample;
            }
        }

        // Accumulate into active classic recording if one is in progress
        if (activeRecording_) {
            activeRecording_->buffer.push_back(liveMix);
        }

        // Check each loop's pending state
        int64_t currentSample = metronome_.position().totalSamples;
        for (auto& lp : loops_) {
            if (lp.hasPendingOps()) {
                flushDueOps(lp, currentSample);
            }
        }

        // Mix output from all playing loops
        float outSample = 0.0f;
        for (auto& lp : loops_) {
            if (!lp.isEmpty()) {
                outSample += lp.processSample();

                // Feed live input mix to overdub-recording loops
                if (lp.isRecording()) {
                    lp.recordSample(liveMix);
                }
            }
        }

        // Mix metronome click
        outSample += click_.nextSample();

        // Input monitoring (pass through the live mix)
        if (inputMonitoring_) {
            outSample += liveMix;
        }

        if (output) {
            output[i] = outSample;
        }

        // Advance metronome and MIDI sync by 1 sample
        metronome_.advance(1);
        midiSync_.advance(1);
    }

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
        lp.clear();
        lastMessage_ = "Loop " + std::to_string(lp.id()) + " cleared";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        ps.clearAll();
        if (callbacks_.onStateChanged) callbacks_.onStateChanged();
        return;
    }

    // Capture
    if (ps.capture && ps.capture->executeSample <= currentSample) {
        PendingCapture cap = *ps.capture;
        ps.capture.reset();
        fulfillCapture(lp, cap);
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
            lastMessage_ = "Loop " + std::to_string(lp.id()) + " overdub started";
        } else {
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

void LoopEngine::fulfillCapture(Loop& lp, const PendingCapture& cap) {
    int idx = lp.id();

    int64_t lookback = cap.lookbackSamples;
    if (lookback <= 0) {
        lookback = static_cast<int64_t>(
            std::round(static_cast<double>(lookbackBars_) * metronome_.samplesPerBar()));
    }

    // Clamp to the minimum available across all input channels
    for (auto& ch : inputChannels_) {
        lookback = std::min(lookback, ch.ringBuffer().available());
    }
    if (lookback <= 0) {
        lastMessage_ = "No audio to capture";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    int captureLen = static_cast<int>(lookback);

    // Capture from each input channel and mix down to mono.
    // A channel is included if it exceeded the live threshold at any point
    // during the capture window (checked via lastThresholdBreachSample_,
    // an O(1) lookup updated each processBlock). This avoids scanning the
    // entire captured segment and ensures the full channel audio is included
    // whenever the channel had activity during the lookback period.
    // Apply latency compensation: read from further back in the ring buffer
    // to align captured audio with the metronome's internal timeline.
    int64_t samplesAgo = static_cast<int64_t>(captureLen) + latencyCompensation_;
    int64_t currentSample = metronome_.position().totalSamples;
    int64_t captureStartSample = currentSample - samplesAgo;
    std::vector<float> audio(static_cast<size_t>(captureLen), 0.0f);
    int liveCount = 0;
    int engineChannels = static_cast<int>(inputChannels_.size());
    for (int chIdx = 0; chIdx < engineChannels; ++chIdx) {
        bool hadActivity = (liveThreshold_ <= 0.0f) ||
            (lastThresholdBreachSample_[static_cast<size_t>(chIdx)] >= captureStartSample);

        if (hadActivity) {
            std::vector<float> chAudio(static_cast<size_t>(captureLen), 0.0f);
            inputChannels_[static_cast<size_t>(chIdx)].ringBuffer()
                .readFromPast(chAudio.data(), captureLen, samplesAgo);
            for (size_t j = 0; j < audio.size(); ++j) {
                audio[j] += chAudio[j];
            }
            ++liveCount;
        }
    }

    if (liveCount == 0) {
        lastMessage_ = "No live input channels to capture";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    lp.loadFromCapture(std::move(audio));
    lp.setCrossfadeSamples(crossfadeSamples_);

    double bars = static_cast<double>(lookback) / metronome_.samplesPerBar();
    lp.setLengthInBars(bars);

    // Record the BPM at capture time for time stretching
    lp.setRecordedBpm(metronome_.bpm());
    lp.setCurrentBpm(metronome_.bpm());

    std::ostringstream msg;
    msg << "Loop " << idx << " captured (" << static_cast<int>(std::round(bars))
        << " bars, " << liveCount << " ch)";
    lastMessage_ = msg.str();
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
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

    // Start accumulating input
    ActiveRecording rec;
    rec.loopIndex = idx;
    rec.startSample = metronome_.position().totalSamples;
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

    // Apply latency compensation: the first latencyCompensation_ samples in the
    // buffer are audio from before the intended recording start (they were still
    // in the hardware pipeline when recording began). Trim them so the loop
    // content aligns with the metronome.
    auto& buf = activeRecording_->buffer;
    if (latencyCompensation_ > 0 && static_cast<int64_t>(buf.size()) > latencyCompensation_) {
        buf.erase(buf.begin(), buf.begin() + latencyCompensation_);
    }

    if (buf.empty()) {
        lastMessage_ = "No audio recorded";
        activeRecording_.reset();
        isRecordingAtomic_.store(false, std::memory_order_relaxed);
        recordingLoopIdxAtomic_.store(-1, std::memory_order_relaxed);
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    // Load the recorded audio into the loop
    lp.loadFromCapture(std::move(buf));
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
        << std::fixed << std::setprecision(1) << bars << " bars)";
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
    int targetLoop = loopIndex < 0 ? nextEmptySlot() : loopIndex;
    int bars = lookbackBarsOverride > 0
        ? static_cast<int>(std::round(lookbackBarsOverride))
        : lookbackBars_;

    EngineCommand cmd;
    cmd.commandType = CommandType::CaptureLoop;
    cmd.loopIndex = targetLoop;
    cmd.quantize = quantize;
    cmd.lookbackBars = bars;
    enqueueCommand(cmd);

    std::ostringstream msg;
    msg << "Capture " << bars << " bar(s) -> Loop " << targetLoop;
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
    int targetLoop = loopIndex < 0 ? nextEmptySlot() : loopIndex;

    EngineCommand cmd;
    cmd.commandType = CommandType::Record;
    cmd.loopIndex = targetLoop;
    cmd.quantize = quantize;
    enqueueCommand(cmd);

    std::ostringstream msg;
    msg << "Record -> Loop " << targetLoop;
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
        switch (cmd.commandType) {
            case CommandType::ScheduleOp: {
                int idx = cmd.loopIndex;
                if (idx < 0 || idx >= maxLoops()) break;
                Loop& lp = loops_[static_cast<size_t>(idx)];
                auto& ps = lp.pendingState();
                int64_t execSample = computeExecuteSample(cmd.quantize);

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
                        break;
                }
                break;
            }
            case CommandType::CaptureLoop: {
                int idx = cmd.loopIndex;
                if (idx < 0 || idx >= maxLoops()) break;
                Loop& lp = loops_[static_cast<size_t>(idx)];
                auto& ps = lp.pendingState();
                PendingCapture cap;
                cap.executeSample = computeExecuteSample(cmd.quantize);
                cap.quantize = cmd.quantize;
                cap.lookbackSamples = static_cast<int64_t>(
                    std::round(static_cast<double>(cmd.lookbackBars) *
                               metronome_.samplesPerBar()));
                ps.capture = cap;
                break;
            }
            case CommandType::Record: {
                int idx = cmd.loopIndex;
                if (idx < 0 || idx >= maxLoops()) break;
                Loop& lp = loops_[static_cast<size_t>(idx)];
                auto& ps = lp.pendingState();
                ps.record = PendingTimedOp{computeExecuteSample(cmd.quantize), cmd.quantize};
                ps.recordOp = PendingState::RecordOp::Start;
                break;
            }
            case CommandType::StopRecord: {
                int idx = cmd.loopIndex;
                if (idx < 0 || idx >= maxLoops()) break;
                Loop& lp = loops_[static_cast<size_t>(idx)];
                auto& ps = lp.pendingState();
                ps.record = PendingTimedOp{computeExecuteSample(cmd.quantize), cmd.quantize};
                ps.recordOp = PendingState::RecordOp::Stop;
                break;
            }
            case CommandType::SetSpeed: {
                int idx = cmd.loopIndex;
                if (idx < 0 || idx >= maxLoops()) break;
                Loop& lp = loops_[static_cast<size_t>(idx)];
                auto& ps = lp.pendingState();
                ps.speed = PendingSpeed{computeExecuteSample(cmd.quantize),
                                        cmd.quantize, cmd.value};
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
        }
    }
}

std::vector<float> LoopEngine::channelPeaksSnapshot() const {
    std::lock_guard<std::mutex> lock(displayMutex_);
    return channelPeaksSnapshot_;
}

} // namespace retrospect
