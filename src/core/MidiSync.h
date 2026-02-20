#pragma once

#include <cstdint>
#include <functional>

namespace retrospect {

/// Generates MIDI clock sync messages (24 PPQN) in sync with the metronome.
/// Uses a callback to send raw MIDI bytes, keeping the core library
/// independent of any MIDI framework.
class MidiSync {
public:
    /// Callback receives a single MIDI status byte (0xF8 clock, 0xFA start, etc.)
    using SendCallback = std::function<void(uint8_t statusByte)>;

    MidiSync(double bpm = 120.0, double sampleRate = 44100.0);

    /// Advance by numSamples, emitting clock ticks (0xF8) as needed
    void advance(int numSamples);

    /// Set BPM (recalculates tick interval, preserves fractional position)
    void setBpm(double bpm);

    /// Set sample rate
    void setSampleRate(double rate);

    /// Enable/disable MIDI sync output.
    /// When enabled, sends Start (0xFA) and begins clock ticks.
    /// When disabled, sends Stop (0xFC) and stops clock ticks.
    void setEnabled(bool on);
    bool isEnabled() const { return enabled_; }

    /// Set the callback for sending MIDI bytes
    void setSendCallback(SendCallback cb) { sendCallback_ = std::move(cb); }

    /// Whether a send callback is wired up (i.e. a MIDI output device is available)
    bool hasOutput() const { return sendCallback_ != nullptr; }

    // MIDI system real-time status bytes
    static constexpr uint8_t kClockTick = 0xF8;
    static constexpr uint8_t kStart     = 0xFA;
    static constexpr uint8_t kContinue  = 0xFB;
    static constexpr uint8_t kStop      = 0xFC;

    static constexpr int kPPQN = 24;  // Pulses per quarter note

private:
    void recalculate();
    void sendByte(uint8_t b);

    double bpm_;
    double sampleRate_;
    double samplesPerTick_ = 0;   // samples per MIDI clock tick
    double sampleInTick_ = 0.0;   // accumulator within current tick
    bool enabled_ = false;

    SendCallback sendCallback_;
};

} // namespace retrospect
