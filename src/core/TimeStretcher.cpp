#include "core/TimeStretcher.h"
#include "signalsmith-stretch.h"

namespace retrospect {

struct TimeStretcher::Impl {
    signalsmith::stretch::SignalsmithStretch<float> stretch;
};

TimeStretcher::TimeStretcher() : impl_(std::make_unique<Impl>()) {}
TimeStretcher::~TimeStretcher() = default;
TimeStretcher::TimeStretcher(TimeStretcher&&) noexcept = default;
TimeStretcher& TimeStretcher::operator=(TimeStretcher&&) noexcept = default;

void TimeStretcher::configure(double sampleRate) {
    impl_->stretch.presetCheaper(1, static_cast<float>(sampleRate));
    configured_ = true;
}

void TimeStretcher::process(const float* input, int inputSamples,
                            float* output, int outputSamples) {
    // Signalsmith API takes float** (one pointer per channel)
    float* inPtr = const_cast<float*>(input);
    float* inputPtrs[1] = {inPtr};
    float* outputPtrs[1] = {output};
    impl_->stretch.process(inputPtrs, inputSamples, outputPtrs, outputSamples);
}

void TimeStretcher::reset() {
    impl_->stretch.reset();
}

} // namespace retrospect
