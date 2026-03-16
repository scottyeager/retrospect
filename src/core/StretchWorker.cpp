#include "core/StretchWorker.h"

#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace retrospect {

StretchWorker::StretchWorker() = default;

StretchWorker::~StretchWorker() {
    stop();
}

void StretchWorker::start(double sampleRate, FeedCallback feedCb) {
    if (running_.load(std::memory_order_relaxed)) return;

    feedCb_ = std::move(feedCb);

    // Allocate buffers
    buf_.assign(static_cast<size_t>(kBufCapacity), 0.0f);
    inputWork_.resize(static_cast<size_t>(kMaxStretchInput), 0.0f);
    outputWork_.resize(static_cast<size_t>(kStretchBlockSize), 0.0f);
    writePos_.store(0, std::memory_order_relaxed);
    readPos_.store(0, std::memory_order_relaxed);

    // Configure stretcher
    stretcher_ = std::make_unique<TimeStretcher>();
    stretcher_->configure(sampleRate);

    // Create self-pipe for wake signaling
    if (::pipe(wakeFd_) == 0) {
        ::fcntl(wakeFd_[0], F_SETFL, O_NONBLOCK);
        ::fcntl(wakeFd_[1], F_SETFL, O_NONBLOCK);
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { workerLoop(); });
}

void StretchWorker::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;

    running_.store(false, std::memory_order_release);

    // Wake the worker so it can observe the stop flag
    requestMore();

    if (thread_.joinable()) {
        thread_.join();
    }

    // Close pipe
    if (wakeFd_[0] >= 0) { ::close(wakeFd_[0]); wakeFd_[0] = -1; }
    if (wakeFd_[1] >= 0) { ::close(wakeFd_[1]); wakeFd_[1] = -1; }
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
    // Write a single byte to the wake pipe. Wait-free — if the pipe is full
    // the worker is already awake or will wake soon.
    if (wakeFd_[1] >= 0) {
        char c = 1;
        (void)::write(wakeFd_[1], &c, 1);
    }
}

void StretchWorker::workerLoop() {
    while (running_.load(std::memory_order_acquire)) {
        // Check if buffer needs filling
        int w = writePos_.load(std::memory_order_relaxed);
        int r = readPos_.load(std::memory_order_acquire);
        int avail = (w - r + kBufCapacity) % kBufCapacity;

        if (avail < kFillThreshold) {
            fillOnce();
            continue;  // Check again immediately
        }

        // Buffer is sufficiently full — wait for a wake signal
        if (wakeFd_[0] >= 0) {
            struct pollfd pfd;
            pfd.fd = wakeFd_[0];
            pfd.events = POLLIN;
            pfd.revents = 0;
            ::poll(&pfd, 1, 50);  // 50ms timeout as safety net

            // Drain the pipe
            char drain[64];
            while (::read(wakeFd_[0], drain, sizeof(drain)) > 0) {}
        }
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
