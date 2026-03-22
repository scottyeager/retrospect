#include "core/StretchWorker.h"

#include <algorithm>
#include <cmath>

namespace retrospect {

StretchWorker::StretchWorker(double sampleRate) {
    buf_.assign(static_cast<size_t>(kBufCapacity), 0.0f);
    inputWork_.resize(static_cast<size_t>(kMaxStretchInput), 0.0f);
    outputWork_.resize(static_cast<size_t>(kStretchBlockSize), 0.0f);

    stretcher_ = std::make_unique<TimeStretcher>();
    stretcher_->configure(sampleRate);

    thread_ = std::thread([this] { workerLoop(); });
}

StretchWorker::~StretchWorker() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void StretchWorker::start(FeedCallback feedCb) {
    std::lock_guard lock(mutex_);
    feedCb_ = std::move(feedCb);
    resetBuffer();
    active_.store(true, std::memory_order_release);
    cv_.notify_one();
}

void StretchWorker::stop() {
    std::lock_guard lock(mutex_);
    active_.store(false, std::memory_order_release);
    feedCb_ = nullptr;
}

void StretchWorker::setTempoRatio(double ratio) {
    tempoRatio_.store(ratio, std::memory_order_relaxed);
}

float StretchWorker::readSample() {
    int w = writePos_.load(std::memory_order_acquire);
    int r = readPos_.load(std::memory_order_relaxed);
    int avail = (w - r + kBufCapacity) % kBufCapacity;

    if (avail == 0) return 0.0f;

    float sample = buf_[static_cast<size_t>(r)];
    readPos_.store((r + 1) % kBufCapacity, std::memory_order_release);
    return sample;
}

int StretchWorker::available() const {
    int w = writePos_.load(std::memory_order_acquire);
    int r = readPos_.load(std::memory_order_relaxed);
    return (w - r + kBufCapacity) % kBufCapacity;
}

void StretchWorker::resetBuffer() {
    writePos_.store(0, std::memory_order_relaxed);
    readPos_.store(0, std::memory_order_relaxed);
    if (stretcher_) stretcher_->reset();
}

void StretchWorker::requestMore() {
    wakeRequested_.store(true, std::memory_order_release);
    cv_.notify_one();
}

bool StretchWorker::needsFill() const {
    int w = writePos_.load(std::memory_order_relaxed);
    int r = readPos_.load(std::memory_order_relaxed);
    int avail = (w - r + kBufCapacity) % kBufCapacity;
    return avail < kFillThreshold;
}

void StretchWorker::workerLoop() {
    std::unique_lock lock(mutex_);
    while (running_) {
        cv_.wait(lock, [this] {
            return !running_ ||
                   (active_.load(std::memory_order_relaxed) &&
                    (needsFill() || wakeRequested_.load(std::memory_order_relaxed)));
        });
        if (!running_) break;
        wakeRequested_.store(false, std::memory_order_relaxed);
        fillOnce();
    }
}

void StretchWorker::fillOnce() {
    if (!stretcher_ || !stretcher_->isConfigured() || !feedCb_) return;

    double ratio = tempoRatio_.load(std::memory_order_relaxed);

    // How many raw input samples to produce kStretchBlockSize output samples
    int inputNeeded = static_cast<int>(std::ceil(kStretchBlockSize * ratio));
    inputNeeded = std::clamp(inputNeeded, 1, kMaxStretchInput);

    // Check we have room in the buffer (leave 1 slot empty for SPSC protocol)
    int w = writePos_.load(std::memory_order_relaxed);
    int r = readPos_.load(std::memory_order_acquire);
    int space = kBufCapacity - 1 - ((w - r + kBufCapacity) % kBufCapacity);

    if (space < kStretchBlockSize) return;

    // Read raw samples via callback
    feedCb_(inputWork_.data(), inputNeeded);

    // Process through stretcher
    stretcher_->process(inputWork_.data(), inputNeeded,
                        outputWork_.data(), kStretchBlockSize);

    // Write to ring buffer
    for (int i = 0; i < kStretchBlockSize; ++i) {
        buf_[static_cast<size_t>((w + i) % kBufCapacity)] = outputWork_[static_cast<size_t>(i)];
    }

    // Publish: advance write position with release semantics so
    // the audio thread sees the data before the updated index
    writePos_.store((w + kStretchBlockSize) % kBufCapacity, std::memory_order_release);
}

} // namespace retrospect
