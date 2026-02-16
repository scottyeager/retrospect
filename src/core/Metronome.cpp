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

    double totalBeats = static_cast<double>(totalSamples_) / samplesPerBeat_;
    int64_t wholeBeat = static_cast<int64_t>(std::floor(totalBeats));

    pos.bar = static_cast<int>(wholeBeat / beatsPerBar_);
    pos.beat = static_cast<int>(wholeBeat % beatsPerBar_);
    pos.beatFraction = totalBeats - std::floor(totalBeats);

    return pos;
}

void Metronome::advance(int numSamples) {
    if (!running_ || numSamples <= 0) return;

    int64_t startSample = totalSamples_;
    int64_t endSample = totalSamples_ + numSamples;

    // Determine beat boundaries crossed during this advance
    auto beatAt = [this](int64_t sample) -> int64_t {
        return static_cast<int64_t>(std::floor(static_cast<double>(sample) / samplesPerBeat_));
    };

    int64_t startBeat = beatAt(startSample);
    int64_t endBeat = beatAt(endSample - 1); // -1 because endSample is exclusive

    // Check if we crossed any beat boundaries
    // A beat boundary is crossed if the beat number at the start sample differs
    // from the beat number at end-1, OR if we started exactly on a beat boundary
    for (int64_t b = startBeat + 1; b <= endBeat + 1; ++b) {
        int64_t boundarySample = static_cast<int64_t>(std::round(b * samplesPerBeat_));
        if (boundarySample > startSample && boundarySample <= endSample) {
            // We crossed beat 'b'
            // Temporarily set position to the boundary for the callback
            int64_t savedTotal = totalSamples_;
            totalSamples_ = boundarySample;
            MetronomePosition pos = position();

            if (beatCallback_) {
                beatCallback_(pos);
            }
            // Bar boundary: beat 0 of a bar
            if (pos.beat == 0 && barCallback_) {
                barCallback_(pos);
            }

            totalSamples_ = savedTotal;
        }
    }

    totalSamples_ = endSample;
}

void Metronome::reset() {
    totalSamples_ = 0;
}

int64_t Metronome::nextBeatSample() const {
    double totalBeats = static_cast<double>(totalSamples_) / samplesPerBeat_;
    int64_t nextBeat = static_cast<int64_t>(std::floor(totalBeats)) + 1;
    return static_cast<int64_t>(std::round(nextBeat * samplesPerBeat_));
}

int64_t Metronome::nextBarSample() const {
    double totalBars = static_cast<double>(totalSamples_) / samplesPerBar_;
    int64_t nextBar = static_cast<int64_t>(std::floor(totalBars)) + 1;
    return static_cast<int64_t>(std::round(nextBar * samplesPerBar_));
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
    bpm_ = std::max(1.0, std::min(bpm, 999.0));
    recalculate();
}

void Metronome::setBeatsPerBar(int beats) {
    beatsPerBar_ = std::max(1, std::min(beats, 16));
    recalculate();
}

void Metronome::setSampleRate(double rate) {
    sampleRate_ = rate;
    recalculate();
}

} // namespace retrospect
