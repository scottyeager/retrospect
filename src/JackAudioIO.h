#pragma once

#include <jack/jack.h>
#include <jack/transport.h>
#include <atomic>
#include <cstdint>
#include <vector>

namespace retrospect {

class LoopEngine;

/// Direct JACK audio I/O and transport master.
///
/// Bypasses JUCE for audio when using the JACK backend, giving full control
/// over port count, naming, and connection management.  Also acts as the
/// JACK timebase master (absorbing the old JackTransport functionality).
class JackAudioIO {
public:
    /// @param engine       The LoopEngine to drive from the JACK process callback.
    /// @param numInputPorts Number of JACK input ports to register.
    /// @param autoConnect   Whether to auto-connect to system:capture/playback ports.
    JackAudioIO(LoopEngine& engine, int numInputPorts, bool autoConnect);
    ~JackAudioIO();

    // Non-copyable / non-movable
    JackAudioIO(const JackAudioIO&) = delete;
    JackAudioIO& operator=(const JackAudioIO&) = delete;

    /// Open the JACK client, register ports, activate, and optionally connect.
    /// Returns true on success.
    bool init();

    /// Deactivate and close the JACK client.
    void shutdown();

    // --- Audio parameters (valid after init) ---

    double sampleRate() const { return sampleRate_; }
    int bufferSize() const { return bufferSize_; }
    int inputChannelCount() const { return numInputPorts_; }

    // --- Transport control ---

    void startTransport();
    void stopTransport();
    void rewindTransport();
    bool isTransportRolling() const;

    // --- Tempo / time-signature ---

    void setBpm(double bpm);
    double bpm() const { return bpm_.load(std::memory_order_relaxed); }

    void setBeatsPerBar(int beats);
    int beatsPerBar() const { return beatsPerBar_.load(std::memory_order_relaxed); }

    /// Update with the metronome's current total sample count.
    /// Called from the audio thread each processBlock.
    void updateMetronomePosition(int64_t totalSamples);

    /// True if JACK has signalled a shutdown (server disconnect, etc.).
    /// Polled by the main loop to trigger clean exit.
    bool isShutdown() const { return shutdown_.load(std::memory_order_relaxed); }

    static constexpr double kTicksPerBeat = 1920.0;

private:
    // JACK callbacks (static trampolines)
    static int processCallback(jack_nframes_t nframes, void* arg);
    static void shutdownCallback(void* arg);
    static void timebaseCallback(jack_transport_state_t state,
                                 jack_nframes_t nframes,
                                 jack_position_t* pos,
                                 int new_pos,
                                 void* arg);

    /// Compute BBT fields from the metronome sample count.
    void fillBBT(jack_position_t* pos) const;

    LoopEngine& engine_;
    int numInputPorts_;
    bool autoConnect_;

    jack_client_t* client_ = nullptr;

    std::vector<jack_port_t*> inputPorts_;
    jack_port_t* outputPort_ = nullptr;

    /// Scratch buffer for input channel pointers (filled each process cycle).
    std::vector<const float*> inputPtrs_;

    double sampleRate_ = 0.0;
    int bufferSize_ = 0;

    std::atomic<bool> shutdown_{false};

    // Timebase state
    std::atomic<double> bpm_{120.0};
    std::atomic<int> beatsPerBar_{4};
    std::atomic<int64_t> metronomeSamples_{0};
};

} // namespace retrospect
