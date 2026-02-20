#include "core/MidiSync.h"
#include <algorithm>

namespace retrospect {

MidiSync::MidiSync(double bpm, double sampleRate)
    : bpm_(bpm), sampleRate_(sampleRate)
{
    recalculate();
}

void MidiSync::recalculate() {
    // 24 ticks per quarter note
    double samplesPerBeat = (60.0 / bpm_) * sampleRate_;
    samplesPerTick_ = samplesPerBeat / kPPQN;
}

void MidiSync::advance(int numSamples) {
    if (!enabled_ || numSamples <= 0) return;

    for (int i = 0; i < numSamples; ++i) {
        sampleInTick_ += 1.0;

        if (sampleInTick_ >= samplesPerTick_) {
            sampleInTick_ -= samplesPerTick_;
            sendByte(kClockTick);
        }
    }
}

void MidiSync::setBpm(double bpm) {
    double fraction = (samplesPerTick_ > 0) ? sampleInTick_ / samplesPerTick_ : 0.0;
    bpm_ = std::max(1.0, std::min(bpm, 999.0));
    recalculate();
    sampleInTick_ = fraction * samplesPerTick_;
}

void MidiSync::setSampleRate(double rate) {
    double fraction = (samplesPerTick_ > 0) ? sampleInTick_ / samplesPerTick_ : 0.0;
    sampleRate_ = rate;
    recalculate();
    sampleInTick_ = fraction * samplesPerTick_;
}

void MidiSync::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    if (on) {
        sampleInTick_ = 0.0;
        sendByte(kStart);
    } else {
        sendByte(kStop);
    }
}

void MidiSync::sendByte(uint8_t b) {
    if (sendCallback_) sendCallback_(b);
}

} // namespace retrospect
