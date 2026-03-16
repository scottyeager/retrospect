#pragma once

#include "core/TimeStretcher.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace retrospect {

/// Background worker that fills a lock-free ring buffer with time-stretched
/// audio. The audio thread only reads from the buffer; a dedicated worker
/// thread produces stretched samples, decoupling stretch computation from
/// the real-time audio deadline.
///
/// Communication is lock-free:
///   - readPos_  is written only by the audio thread
///   - writePos_ is written only by the worker thread
///   - A pipe-based eventfd/self-pipe wakes the worker without blocking
class StretchWorker {
public:
    /// Callback that reads raw loop samples for the stretcher.
    /// Fills `output` with `count` samples starting from the current raw
    /// read position, advancing it. Called only on the worker thread.
    using FeedCallback = std::function<void(float* output, int count)>;

    StretchWorker();
    ~StretchWorker();

    // Non-copyable, non-movable (owns a thread)
    StretchWorker(const StretchWorker&) = delete;
    StretchWorker& operator=(const StretchWorker&) = delete;
    StretchWorker(StretchWorker&&) = delete;
    StretchWorker& operator=(StretchWorker&&) = delete;

    /// Start the worker thread. Must call before any audio processing.
    /// @param sampleRate  Audio sample rate
    /// @param feedCb      Callback to read raw loop samples (called on worker thread)
    void start(double sampleRate, FeedCallback feedCb);

    /// Stop the worker thread and join. Safe to call if not started.
    void stop();

    /// Whether the worker is running
    bool isRunning() const { return running_.load(std::memory_order_relaxed); }

    /// Update the tempo ratio. Thread-safe (atomic).
    void setTempoRatio(double ratio);

    /// Read one sample from the stretch buffer. Returns 0 if buffer is empty.
    /// Called only from the audio thread.
    float readSample();

    /// Number of stretched samples available to read.
    /// Called only from the audio thread.
    int available() const;

    /// Reset the buffer state (e.g., on seek or mode transition).
    /// Must only be called when the worker is stopped or from the worker
    /// thread itself.
    void resetBuffer();

    /// Signal the worker to wake up and produce more samples.
    /// Called from the audio thread. This is a wait-free operation.
    void requestMore();

    static constexpr int kStretchBlockSize = 512;
    static constexpr int kBufCapacity = 16384;  // ~340ms at 48kHz
    static constexpr int kMaxStretchInput = kStretchBlockSize * 4;
    /// Worker fills when available drops below this threshold
    static constexpr int kFillThreshold = kBufCapacity / 2;

private:
    void workerLoop();
    void fillOnce();

    std::unique_ptr<TimeStretcher> stretcher_;
    FeedCallback feedCb_;

    // Lock-free ring buffer
    std::vector<float> buf_;
    alignas(64) std::atomic<int> writePos_{0};
    alignas(64) std::atomic<int> readPos_{0};

    std::atomic<double> tempoRatio_{1.0};
    std::atomic<bool> running_{false};
    std::atomic<bool> wakeRequested_{false};

    // Worker thread
    std::thread thread_;

    // Self-pipe for wake signaling (avoids spinning)
    int wakeFd_[2] = {-1, -1};

    // Pre-allocated work buffers (worker thread only)
    std::vector<float> inputWork_;
    std::vector<float> outputWork_;
};

} // namespace retrospect
