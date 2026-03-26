#include "JackAudioIO.h"
#include "core/LoopEngine.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace retrospect {

JackAudioIO::JackAudioIO(LoopEngine& engine, int numInputPorts, bool autoConnect)
    : engine_(engine)
    , numInputPorts_(numInputPorts)
    , autoConnect_(autoConnect)
{}

JackAudioIO::~JackAudioIO() {
    shutdown();
}

bool JackAudioIO::init() {
    if (client_) return true;

    jack_status_t status{};
    client_ = jack_client_open("Retrospect", JackNoStartServer, &status);
    if (!client_) {
        fprintf(stderr, "JackAudioIO: could not open JACK client (status 0x%x)\n",
                static_cast<unsigned>(status));
        return false;
    }

    sampleRate_ = static_cast<double>(jack_get_sample_rate(client_));
    bufferSize_ = static_cast<int>(jack_get_buffer_size(client_));

    // Register input ports
    inputPorts_.reserve(numInputPorts_);
    for (int i = 0; i < numInputPorts_; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "input_%d", i + 1);
        jack_port_t* port = jack_port_register(client_, name,
                                                JACK_DEFAULT_AUDIO_TYPE,
                                                JackPortIsInput, 0);
        if (!port) {
            fprintf(stderr, "JackAudioIO: failed to register input port %d\n", i + 1);
            jack_client_close(client_);
            client_ = nullptr;
            return false;
        }
        inputPorts_.push_back(port);
    }

    // Register output port
    outputPort_ = jack_port_register(client_, "output_1",
                                      JACK_DEFAULT_AUDIO_TYPE,
                                      JackPortIsOutput, 0);
    if (!outputPort_) {
        fprintf(stderr, "JackAudioIO: failed to register output port\n");
        jack_client_close(client_);
        client_ = nullptr;
        return false;
    }

    // Pre-allocate scratch buffer
    inputPtrs_.resize(numInputPorts_);

    // Set callbacks
    jack_set_process_callback(client_, processCallback, this);
    jack_on_shutdown(client_, shutdownCallback, this);

    // Register as unconditional timebase master
    int err = jack_set_timebase_callback(client_, /*conditional=*/0,
                                          timebaseCallback, this);
    if (err != 0) {
        fprintf(stderr, "JackAudioIO: warning: failed to become timebase master\n");
        // Non-fatal — audio still works without transport mastery
    }

    // Activate
    if (jack_activate(client_) != 0) {
        fprintf(stderr, "JackAudioIO: failed to activate JACK client\n");
        jack_client_close(client_);
        client_ = nullptr;
        return false;
    }

    fprintf(stderr, "JackAudioIO: active (%.0f Hz, %d buffer, %d inputs)\n",
            sampleRate_, bufferSize_, numInputPorts_);

    // Auto-connect
    if (autoConnect_) {
        // Connect inputs to system:capture_*
        const char** capturePorts = jack_get_ports(client_, "system:capture",
                                                    JACK_DEFAULT_AUDIO_TYPE,
                                                    JackPortIsOutput);
        if (capturePorts) {
            for (int i = 0; i < numInputPorts_ && capturePorts[i]; ++i) {
                const char* myPort = jack_port_name(inputPorts_[i]);
                int rc = jack_connect(client_, capturePorts[i], myPort);
                if (rc != 0 && rc != EEXIST) {
                    fprintf(stderr, "JackAudioIO: failed to connect %s → %s\n",
                            capturePorts[i], myPort);
                }
            }
            jack_free(capturePorts);
        }

        // Connect output to system:playback_1
        const char** playbackPorts = jack_get_ports(client_, "system:playback",
                                                     JACK_DEFAULT_AUDIO_TYPE,
                                                     JackPortIsInput);
        if (playbackPorts) {
            const char* myOutput = jack_port_name(outputPort_);
            if (playbackPorts[0]) {
                int rc = jack_connect(client_, myOutput, playbackPorts[0]);
                if (rc != 0 && rc != EEXIST) {
                    fprintf(stderr, "JackAudioIO: failed to connect %s → %s\n",
                            myOutput, playbackPorts[0]);
                }
            }
            jack_free(playbackPorts);
        }
    }

    return true;
}

void JackAudioIO::shutdown() {
    if (!client_) return;

    jack_release_timebase(client_);
    jack_deactivate(client_);
    jack_client_close(client_);
    client_ = nullptr;
}

// ---------------------------------------------------------------------------
// Transport control
// ---------------------------------------------------------------------------

void JackAudioIO::startTransport() {
    if (client_) jack_transport_start(client_);
}

void JackAudioIO::stopTransport() {
    if (client_) jack_transport_stop(client_);
}

void JackAudioIO::rewindTransport() {
    if (client_) jack_transport_locate(client_, 0);
}

bool JackAudioIO::isTransportRolling() const {
    if (!client_) return false;
    return jack_transport_query(client_, nullptr) == JackTransportRolling;
}

// ---------------------------------------------------------------------------
// Tempo / time-signature
// ---------------------------------------------------------------------------

void JackAudioIO::setBpm(double bpm) {
    bpm_.store(bpm, std::memory_order_relaxed);
}

void JackAudioIO::setBeatsPerBar(int beats) {
    beatsPerBar_.store(beats, std::memory_order_relaxed);
}

void JackAudioIO::updateMetronomePosition(int64_t totalSamples) {
    metronomeSamples_.store(totalSamples, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// JACK callbacks
// ---------------------------------------------------------------------------

int JackAudioIO::processCallback(jack_nframes_t nframes, void* arg) {
    auto* self = static_cast<JackAudioIO*>(arg);

    // Get input port buffers (zero-copy pointers into JACK shared memory)
    for (int i = 0; i < self->numInputPorts_; ++i) {
        self->inputPtrs_[i] = static_cast<const float*>(
            jack_port_get_buffer(self->inputPorts_[i], nframes));
    }

    // Get output buffer and clear it
    float* output = static_cast<float*>(
        jack_port_get_buffer(self->outputPort_, nframes));
    std::memset(output, 0, sizeof(float) * nframes);

    // Drive the engine
    self->engine_.processBlock(self->inputPtrs_.data(), self->numInputPorts_,
                               output, static_cast<int>(nframes));

    return 0;
}

void JackAudioIO::shutdownCallback(void* arg) {
    auto* self = static_cast<JackAudioIO*>(arg);
    self->shutdown_.store(true, std::memory_order_relaxed);
    fprintf(stderr, "JackAudioIO: JACK server shut down\n");
}

void JackAudioIO::timebaseCallback(jack_transport_state_t /*state*/,
                                    jack_nframes_t /*nframes*/,
                                    jack_position_t* pos,
                                    int /*new_pos*/,
                                    void* arg) {
    auto* self = static_cast<JackAudioIO*>(arg);
    self->fillBBT(pos);
}

void JackAudioIO::fillBBT(jack_position_t* pos) const {
    double bpmVal     = bpm_.load(std::memory_order_relaxed);
    int beatsPerBar   = beatsPerBar_.load(std::memory_order_relaxed);
    double sr         = sampleRate_;

    double framesPerBeat = (60.0 / bpmVal) * sr;
    double framesPerTick = framesPerBeat / kTicksPerBeat;

    int64_t mSamples = metronomeSamples_.load(std::memory_order_relaxed);
    double absTick = static_cast<double>(mSamples) / framesPerTick;
    double absBeat = absTick / kTicksPerBeat;

    int bar  = static_cast<int>(absBeat / beatsPerBar);
    int beat = static_cast<int>(std::fmod(absBeat, static_cast<double>(beatsPerBar)));
    double tick = std::fmod(absTick, kTicksPerBeat);

    // JACK BBT is 1-indexed for bar and beat
    pos->valid          = JackPositionBBT;
    pos->bar            = bar + 1;
    pos->beat           = beat + 1;
    pos->tick           = static_cast<int32_t>(tick);
    pos->bar_start_tick = static_cast<double>(bar) * beatsPerBar * kTicksPerBeat;
    pos->beats_per_bar  = static_cast<float>(beatsPerBar);
    pos->beat_type      = 4.0f;
    pos->ticks_per_beat = kTicksPerBeat;
    pos->beats_per_minute = bpmVal;
}

} // namespace retrospect
