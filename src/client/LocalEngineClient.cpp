#include "client/LocalEngineClient.h"
#include <algorithm>

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

int LocalEngineClient::setLookbackBars(int bars) {
    return engine_.setLookbackBars(bars);
}

void LocalEngineClient::setMetronomeClickEnabled(bool on) {
    engine_.setMetronomeClickEnabled(on);
}

void LocalEngineClient::setMidiSyncEnabled(bool on) {
    engine_.setMidiSyncEnabled(on);
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
        ls.recordedBpm = lp.recordedBpm();
        ls.timeStretchActive = lp.isTimeStretchActive();
        if (!lp.isEmpty()) ++active;
    }
    snap_.activeLoopCount = active;

    // Recording state
    snap_.isRecording = engine_.isRecordingAtomic();
    snap_.recordingLoopIndex = engine_.recordingLoopIdxAtomic();

    // Pending ops — gathered from each loop's pending state
    snap_.pendingOps.clear();
    for (int i = 0; i < engine_.maxLoops(); ++i) {
        const auto& lp = engine_.loop(i);
        const auto& ps = lp.pendingState();
        auto addOp = [&](const std::string& desc, Quantize q, int64_t execSample) {
            PendingOpSnapshot pos;
            pos.loopIndex = i;
            pos.quantize = q;
            pos.description = desc;
            pos.executeSample = execSample;
            snap_.pendingOps.push_back(std::move(pos));
        };
        if (ps.capture) addOp("Capture Loop", ps.capture->quantize, ps.capture->executeSample);
        if (ps.record) {
            std::string desc = (ps.recordOp == PendingState::RecordOp::Start) ? "Record" : "Stop Record";
            addOp(desc, ps.record->quantize, ps.record->executeSample);
        }
        if (ps.mute) {
            std::string desc;
            switch (ps.muteOp) {
                case PendingState::MuteOp::Mute:   desc = "Mute"; break;
                case PendingState::MuteOp::Unmute:  desc = "Unmute"; break;
                case PendingState::MuteOp::Toggle:  desc = "Toggle Mute"; break;
            }
            addOp(desc, ps.mute->quantize, ps.mute->executeSample);
        }
        if (ps.overdub) {
            std::string desc = (ps.overdubOp == PendingState::OverdubOp::Start) ? "Start Overdub" : "Stop Overdub";
            addOp(desc, ps.overdub->quantize, ps.overdub->executeSample);
        }
        if (ps.reverse) addOp("Reverse", ps.reverse->quantize, ps.reverse->executeSample);
        if (ps.speed) addOp("Set Speed", ps.speed->quantize, ps.speed->executeSample);
        if (ps.undo) {
            std::string desc = (ps.undo->direction == UndoDirection::Undo) ? "Undo Layer" : "Redo Layer";
            if (ps.undo->count > 1) desc += " x" + std::to_string(ps.undo->count);
            addOp(desc, ps.undo->quantize, ps.undo->executeSample);
        }
        if (ps.clear) addOp("Clear", ps.clear->quantize, ps.clear->executeSample);
    }
    // Sort by execution time for display consistency
    std::sort(snap_.pendingOps.begin(), snap_.pendingOps.end(),
              [](const PendingOpSnapshot& a, const PendingOpSnapshot& b) {
                  return a.executeSample < b.executeSample;
              });

    // Input channel live status
    {
        int numCh = engine_.numInputChannels();
        uint64_t mask = engine_.liveChannelMask();
        auto peaks = engine_.channelPeaksSnapshot();
        snap_.inputChannels.resize(static_cast<size_t>(numCh));
        for (int ch = 0; ch < numCh; ++ch) {
            auto& cs = snap_.inputChannels[static_cast<size_t>(ch)];
            cs.live = (mask >> ch) & 1;
            cs.peakLevel = (ch < static_cast<int>(peaks.size()))
                ? peaks[static_cast<size_t>(ch)] : 0.0f;
        }
    }

    // Settings
    snap_.defaultQuantize = engine_.defaultQuantize();
    snap_.lookbackBars = engine_.lookbackBars();
    snap_.clickEnabled = engine_.metronomeClickEnabled();
    snap_.midiSyncEnabled = engine_.midiSyncEnabled();
    snap_.midiOutputAvailable = engine_.midiSync().hasOutput();
    snap_.liveThreshold = engine_.liveThreshold();

    // Drain buffered messages
    {
        std::lock_guard<std::mutex> lock(msgMutex_);
        snap_.messages = std::move(pendingMessages_);
        pendingMessages_.clear();
    }
}

} // namespace retrospect
