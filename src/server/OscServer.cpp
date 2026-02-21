#include "server/OscServer.h"
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace retrospect {

OscServer::OscServer(LoopEngine& engine, const std::string& port)
    : engine_(engine)
    , port_(port)
{
}

OscServer::~OscServer() {
    stop();
}

bool OscServer::start() {
    serverThread_ = lo_server_thread_new(port_.c_str(), errorHandler);
    if (!serverThread_) {
        fprintf(stderr, "OscServer: failed to create server on port %s\n", port_.c_str());
        return false;
    }

    // Register all command handlers
    lo_server_thread_add_method(serverThread_, "/retro/loop/capture", "iii",
                                handleCaptureLoop, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/record", "ii",
                                handleRecord, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/stop_record", "ii",
                                handleStopRecord, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/mute", "ii",
                                handleMute, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/unmute", "ii",
                                handleUnmute, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/toggle_mute", "ii",
                                handleToggleMute, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/reverse", "ii",
                                handleReverse, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/overdub/start", "ii",
                                handleOverdubStart, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/overdub/stop", "ii",
                                handleOverdubStop, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/undo", "i",
                                handleUndo, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/redo", "i",
                                handleRedo, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/speed", "idi",
                                handleSpeed, this);
    lo_server_thread_add_method(serverThread_, "/retro/loop/clear", "i",
                                handleClear, this);
    lo_server_thread_add_method(serverThread_, "/retro/metronome/bpm", "d",
                                handleBpm, this);
    lo_server_thread_add_method(serverThread_, "/retro/metronome/click", "i",
                                handleClick, this);
    lo_server_thread_add_method(serverThread_, "/retro/settings/midi_sync", "i",
                                handleMidiSync, this);
    lo_server_thread_add_method(serverThread_, "/retro/settings/quantize", "i",
                                handleQuantize, this);
    lo_server_thread_add_method(serverThread_, "/retro/settings/lookback_bars", "i",
                                handleLookbackBars, this);
    lo_server_thread_add_method(serverThread_, "/retro/cancel_pending", "",
                                handleCancelPending, this);
    lo_server_thread_add_method(serverThread_, "/retro/client/subscribe", "si",
                                handleSubscribe, this);
    lo_server_thread_add_method(serverThread_, "/retro/client/unsubscribe", "si",
                                handleUnsubscribe, this);

    // Wire engine message callback to buffer for state pushes
    EngineCallbacks callbacks;
    callbacks.onMessage = [this](const std::string& msg) {
        std::lock_guard<std::mutex> lock(msgMutex_);
        pendingMessages_.push_back(msg);
    };
    callbacks.onStateChanged = []() {};
    callbacks.onBeat = [](const MetronomePosition&) {};
    callbacks.onBar = [](const MetronomePosition&) {};
    engine_.setCallbacks(std::move(callbacks));

    lo_server_thread_start(serverThread_);
    fprintf(stderr, "OscServer: listening on port %s\n", port_.c_str());
    return true;
}

void OscServer::stop() {
    if (serverThread_) {
        lo_server_thread_stop(serverThread_);
        lo_server_thread_free(serverThread_);
        serverThread_ = nullptr;
    }

    // Clean up subscriber addresses
    std::lock_guard<std::mutex> lock(subMutex_);
    for (auto& sub : subscribers_) {
        if (sub.addr) lo_address_free(sub.addr);
    }
    subscribers_.clear();
}

void OscServer::pushState() {
    pruneSubscribers();

    std::lock_guard<std::mutex> lock(subMutex_);
    for (const auto& sub : subscribers_) {
        pushStateTo(sub.addr);
    }
}

void OscServer::pushStateTo(lo_address addr) {
    const auto& met = engine_.metronome();
    auto pos = met.position();

    // Metronome: iiddii
    lo_send(addr, "/retro/state/metronome", "iiddii",
            pos.bar, pos.beat, pos.beatFraction,
            met.bpm(), met.beatsPerBar(),
            met.isRunning() ? 1 : 0);

    // Loops: one message per loop
    for (int i = 0; i < engine_.maxLoops(); ++i) {
        const auto& lp = engine_.loop(i);
        double playPosPct = 0.0;
        if (lp.lengthSamples() > 0) {
            playPosPct = static_cast<double>(lp.playPosition()) /
                         static_cast<double>(lp.lengthSamples());
        }

        lo_send(addr, "/retro/state/loop", "iidiiddih",
                i,
                loopStateToInt(lp.state()),
                lp.lengthInBars(),
                lp.layerCount(),
                lp.activeLayerCount(),
                lp.speed(),
                lp.isReversed() ? 1 : 0,
                playPosPct,
                static_cast<int64_t>(lp.lengthSamples()));
    }

    // Recording state
    lo_send(addr, "/retro/state/recording", "ii",
            engine_.isRecordingAtomic() ? 1 : 0,
            engine_.recordingLoopIdxAtomic());

    // Settings
    lo_send(addr, "/retro/state/settings", "iiiiii",
            quantizeToInt(engine_.defaultQuantize()),
            engine_.lookbackBars(),
            engine_.metronomeClickEnabled() ? 1 : 0,
            static_cast<int>(engine_.sampleRate()),
            engine_.midiSyncEnabled() ? 1 : 0,
            engine_.midiSync().hasOutput() ? 1 : 0);

    // Pending ops: send clear first, then each op from loop-level state
    lo_send(addr, "/retro/state/pending_clear", "");

    for (int i = 0; i < engine_.maxLoops(); ++i) {
        const auto& lp = engine_.loop(i);
        const auto& ps = lp.pendingState();
        auto sendOp = [&](const char* desc, Quantize q) {
            lo_send(addr, "/retro/state/pending_op", "iis",
                    i, quantizeToInt(q), desc);
        };
        if (ps.capture) sendOp("Capture Loop", ps.capture->quantize);
        if (ps.record) {
            const char* desc = (ps.recordOp == PendingState::RecordOp::Start) ? "Record" : "Stop Record";
            sendOp(desc, ps.record->quantize);
        }
        if (ps.mute) {
            const char* desc = "Toggle Mute";
            if (ps.muteOp == PendingState::MuteOp::Mute) desc = "Mute";
            else if (ps.muteOp == PendingState::MuteOp::Unmute) desc = "Unmute";
            sendOp(desc, ps.mute->quantize);
        }
        if (ps.overdub) {
            const char* desc = (ps.overdubOp == PendingState::OverdubOp::Start) ? "Start Overdub" : "Stop Overdub";
            sendOp(desc, ps.overdub->quantize);
        }
        if (ps.reverse) sendOp("Reverse", ps.reverse->quantize);
        if (ps.speed) sendOp("Set Speed", ps.speed->quantize);
        if (ps.undo) {
            const char* desc = (ps.undo->direction == UndoDirection::Undo) ? "Undo Layer" : "Redo Layer";
            sendOp(desc, ps.undo->quantize);
        }
        if (ps.clear) sendOp("Clear", ps.clear->quantize);
    }

    // Log messages
    {
        std::lock_guard<std::mutex> lock(msgMutex_);
        for (const auto& msg : pendingMessages_) {
            lo_send(addr, "/retro/state/log", "s", msg.c_str());
        }
        pendingMessages_.clear();
    }
}

// --- Static handler trampolines ---

int OscServer::handleCaptureLoop(const char*, const char*, lo_arg** argv,
                                  int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    int loopIdx = argv[0]->i;
    Quantize q = intToQuantize(argv[1]->i);
    int lookback = argv[2]->i;
    self->engine_.scheduleCaptureLoop(loopIdx, q,
                                      lookback > 0 ? static_cast<double>(lookback) : 0.0);
    return 0;
}

int OscServer::handleRecord(const char*, const char*, lo_arg** argv,
                             int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.scheduleRecord(argv[0]->i, intToQuantize(argv[1]->i));
    return 0;
}

int OscServer::handleStopRecord(const char*, const char*, lo_arg** argv,
                                 int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.scheduleStopRecord(argv[0]->i, intToQuantize(argv[1]->i));
    return 0;
}

int OscServer::handleMute(const char*, const char*, lo_arg** argv,
                           int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->handleSimpleOp(OpType::Mute, argv);
    return 0;
}

int OscServer::handleUnmute(const char*, const char*, lo_arg** argv,
                             int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->handleSimpleOp(OpType::Unmute, argv);
    return 0;
}

int OscServer::handleToggleMute(const char*, const char*, lo_arg** argv,
                                 int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->handleSimpleOp(OpType::ToggleMute, argv);
    return 0;
}

int OscServer::handleReverse(const char*, const char*, lo_arg** argv,
                              int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->handleSimpleOp(OpType::Reverse, argv);
    return 0;
}

int OscServer::handleOverdubStart(const char*, const char*, lo_arg** argv,
                                   int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->handleSimpleOp(OpType::StartOverdub, argv);
    return 0;
}

int OscServer::handleOverdubStop(const char*, const char*, lo_arg** argv,
                                  int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->handleSimpleOp(OpType::StopOverdub, argv);
    return 0;
}

int OscServer::handleUndo(const char*, const char*, lo_arg** argv,
                           int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.scheduleOp(OpType::UndoLayer, argv[0]->i, Quantize::Free);
    return 0;
}

int OscServer::handleRedo(const char*, const char*, lo_arg** argv,
                           int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.scheduleOp(OpType::RedoLayer, argv[0]->i, Quantize::Free);
    return 0;
}

int OscServer::handleSpeed(const char*, const char*, lo_arg** argv,
                            int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    int loopIdx = argv[0]->i;
    double speed = argv[1]->d;
    Quantize q = intToQuantize(argv[2]->i);
    self->engine_.scheduleSetSpeed(loopIdx, speed, q);
    return 0;
}

int OscServer::handleClear(const char*, const char*, lo_arg** argv,
                            int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.executeOpNow(OpType::ClearLoop, argv[0]->i);
    return 0;
}

int OscServer::handleBpm(const char*, const char*, lo_arg** argv,
                          int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    EngineCommand cmd;
    cmd.commandType = CommandType::SetBpm;
    cmd.value = argv[0]->d;
    self->engine_.enqueueCommand(cmd);
    return 0;
}

int OscServer::handleClick(const char*, const char*, lo_arg** argv,
                            int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.setMetronomeClickEnabled(argv[0]->i != 0);
    return 0;
}

int OscServer::handleMidiSync(const char*, const char*, lo_arg** argv,
                               int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.setMidiSyncEnabled(argv[0]->i != 0);
    return 0;
}

int OscServer::handleQuantize(const char*, const char*, lo_arg** argv,
                               int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.setDefaultQuantize(intToQuantize(argv[0]->i));
    return 0;
}

int OscServer::handleLookbackBars(const char*, const char*, lo_arg** argv,
                                   int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    int requested = argv[0]->i;
    int actual = self->engine_.setLookbackBars(requested);
    if (actual != requested) {
        std::lock_guard<std::mutex> lock(self->msgMutex_);
        self->pendingMessages_.push_back(
            "Lookback clamped to " + std::to_string(actual) + " bar(s) (max " +
            std::to_string(self->engine_.maxLookbackBars()) + ")");
    }
    return 0;
}

int OscServer::handleCancelPending(const char*, const char*, lo_arg**,
                                    int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->engine_.cancelPending();
    return 0;
}

int OscServer::handleSubscribe(const char*, const char*, lo_arg** argv,
                                int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->addSubscriber(&argv[0]->s, argv[1]->i);
    return 0;
}

int OscServer::handleUnsubscribe(const char*, const char*, lo_arg** argv,
                                  int, lo_message, void* user) {
    auto* self = static_cast<OscServer*>(user);
    self->removeSubscriber(&argv[0]->s, argv[1]->i);
    return 0;
}

void OscServer::errorHandler(int num, const char* msg, const char* path) {
    fprintf(stderr, "OscServer error %d: %s (path: %s)\n",
            num, msg, path ? path : "null");
}

void OscServer::handleSimpleOp(OpType type, lo_arg** argv) {
    int loopIdx = argv[0]->i;
    Quantize q = intToQuantize(argv[1]->i);
    engine_.scheduleOp(type, loopIdx, q);
}

void OscServer::addSubscriber(const char* url, int port) {
    std::string portStr = std::to_string(port);
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(subMutex_);

    // Check if already subscribed (refresh timestamp)
    for (auto& sub : subscribers_) {
        const char* existingUrl = lo_address_get_hostname(sub.addr);
        const char* existingPort = lo_address_get_port(sub.addr);
        if (std::strcmp(existingUrl, url) == 0 &&
            std::strcmp(existingPort, portStr.c_str()) == 0) {
            sub.lastSeen = now;
            return;
        }
    }

    // New subscriber
    OscSubscriber sub;
    sub.addr = lo_address_new(url, portStr.c_str());
    sub.lastSeen = now;
    subscribers_.push_back(sub);
    fprintf(stderr, "OscServer: client subscribed %s:%d\n", url, port);
}

void OscServer::removeSubscriber(const char* url, int port) {
    std::string portStr = std::to_string(port);

    std::lock_guard<std::mutex> lock(subMutex_);
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
            [&](const OscSubscriber& sub) {
                const char* h = lo_address_get_hostname(sub.addr);
                const char* p = lo_address_get_port(sub.addr);
                if (std::strcmp(h, url) == 0 && std::strcmp(p, portStr.c_str()) == 0) {
                    lo_address_free(sub.addr);
                    return true;
                }
                return false;
            }),
        subscribers_.end());
    fprintf(stderr, "OscServer: client unsubscribed %s:%d\n", url, port);
}

void OscServer::pruneSubscribers() {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(subMutex_);
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
            [&](const OscSubscriber& sub) {
                double age = std::chrono::duration<double>(now - sub.lastSeen).count();
                if (age > kSubscriberTimeoutSec) {
                    fprintf(stderr, "OscServer: pruning stale client %s:%s\n",
                            lo_address_get_hostname(sub.addr),
                            lo_address_get_port(sub.addr));
                    lo_address_free(sub.addr);
                    return true;
                }
                return false;
            }),
        subscribers_.end());
}

} // namespace retrospect
