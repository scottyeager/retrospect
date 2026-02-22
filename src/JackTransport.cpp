#include "JackTransport.h"

#include <cmath>
#include <cstdio>

namespace retrospect {

JackTransport::JackTransport(double sampleRate)
    : sampleRate_(sampleRate)
{}

JackTransport::~JackTransport() {
    shutdown();
}

bool JackTransport::init() {
    if (client_) return true;  // already initialised

    jack_status_t status{};
    client_ = jack_client_open("Retrospect Transport", JackNoStartServer, &status);
    if (!client_) {
        fprintf(stderr, "JackTransport: could not open JACK client (status 0x%x)\n",
                static_cast<unsigned>(status));
        return false;
    }

    // Register as unconditional timebase master.
    int err = jack_set_timebase_callback(client_, /*conditional=*/0,
                                         timebaseCallback, this);
    if (err != 0) {
        fprintf(stderr, "JackTransport: failed to become timebase master\n");
        jack_client_close(client_);
        client_ = nullptr;
        return false;
    }

    // Activate the client (no ports, but required for the callback).
    if (jack_activate(client_) != 0) {
        fprintf(stderr, "JackTransport: failed to activate JACK client\n");
        jack_client_close(client_);
        client_ = nullptr;
        return false;
    }

    active_ = true;
    fprintf(stderr, "JackTransport: active as timebase master\n");
    return true;
}

void JackTransport::shutdown() {
    if (!client_) return;

    if (active_) {
        jack_release_timebase(client_);
        jack_deactivate(client_);
        active_ = false;
    }
    jack_client_close(client_);
    client_ = nullptr;
}

// ---------------------------------------------------------------------------
// Transport control
// ---------------------------------------------------------------------------

void JackTransport::start() {
    if (client_) jack_transport_start(client_);
}

void JackTransport::stop() {
    if (client_) jack_transport_stop(client_);
}

void JackTransport::rewind() {
    if (client_) jack_transport_locate(client_, 0);
}

bool JackTransport::isRolling() const {
    if (!client_) return false;
    return jack_transport_query(client_, nullptr) == JackTransportRolling;
}

// ---------------------------------------------------------------------------
// Tempo / time-signature setters
// ---------------------------------------------------------------------------

void JackTransport::setBpm(double bpm) {
    bpm_.store(bpm, std::memory_order_relaxed);
}

void JackTransport::setBeatsPerBar(int beats) {
    beatsPerBar_.store(beats, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Timebase callback
// ---------------------------------------------------------------------------

void JackTransport::timebaseCallback(jack_transport_state_t /*state*/,
                                     jack_nframes_t /*nframes*/,
                                     jack_position_t* pos,
                                     int /*new_pos*/,
                                     void* arg) {
    auto* self = static_cast<JackTransport*>(arg);
    self->fillBBT(pos);
}

void JackTransport::fillBBT(jack_position_t* pos) const {
    double bpm       = bpm_.load(std::memory_order_relaxed);
    int beatsPerBar  = beatsPerBar_.load(std::memory_order_relaxed);
    double sr        = sampleRate_;

    double framesPerBeat = (60.0 / bpm) * sr;
    double framesPerTick = framesPerBeat / kTicksPerBeat;

    // Absolute tick from the frame position
    double absTick = static_cast<double>(pos->frame) / framesPerTick;
    double absBeat = absTick / kTicksPerBeat;

    int bar  = static_cast<int>(absBeat / beatsPerBar);    // 0-based
    int beat = static_cast<int>(std::fmod(absBeat, static_cast<double>(beatsPerBar)));
    double tick = std::fmod(absTick, kTicksPerBeat);

    // JACK BBT is 1-indexed for bar and beat
    pos->valid          = JackPositionBBT;
    pos->bar            = bar + 1;
    pos->beat           = beat + 1;
    pos->tick           = static_cast<int32_t>(tick);
    pos->bar_start_tick = static_cast<double>(bar) * beatsPerBar * kTicksPerBeat;
    pos->beats_per_bar  = static_cast<float>(beatsPerBar);
    pos->beat_type      = 4.0f;   // quarter-note denominator
    pos->ticks_per_beat = kTicksPerBeat;
    pos->beats_per_minute = bpm;
}

} // namespace retrospect
