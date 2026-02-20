#include "client/OscEngineClient.h"
#include "core/LoopEngine.h"  // For OpType

#include <cstdio>

namespace retrospect {

OscEngineClient::OscEngineClient(const std::string& host, const std::string& port)
    : host_(host)
    , port_(port)
{
    // Create non-threaded server on ephemeral port for receiving state pushes
    server_ = lo_server_new(nullptr, errorHandler);
    if (!server_) {
        fprintf(stderr, "OscEngineClient: failed to create local server\n");
        return;
    }
    localPort_ = lo_server_get_port(server_);

    // Create address for sending commands to the server
    serverAddr_ = lo_address_new(host_.c_str(), port_.c_str());
    if (!serverAddr_) {
        fprintf(stderr, "OscEngineClient: failed to create server address %s:%s\n",
                host_.c_str(), port_.c_str());
        lo_server_free(server_);
        server_ = nullptr;
        return;
    }

    // Register state handlers
    lo_server_add_method(server_, "/retro/state/metronome", "iiddii",
                         handleMetronome, this);
    lo_server_add_method(server_, "/retro/state/loop", "iidiiddih",
                         handleLoop, this);
    lo_server_add_method(server_, "/retro/state/recording", "ii",
                         handleRecording, this);
    lo_server_add_method(server_, "/retro/state/settings", "iiiiii",
                         handleSettings, this);
    lo_server_add_method(server_, "/retro/state/pending_clear", "",
                         handlePendingClear, this);
    lo_server_add_method(server_, "/retro/state/pending_op", "iis",
                         handlePendingOp, this);
    lo_server_add_method(server_, "/retro/state/log", "s",
                         handleLog, this);

    // Initialize default snapshot
    snap_.loops.resize(8);
    snap_.maxLoops = 8;

    fprintf(stderr, "OscEngineClient: connecting to %s:%s, listening on port %d\n",
            host_.c_str(), port_.c_str(), localPort_);

    // Send initial subscribe
    subscribe();
}

OscEngineClient::~OscEngineClient() {
    // Unsubscribe
    if (serverAddr_ && server_) {
        lo_send(serverAddr_, "/retro/client/unsubscribe", "si",
                "localhost", localPort_);
    }

    if (serverAddr_) lo_address_free(serverAddr_);
    if (server_) lo_server_free(server_);
}

void OscEngineClient::subscribe() {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/client/subscribe", "si",
            "localhost", localPort_);
    lastSubscribe_ = std::chrono::steady_clock::now();
}

void OscEngineClient::poll() {
    if (!server_) return;

    // Clear per-frame data
    snap_.messages.clear();

    // Drain all pending OSC messages (non-blocking)
    while (lo_server_recv_noblock(server_, 0) > 0) {
        // Messages are processed by handlers
    }

    // Periodic heartbeat/resubscribe
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - lastSubscribe_).count();
    if (elapsed >= kHeartbeatIntervalSec) {
        subscribe();
    }
}

// --- Commands ---

void OscEngineClient::scheduleCaptureLoop(int loopIndex, Quantize quantize,
                                           int lookbackBars) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/loop/capture", "iii",
            loopIndex, quantizeToInt(quantize), lookbackBars);
}

void OscEngineClient::scheduleRecord(int loopIndex, Quantize quantize) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/loop/record", "ii",
            loopIndex, quantizeToInt(quantize));
}

void OscEngineClient::scheduleStopRecord(int loopIndex, Quantize quantize) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/loop/stop_record", "ii",
            loopIndex, quantizeToInt(quantize));
}

void OscEngineClient::scheduleOp(OpType type, int loopIndex, Quantize quantize) {
    if (!serverAddr_) return;

    const char* path = nullptr;
    switch (type) {
        case OpType::Mute:         path = "/retro/loop/mute"; break;
        case OpType::Unmute:       path = "/retro/loop/unmute"; break;
        case OpType::ToggleMute:   path = "/retro/loop/toggle_mute"; break;
        case OpType::Reverse:      path = "/retro/loop/reverse"; break;
        case OpType::StartOverdub: path = "/retro/loop/overdub/start"; break;
        case OpType::StopOverdub:  path = "/retro/loop/overdub/stop"; break;
        case OpType::UndoLayer:
            lo_send(serverAddr_, "/retro/loop/undo", "i", loopIndex);
            return;
        case OpType::RedoLayer:
            lo_send(serverAddr_, "/retro/loop/redo", "i", loopIndex);
            return;
        case OpType::ClearLoop:
            lo_send(serverAddr_, "/retro/loop/clear", "i", loopIndex);
            return;
        case OpType::CaptureLoop:
            scheduleCaptureLoop(loopIndex, quantize);
            return;
        case OpType::Record:
            scheduleRecord(loopIndex, quantize);
            return;
        case OpType::StopRecord:
            scheduleStopRecord(loopIndex, quantize);
            return;
        case OpType::SetSpeed:
            return;  // Use scheduleSetSpeed instead
    }

    if (path) {
        lo_send(serverAddr_, path, "ii",
                loopIndex, quantizeToInt(quantize));
    }
}

void OscEngineClient::scheduleSetSpeed(int loopIndex, double speed,
                                        Quantize quantize) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/loop/speed", "idi",
            loopIndex, speed, quantizeToInt(quantize));
}

void OscEngineClient::executeOpNow(OpType type, int loopIndex) {
    scheduleOp(type, loopIndex, Quantize::Free);
}

void OscEngineClient::cancelPending() {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/cancel_pending", "");
}

void OscEngineClient::setDefaultQuantize(Quantize q) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/settings/quantize", "i", quantizeToInt(q));
    // Also update local snapshot for immediate UI feedback
    snap_.defaultQuantize = q;
}

void OscEngineClient::setLookbackBars(int bars) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/settings/lookback_bars", "i", bars);
    snap_.lookbackBars = bars;
}

void OscEngineClient::setMetronomeClickEnabled(bool on) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/metronome/click", "i", on ? 1 : 0);
    snap_.clickEnabled = on;
}

void OscEngineClient::setMidiSyncEnabled(bool on) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/settings/midi_sync", "i", on ? 1 : 0);
    snap_.midiSyncEnabled = on;
}

void OscEngineClient::setBpm(double bpm) {
    if (!serverAddr_) return;
    lo_send(serverAddr_, "/retro/metronome/bpm", "d", bpm);
}

// --- State handlers ---

int OscEngineClient::handleMetronome(const char*, const char*, lo_arg** argv,
                                      int, lo_message, void* user) {
    auto* self = static_cast<OscEngineClient*>(user);
    auto& met = self->snap_.metronome;
    met.bar = argv[0]->i;
    met.beat = argv[1]->i;
    met.beatFraction = argv[2]->d;
    met.bpm = argv[3]->d;
    met.beatsPerBar = argv[4]->i;
    met.running = argv[5]->i != 0;
    return 0;
}

int OscEngineClient::handleLoop(const char*, const char*, lo_arg** argv,
                                 int, lo_message, void* user) {
    auto* self = static_cast<OscEngineClient*>(user);
    int idx = argv[0]->i;

    // Resize if needed
    if (idx >= static_cast<int>(self->snap_.loops.size())) {
        self->snap_.loops.resize(static_cast<size_t>(idx + 1));
        self->snap_.maxLoops = static_cast<int>(self->snap_.loops.size());
    }

    auto& lp = self->snap_.loops[static_cast<size_t>(idx)];
    lp.state = intToLoopState(argv[1]->i);
    lp.lengthInBars = argv[2]->d;
    lp.layers = argv[3]->i;
    lp.activeLayers = argv[4]->i;
    lp.speed = argv[5]->d;
    lp.reversed = argv[6]->i != 0;
    double playPosPct = argv[7]->d;
    lp.lengthSamples = argv[8]->h;
    lp.playPosition = static_cast<int64_t>(playPosPct * static_cast<double>(lp.lengthSamples));

    // Update active loop count
    int active = 0;
    for (const auto& l : self->snap_.loops) {
        if (!l.isEmpty()) ++active;
    }
    self->snap_.activeLoopCount = active;

    return 0;
}

int OscEngineClient::handleRecording(const char*, const char*, lo_arg** argv,
                                      int, lo_message, void* user) {
    auto* self = static_cast<OscEngineClient*>(user);
    self->snap_.isRecording = argv[0]->i != 0;
    self->snap_.recordingLoopIndex = argv[1]->i;
    return 0;
}

int OscEngineClient::handleSettings(const char*, const char*, lo_arg** argv,
                                     int, lo_message, void* user) {
    auto* self = static_cast<OscEngineClient*>(user);
    self->snap_.defaultQuantize = intToQuantize(argv[0]->i);
    self->snap_.lookbackBars = argv[1]->i;
    self->snap_.clickEnabled = argv[2]->i != 0;
    self->snap_.sampleRate = static_cast<double>(argv[3]->i);
    self->snap_.midiSyncEnabled = argv[4]->i != 0;
    self->snap_.midiOutputAvailable = argv[5]->i != 0;
    return 0;
}

int OscEngineClient::handlePendingClear(const char*, const char*, lo_arg**,
                                         int, lo_message, void* user) {
    auto* self = static_cast<OscEngineClient*>(user);
    self->snap_.pendingOps.clear();
    return 0;
}

int OscEngineClient::handlePendingOp(const char*, const char*, lo_arg** argv,
                                      int, lo_message, void* user) {
    auto* self = static_cast<OscEngineClient*>(user);
    PendingOpSnapshot op;
    op.loopIndex = argv[0]->i;
    op.quantize = intToQuantize(argv[1]->i);
    op.description = &argv[2]->s;
    self->snap_.pendingOps.push_back(std::move(op));
    return 0;
}

int OscEngineClient::handleLog(const char*, const char*, lo_arg** argv,
                                int, lo_message, void* user) {
    auto* self = static_cast<OscEngineClient*>(user);
    self->snap_.messages.push_back(&argv[0]->s);
    return 0;
}

void OscEngineClient::errorHandler(int num, const char* msg, const char* path) {
    fprintf(stderr, "OscEngineClient error %d: %s (path: %s)\n",
            num, msg, path ? path : "null");
}

} // namespace retrospect
