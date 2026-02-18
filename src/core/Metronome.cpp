#include "core/Metronome.h"
#include <cmath>
#include <algorithm>

namespace retrospect {

Metronome::Metronome(double bpm, int beatsPerBar, double sampleRate)
    : bpm_(bpm), beatsPerBar_(beatsPerBar), sampleRate_(sampleRate)
{
    recalculate();
}

void Metronome::recalculate() {
    samplesPerBeat_ = (60.0 / bpm_) * sampleRate_;
    samplesPerBar_ = samplesPerBeat_ * beatsPerBar_;
}

MetronomePosition Metronome::position() const {
    MetronomePosition pos;
    pos.totalSamples = totalSamples_;
    pos.bar = currentBar_;
    pos.beat = currentBeat_;
    pos.beatFraction = sampleInBeat_ / samplesPerBeat_;
    return pos;
}

void Metronome::advance(int numSamples) {
    if (!running_ || numSamples <= 0) return;

    for (int i = 0; i < numSamples; ++i) {
        totalSamples_++;
        sampleInBeat_ += 1.0;

        if (sampleInBeat_ >= samplesPerBeat_) {
            sampleInBeat_ -= samplesPerBeat_;
            currentBeat_++;
            if (currentBeat_ >= beatsPerBar_) {
                currentBeat_ = 0;
                currentBar_++;
            }

            MetronomePosition pos = position();
            if (beatCallback_) beatCallback_(pos);
            if (pos.beat == 0 && barCallback_) barCallback_(pos);
        }
    }
}

void Metronome::reset() {
    totalSamples_ = 0;
    currentBar_ = 0;
    currentBeat_ = 0;
    sampleInBeat_ = 0.0;
}

int64_t Metronome::nextBeatSample() const {
    double samplesUntilNext = samplesPerBeat_ - sampleInBeat_;
    return totalSamples_ + static_cast<int64_t>(std::round(samplesUntilNext));
}

int64_t Metronome::nextBarSample() const {
    int beatsLeft = beatsPerBar_ - currentBeat_;
    double samplesUntilBar = beatsLeft * samplesPerBeat_ - sampleInBeat_;
    return totalSamples_ + static_cast<int64_t>(std::round(samplesUntilBar));
}

int64_t Metronome::samplesUntilBoundary(Quantize q) const {
    switch (q) {
        case Quantize::Free:
            return 0;
        case Quantize::Beat:
            return nextBeatSample() - totalSamples_;
        case Quantize::Bar:
            return nextBarSample() - totalSamples_;
    }
    return 0;
}

double Metronome::samplesPerBeat() const {
    return samplesPerBeat_;
}

double Metronome::samplesPerBar() const {
    return samplesPerBar_;
}

void Metronome::setBpm(double bpm) {
    // Preserve fractional position within the current beat
    double fraction = (samplesPerBeat_ > 0) ? sampleInBeat_ / samplesPerBeat_ : 0.0;

    bpm_ = std::max(1.0, std::min(bpm, 999.0));
    recalculate();

    // Restore the same fractional position at the new tempo
    sampleInBeat_ = fraction * samplesPerBeat_;
}

void Metronome::setBeatsPerBar(int beats) {
    beatsPerBar_ = std::max(1, std::min(beats, 16));
    recalculate();
    if (currentBeat_ >= beatsPerBar_) {
        currentBeat_ = 0;
        currentBar_++;
    }
}

void Metronome::setSampleRate(double rate) {
    double fraction = (samplesPerBeat_ > 0) ? sampleInBeat_ / samplesPerBeat_ : 0.0;
    sampleRate_ = rate;
    recalculate();
    sampleInBeat_ = fraction * samplesPerBeat_;
}

} // namespace retrospect
