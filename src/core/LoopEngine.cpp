#include "core/LoopEngine.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace retrospect {

// PendingOp description
std::string PendingOp::description() const {
    switch (type) {
        case OpType::CaptureLoop:  return "Capture Loop";
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

LoopEngine::LoopEngine(int maxLoops, double lookbackSeconds, double sampleRate)
    : metronome_(120.0, 4, sampleRate)
    , ringBuffer_(static_cast<int64_t>(lookbackSeconds * sampleRate))
    , loops_(static_cast<size_t>(maxLoops))
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

                // Feed input to recording loops
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
                                     double lookbackBars) {
    PendingOp op;
    op.type = OpType::CaptureLoop;
    op.loopIndex = loopIndex < 0 ? nextEmptySlot() : loopIndex;
    op.quantize = quantize;

    double bars = lookbackBars > 0 ? lookbackBars : lookbackBars_;
    op.lookbackSamples = static_cast<int64_t>(
        std::round(bars * metronome_.samplesPerBar()));

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

void LoopEngine::executeOpNow(OpType type, int loopIndex) {
    PendingOp op;
    op.type = type;
    op.loopIndex = loopIndex;
    op.quantize = Quantize::Free;
    op.executeSample = metronome_.position().totalSamples;

    if (type == OpType::CaptureLoop) {
        op.loopIndex = loopIndex < 0 ? nextEmptySlot() : loopIndex;
        op.lookbackSamples = static_cast<int64_t>(
            std::round(lookbackBars_ * metronome_.samplesPerBar()));
    }

    executeOp(op);
}

void LoopEngine::executeOp(const PendingOp& op) {
    int idx = op.loopIndex;

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
            std::round(lookbackBars_ * metronome_.samplesPerBar()));
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
    msg << "Loop " << idx << " captured (" << bars << " bars)";
    lastMessage_ = msg.str();
    if (callbacks_.onMessage) callbacks_.onMessage(lastMessage_);
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
