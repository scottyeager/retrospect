#include "core/LoopEngine.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace retrospect {

// PendingOp description
std::string PendingOp::description() const {
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
                       double sampleRate, double minBpm)
    : metronome_(120.0, 4, sampleRate)
    // Size ring buffer for maxLookbackBars at the slowest expected tempo.
    // At minBpm, one beat = (60/minBpm) seconds, one bar = beatsPerBar beats.
    , ringBuffer_(static_cast<int64_t>(
          std::ceil(maxLookbackBars * 4 * (60.0 / minBpm) * sampleRate)))
    , loops_(static_cast<size_t>(maxLoops))
    , maxLookbackBars_(maxLookbackBars)
    , sampleRate_(sampleRate)
{
    for (int i = 0; i < maxLoops; ++i) {
        loops_[static_cast<size_t>(i)].setId(i);
        loops_[static_cast<size_t>(i)].setCrossfadeSamples(crossfadeSamples_);
    }

    // Wire metronome callbacks
    metronome_.onBeat([this](const MetronomePosition& pos) {
        if (callbacks_.onBeat) callbacks_.onBeat(pos);
    });
    metronome_.onBar([this](const MetronomePosition& pos) {
        if (callbacks_.onBar) callbacks_.onBar(pos);
    });
}

void LoopEngine::processBlock(const float* input, float* output, int numSamples) {
    for (int i = 0; i < numSamples; ++i) {
        float inSample = input ? input[i] : 0.0f;

        // Write input to ring buffer
        ringBuffer_.write(&inSample, 1);

        // Accumulate into active classic recording if one is in progress
        if (activeRecording_) {
            activeRecording_->buffer.push_back(inSample);
        }

        // Check pending ops that should execute at or before this sample
        int64_t currentSample = metronome_.position().totalSamples;

        // Process pending ops that are due
        while (!pendingOps_.empty() &&
               pendingOps_.front().executeSample <= currentSample) {
            PendingOp op = pendingOps_.front();
            pendingOps_.pop_front();
            executeOp(op);
        }

        // Mix output from all playing loops
        float outSample = 0.0f;
        for (auto& lp : loops_) {
            if (!lp.isEmpty()) {
                outSample += lp.processSample();

                // Feed input to overdub-recording loops
                if (lp.isRecording()) {
                    lp.recordSample(inSample);
                }
            }
        }

        // Input monitoring
        if (inputMonitoring_) {
            outSample += inSample;
        }

        if (output) {
            output[i] = outSample;
        }

        // Advance metronome by 1 sample
        metronome_.advance(1);
    }
}

void LoopEngine::scheduleOp(OpType type, int loopIndex, Quantize quantize) {
    PendingOp op;
    op.type = type;
    op.loopIndex = loopIndex;
    op.quantize = quantize;

    if (quantize == Quantize::Free) {
        op.executeSample = metronome_.position().totalSamples;
    } else {
        op.executeSample = metronome_.position().totalSamples +
                          metronome_.samplesUntilBoundary(quantize);
    }

    // Insert sorted by execution time
    auto it = std::lower_bound(pendingOps_.begin(), pendingOps_.end(), op,
        [](const PendingOp& a, const PendingOp& b) {
            return a.executeSample < b.executeSample;
        });
    pendingOps_.insert(it, op);

    std::string msg = op.description();
    if (quantize != Quantize::Free) {
        msg += " (pending: ";
        msg += (quantize == Quantize::Beat ? "next beat" : "next bar");
        msg += ")";
    }
    lastMessage_ = msg;
    if (callbacks_.onMessage) callbacks_.onMessage(msg);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::scheduleCaptureLoop(int loopIndex, Quantize quantize,
                                     double lookbackBarsOverride) {
    PendingOp op;
    op.type = OpType::CaptureLoop;
    op.loopIndex = loopIndex < 0 ? nextEmptySlot() : loopIndex;
    op.quantize = quantize;

    // Duration is always in whole bars
    int bars = lookbackBarsOverride > 0
        ? static_cast<int>(std::round(lookbackBarsOverride))
        : lookbackBars_;
    op.lookbackSamples = static_cast<int64_t>(
        std::round(static_cast<double>(bars) * metronome_.samplesPerBar()));

    if (quantize == Quantize::Free) {
        op.executeSample = metronome_.position().totalSamples;
    } else {
        op.executeSample = metronome_.position().totalSamples +
                          metronome_.samplesUntilBoundary(quantize);
    }

    auto it = std::lower_bound(pendingOps_.begin(), pendingOps_.end(), op,
        [](const PendingOp& a, const PendingOp& b) {
            return a.executeSample < b.executeSample;
        });
    pendingOps_.insert(it, op);

    std::ostringstream msg;
    msg << "Capture " << bars << " bar(s) -> Loop " << op.loopIndex;
    if (quantize != Quantize::Free) {
        msg << " (pending: " << (quantize == Quantize::Beat ? "next beat" : "next bar") << ")";
    }
    lastMessage_ = msg.str();
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::scheduleSetSpeed(int loopIndex, double speed, Quantize quantize) {
    PendingOp op;
    op.type = OpType::SetSpeed;
    op.loopIndex = loopIndex;
    op.quantize = quantize;
    op.speedValue = speed;

    if (quantize == Quantize::Free) {
        op.executeSample = metronome_.position().totalSamples;
    } else {
        op.executeSample = metronome_.position().totalSamples +
                          metronome_.samplesUntilBoundary(quantize);
    }

    auto it = std::lower_bound(pendingOps_.begin(), pendingOps_.end(), op,
        [](const PendingOp& a, const PendingOp& b) {
            return a.executeSample < b.executeSample;
        });
    pendingOps_.insert(it, op);

    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::scheduleRecord(int loopIndex, Quantize quantize) {
    PendingOp op;
    op.type = OpType::Record;
    op.loopIndex = loopIndex < 0 ? nextEmptySlot() : loopIndex;
    op.quantize = quantize;

    if (quantize == Quantize::Free) {
        op.executeSample = metronome_.position().totalSamples;
    } else {
        op.executeSample = metronome_.position().totalSamples +
                          metronome_.samplesUntilBoundary(quantize);
    }

    auto it = std::lower_bound(pendingOps_.begin(), pendingOps_.end(), op,
        [](const PendingOp& a, const PendingOp& b) {
            return a.executeSample < b.executeSample;
        });
    pendingOps_.insert(it, op);

    std::ostringstream msg;
    msg << "Record -> Loop " << op.loopIndex;
    if (quantize != Quantize::Free) {
        msg << " (pending: " << (quantize == Quantize::Beat ? "next beat" : "next bar") << ")";
    }
    lastMessage_ = msg.str();
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::scheduleStopRecord(int loopIndex, Quantize quantize) {
    PendingOp op;
    op.type = OpType::StopRecord;
    op.loopIndex = loopIndex;
    op.quantize = quantize;

    if (quantize == Quantize::Free) {
        op.executeSample = metronome_.position().totalSamples;
    } else {
        op.executeSample = metronome_.position().totalSamples +
                          metronome_.samplesUntilBoundary(quantize);
    }

    auto it = std::lower_bound(pendingOps_.begin(), pendingOps_.end(), op,
        [](const PendingOp& a, const PendingOp& b) {
            return a.executeSample < b.executeSample;
        });
    pendingOps_.insert(it, op);

    std::string msg = "Stop Record (pending: ";
    msg += (quantize == Quantize::Beat ? "next beat" : "next bar");
    msg += ")";
    lastMessage_ = msg;
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::executeOpNow(OpType type, int loopIndex) {
    PendingOp op;
    op.type = type;
    op.loopIndex = loopIndex;
    op.quantize = Quantize::Free;
    op.executeSample = metronome_.position().totalSamples;

    if (type == OpType::CaptureLoop) {
        op.loopIndex = loopIndex < 0 ? nextEmptySlot() : loopIndex;
        op.lookbackSamples = static_cast<int64_t>(
            std::round(static_cast<double>(lookbackBars_) * metronome_.samplesPerBar()));
    }

    executeOp(op);
}

void LoopEngine::executeOp(const PendingOp& op) {
    int idx = op.loopIndex;

    // Record/StopRecord have their own validation
    if (op.type == OpType::Record) {
        executeRecord(op);
        return;
    }
    if (op.type == OpType::StopRecord) {
        executeStopRecord(op);
        return;
    }

    // Validate index
    if (idx < 0 || idx >= maxLoops()) {
        if (op.type == OpType::CaptureLoop) {
            lastMessage_ = "No empty loop slot available";
            if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
            return;
        }
        return;
    }

    Loop& lp = loops_[static_cast<size_t>(idx)];

    switch (op.type) {
        case OpType::CaptureLoop:
            executeCaptureLoop(op);
            break;
        case OpType::Mute:
            lp.mute();
            lastMessage_ = "Loop " + std::to_string(idx) + " muted";
            break;
        case OpType::Unmute:
            lp.play();
            lastMessage_ = "Loop " + std::to_string(idx) + " unmuted";
            break;
        case OpType::ToggleMute:
            lp.toggleMute();
            lastMessage_ = "Loop " + std::to_string(idx) +
                          (lp.isMuted() ? " muted" : " unmuted");
            break;
        case OpType::Reverse:
            lp.toggleReverse();
            lastMessage_ = "Loop " + std::to_string(idx) +
                          (lp.isReversed() ? " reversed" : " forward");
            break;
        case OpType::StartOverdub:
            lp.startOverdub();
            lastMessage_ = "Loop " + std::to_string(idx) + " overdub started";
            break;
        case OpType::StopOverdub:
            lp.stopOverdub();
            lastMessage_ = "Loop " + std::to_string(idx) + " overdub stopped";
            break;
        case OpType::UndoLayer:
            lp.undoLayer();
            lastMessage_ = "Loop " + std::to_string(idx) + " layer undone";
            break;
        case OpType::RedoLayer:
            lp.redoLayer();
            lastMessage_ = "Loop " + std::to_string(idx) + " layer redone";
            break;
        case OpType::SetSpeed:
            lp.setSpeed(op.speedValue);
            lastMessage_ = "Loop " + std::to_string(idx) + " speed: " +
                          std::to_string(op.speedValue) + "x";
            break;
        case OpType::ClearLoop:
            lp.clear();
            lastMessage_ = "Loop " + std::to_string(idx) + " cleared";
            break;
        case OpType::Record:
        case OpType::StopRecord:
            break; // Handled above
    }

    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::executeCaptureLoop(const PendingOp& op) {
    int idx = op.loopIndex;
    if (idx < 0 || idx >= maxLoops()) return;

    int64_t lookback = op.lookbackSamples;
    if (lookback <= 0) {
        lookback = static_cast<int64_t>(
            std::round(static_cast<double>(lookbackBars_) * metronome_.samplesPerBar()));
    }

    // Clamp to available data
    lookback = std::min(lookback, ringBuffer_.available());
    if (lookback <= 0) {
        lastMessage_ = "No audio to capture";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    std::vector<float> audio = ringBuffer_.capture(static_cast<int>(lookback));
    Loop& lp = loops_[static_cast<size_t>(idx)];
    lp.loadFromCapture(std::move(audio));
    lp.setCrossfadeSamples(crossfadeSamples_);

    double bars = static_cast<double>(lookback) / metronome_.samplesPerBar();
    lp.setLengthInBars(bars);

    std::ostringstream msg;
    msg << "Loop " << idx << " captured (" << static_cast<int>(std::round(bars)) << " bars)";
    lastMessage_ = msg.str();
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
}

void LoopEngine::executeRecord(const PendingOp& op) {
    int idx = op.loopIndex;
    if (idx < 0 || idx >= maxLoops()) {
        lastMessage_ = "No empty loop slot available";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    if (activeRecording_) {
        lastMessage_ = "Already recording on Loop " +
                       std::to_string(activeRecording_->loopIndex);
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    // Clear the target loop if it has content
    loops_[static_cast<size_t>(idx)].clear();

    // Start accumulating input
    ActiveRecording rec;
    rec.loopIndex = idx;
    rec.startSample = metronome_.position().totalSamples;
    activeRecording_ = std::move(rec);

    lastMessage_ = "Loop " + std::to_string(idx) + " recording...";
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::executeStopRecord(const PendingOp& op) {
    if (!activeRecording_) {
        lastMessage_ = "No active recording";
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    int idx = activeRecording_->loopIndex;

    // Ignore if the stop targets a different loop than what's recording
    if (op.loopIndex >= 0 && op.loopIndex != idx) {
        lastMessage_ = "Stop ignored: recording is on Loop " + std::to_string(idx);
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    if (activeRecording_->buffer.empty()) {
        lastMessage_ = "No audio recorded";
        activeRecording_.reset();
        if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
        return;
    }

    // Load the recorded audio into the loop
    Loop& lp = loops_[static_cast<size_t>(idx)];
    lp.loadFromCapture(std::move(activeRecording_->buffer));
    lp.setCrossfadeSamples(crossfadeSamples_);

    double bars = static_cast<double>(lp.lengthSamples()) / metronome_.samplesPerBar();
    lp.setLengthInBars(bars);

    activeRecording_.reset();

    std::ostringstream msg;
    msg << "Loop " << idx << " recorded ("
        << std::fixed << std::setprecision(1) << bars << " bars)";
    lastMessage_ = msg.str();
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::cancelPending() {
    pendingOps_.clear();
    lastMessage_ = "All pending ops cancelled";
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
}

void LoopEngine::cancelPending(int loopIndex) {
    pendingOps_.erase(
        std::remove_if(pendingOps_.begin(), pendingOps_.end(),
            [loopIndex](const PendingOp& op) { return op.loopIndex == loopIndex; }),
        pendingOps_.end()
    );
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

void LoopEngine::setLookbackBars(int bars) {
    lookbackBars_ = std::max(1, std::min(bars, maxLookbackBars_));
}

int LoopEngine::recordingLoopIndex() const {
    if (activeRecording_) return activeRecording_->loopIndex;
    return -1;
}

void LoopEngine::setCallbacks(EngineCallbacks cb) {
    callbacks_ = std::move(cb);

    // Re-wire metronome callbacks to include the new ones
    metronome_.onBeat([this](const MetronomePosition& pos) {
        if (callbacks_.onBeat) callbacks_.onBeat(pos);
    });
    metronome_.onBar([this](const MetronomePosition& pos) {
        if (callbacks_.onBar) callbacks_.onBar(pos);
    });
}

std::string LoopEngine::statusMessage() const {
    return lastMessage_;
}

} // namespace retrospect
