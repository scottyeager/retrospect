#pragma once

#include <jack/jack.h>
#include <jack/transport.h>
#include <atomic>
#include <cstdint>

namespace retrospect {

/// JACK transport master — broadcasts BBT position and tempo so that other
/// JACK clients can follow this application's timeline.
///
/// Opens its own lightweight JACK client (no audio ports) and registers as
/// the unconditional timebase master.  BPM and time-signature changes are
/// propagated through atomic variables so the timebase callback (called from
/// the JACK process thread) always sees a consistent snapshot.
class JackTransport {
public:
    explicit JackTransport(double sampleRate);
    ~JackTransport();

    // Non-copyable / non-movable
    JackTransport(const JackTransport&) = delete;
    JackTransport& operator=(const JackTransport&) = delete;

    /// Open the JACK client and register as timebase master.
    /// Returns true on success.
    bool init();

    /// Deactivate and close the JACK client.
    void shutdown();

    /// Whether the JACK client is connected and active.
    bool isActive() const { return client_ != nullptr && active_; }

    // --- Transport control ---

    /// Start JACK transport rolling (from frame 0 if not already running).
    void start();

    /// Stop JACK transport.
    void stop();

    /// Relocate transport to frame 0.
    void rewind();

    /// Whether JACK transport is currently rolling.
    bool isRolling() const;

    // --- Tempo / time-signature ---

    void setBpm(double bpm);
    double bpm() const { return bpm_.load(std::memory_order_relaxed); }

    void setBeatsPerBar(int beats);
    int beatsPerBar() const { return beatsPerBar_.load(std::memory_order_relaxed); }

    static constexpr double kTicksPerBeat = 1920.0;

private:
    /// JACK timebase callback (static trampoline → member).
    static void timebaseCallback(jack_transport_state_t state,
                                 jack_nframes_t nframes,
                                 jack_position_t* pos,
                                 int new_pos,
                                 void* arg);

    /// Compute BBT fields in *pos from its frame field.
    void fillBBT(jack_position_t* pos) const;

    jack_client_t* client_ = nullptr;
    bool active_ = false;
    double sampleRate_;

    std::atomic<double> bpm_{120.0};
    std::atomic<int> beatsPerBar_{4};
};

} // namespace retrospect
