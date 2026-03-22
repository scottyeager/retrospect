#pragma once

#include "core/TimeStretcher.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace retrospect {

/// Background worker that fills a lock-free ring buffer with time-stretched
/// audio. The audio thread only reads from the buffer; a dedicated worker
/// thread produces stretched samples, decoupling stretch computation from
/// the real-time audio deadline.
///
/// Lifecycle: constructed once per Loop (pre-allocates buffers and starts
/// the worker thread). The thread blocks on a condition variable until
/// activate() is called. deactivate() pauses production; the thread stays
/// alive until destruction.
///
/// Communication:
///   - readPos_  is written only by the audio thread
///   - writePos_ is written only by the worker thread
///   - A condition variable wakes the worker when more samples are needed
class StretchWorker {
public:
    /// Callback that reads raw loop samples for the stretcher.
    /// Fills `output` with `count` samples starting from the current raw
    /// read position, advancing it. Called only on the worker thread.
    using FeedCallback = std::function<void(float* output, int count)>;

    /// Pre-allocates all buffers, configures the stretcher, and starts
    /// the worker thread (which blocks immediately until activated).
    explicit StretchWorker(double sampleRate);
    ~StretchWorker();

    // Non-copyable, non-movable (owns a thread)
    StretchWorker(const StretchWorker&) = delete;
    StretchWorker& operator=(const StretchWorker&) = delete;
    StretchWorker(StretchWorker&&) = delete;
    StretchWorker& operator=(StretchWorker&&) = delete;

    /// Activate production with the given feed callback. Resets the
    /// stretch buffer and wakes the worker thread.
    void start(FeedCallback feedCb);

    /// Deactivate production. Blocks until any in-progress fill completes,
    /// then clears the feed callback. The worker thread remains alive.
    void stop();

    /// Whether the worker is actively producing stretched audio.
    bool isActive() const { return active_.load(std::memory_order_relaxed); }

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
    bool needsFill() const;

    std::unique_ptr<TimeStretcher> stretcher_;
    FeedCallback feedCb_;

    // Lock-free ring buffer
    std::vector<float> buf_;
    alignas(64) std::atomic<int> writePos_{0};
    alignas(64) std::atomic<int> readPos_{0};

    std::atomic<double> tempoRatio_{1.0};
    std::atomic<bool> active_{false};
    std::atomic<bool> wakeRequested_{false};

    // Worker thread synchronization
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = true;  // protected by mutex_
    std::thread thread_;

    // Pre-allocated work buffers (worker thread only)
    std::vector<float> inputWork_;
    std::vector<float> outputWork_;
};

} // namespace retrospect
