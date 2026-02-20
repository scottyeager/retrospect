#include "client/LocalEngineClient.h"

namespace retrospect {

LocalEngineClient::LocalEngineClient(LoopEngine& engine)
    : engine_(engine)
{
    // Initialize snapshot with correct sizing
    snap_.maxLoops = engine_.maxLoops();
    snap_.loops.resize(static_cast<size_t>(snap_.maxLoops));
    snap_.sampleRate = engine_.sampleRate();

    // Wire engine message callback to buffer messages for poll()
    EngineCallbacks callbacks;
    callbacks.onMessage = [this](const std::string& msg) {
        std::lock_guard<std::mutex> lock(msgMutex_);
        pendingMessages_.push_back(msg);
    };
    callbacks.onStateChanged = []() {};
    callbacks.onBeat = [](const MetronomePosition&) {};
    callbacks.onBar = [](const MetronomePosition&) {};
    engine_.setCallbacks(std::move(callbacks));
}

void LocalEngineClient::scheduleCaptureLoop(int loopIndex, Quantize quantize,
                                            int lookbackBars) {
    engine_.scheduleCaptureLoop(loopIndex, quantize,
                                lookbackBars > 0 ? static_cast<double>(lookbackBars) : 0.0);
}

void LocalEngineClient::scheduleRecord(int loopIndex, Quantize quantize) {
    engine_.scheduleRecord(loopIndex, quantize);
}

void LocalEngineClient::scheduleStopRecord(int loopIndex, Quantize quantize) {
    engine_.scheduleStopRecord(loopIndex, quantize);
}

void LocalEngineClient::scheduleOp(OpType type, int loopIndex, Quantize quantize) {
    engine_.scheduleOp(type, loopIndex, quantize);
}

void LocalEngineClient::scheduleSetSpeed(int loopIndex, double speed,
                                         Quantize quantize) {
    engine_.scheduleSetSpeed(loopIndex, speed, quantize);
}

void LocalEngineClient::executeOpNow(OpType type, int loopIndex) {
    engine_.executeOpNow(type, loopIndex);
}

void LocalEngineClient::cancelPending() {
    engine_.cancelPending();
}

void LocalEngineClient::setDefaultQuantize(Quantize q) {
    engine_.setDefaultQuantize(q);
}

void LocalEngineClient::setLookbackBars(int bars) {
    engine_.setLookbackBars(bars);
}

void LocalEngineClient::setMetronomeClickEnabled(bool on) {
    engine_.setMetronomeClickEnabled(on);
}

void LocalEngineClient::setBpm(double bpm) {
    // Use the command queue for thread-safe BPM changes
    EngineCommand cmd;
    cmd.commandType = CommandType::SetBpm;
    cmd.value = bpm;
    engine_.enqueueCommand(cmd);
}

void LocalEngineClient::poll() {
    // Metronome
    const auto& met = engine_.metronome();
    auto pos = met.position();
    snap_.metronome.bar = pos.bar;
    snap_.metronome.beat = pos.beat;
    snap_.metronome.beatFraction = pos.beatFraction;
    snap_.metronome.bpm = met.bpm();
    snap_.metronome.beatsPerBar = met.beatsPerBar();
    snap_.metronome.running = met.isRunning();

    // Loops
    int active = 0;
    for (int i = 0; i < engine_.maxLoops(); ++i) {
        const auto& lp = engine_.loop(i);
        auto& ls = snap_.loops[static_cast<size_t>(i)];
        ls.state = lp.state();
        ls.lengthInBars = lp.lengthInBars();
        ls.layers = lp.layerCount();
        ls.activeLayers = lp.activeLayerCount();
        ls.speed = lp.speed();
        ls.reversed = lp.isReversed();
        ls.playPosition = lp.playPosition();
        ls.lengthSamples = lp.lengthSamples();
        if (!lp.isEmpty()) ++active;
    }
    snap_.activeLoopCount = active;

    // Recording state
    snap_.isRecording = engine_.isRecordingAtomic();
    snap_.recordingLoopIndex = engine_.recordingLoopIdxAtomic();

    // Pending ops
    auto ops = engine_.pendingOpsSnapshot();
    snap_.pendingOps.clear();
    snap_.pendingOps.reserve(ops.size());
    for (const auto& op : ops) {
        PendingOpSnapshot pos;
        pos.loopIndex = op.loopIndex;
        pos.quantize = op.quantize;
        pos.description = op.description();
        snap_.pendingOps.push_back(std::move(pos));
    }

    // Settings
    snap_.defaultQuantize = engine_.defaultQuantize();
    snap_.lookbackBars = engine_.lookbackBars();
    snap_.clickEnabled = engine_.metronomeClickEnabled();

    // Drain buffered messages
    {
        std::lock_guard<std::mutex> lock(msgMutex_);
        snap_.messages = std::move(pendingMessages_);
        pendingMessages_.clear();
    }
}

} // namespace retrospect
