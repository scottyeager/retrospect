#pragma once

#include "client/EngineClient.h"
#include <lo/lo.h>

#include <string>
#include <chrono>

namespace retrospect {

/// OSC-based EngineClient that communicates with a remote OscServer.
/// Commands are sent as OSC messages; state is received via pushed updates.
/// Uses a non-threaded lo_server — all receiving happens in poll().
class OscEngineClient : public EngineClient {
public:
    /// Connect to an OSC server at host:port
    OscEngineClient(const std::string& host, const std::string& port);
    ~OscEngineClient() override;

    /// Whether the client successfully initialized
    bool isValid() const { return server_ != nullptr && serverAddr_ != nullptr; }

    // Commands
    void scheduleCaptureLoop(int loopIndex, Quantize quantize,
                             int lookbackBars = 0) override;
    void scheduleRecord(int loopIndex, Quantize quantize) override;
    void scheduleStopRecord(int loopIndex, Quantize quantize) override;
    void scheduleOp(OpType type, int loopIndex, Quantize quantize) override;
    void scheduleSetSpeed(int loopIndex, double speed,
                          Quantize quantize) override;
    void executeOpNow(OpType type, int loopIndex) override;
    void cancelPending() override;

    // Settings
    void setDefaultQuantize(Quantize q) override;
    void setLookbackBars(int bars) override;
    void setMetronomeClickEnabled(bool on) override;
    void setMidiSyncEnabled(bool on) override;
    void setBpm(double bpm) override;

    // State
    const EngineSnapshot& snapshot() const override { return snap_; }
    void poll() override;

private:
    void subscribe();

    // OSC state handlers (static trampolines)
    static int handleMetronome(const char* path, const char* types,
                               lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleLoop(const char* path, const char* types,
                          lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleRecording(const char* path, const char* types,
                               lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleSettings(const char* path, const char* types,
                              lo_arg** argv, int argc, lo_message msg, void* user);
    static int handlePendingClear(const char* path, const char* types,
                                  lo_arg** argv, int argc, lo_message msg, void* user);
    static int handlePendingOp(const char* path, const char* types,
                               lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleLog(const char* path, const char* types,
                         lo_arg** argv, int argc, lo_message msg, void* user);
    static void errorHandler(int num, const char* msg, const char* path);

    lo_server server_ = nullptr;       // Non-threaded receiver
    lo_address serverAddr_ = nullptr;  // Server we send commands to
    std::string host_;
    std::string port_;
    int localPort_ = 0;

    EngineSnapshot snap_;

    // Heartbeat/subscribe timer
    std::chrono::steady_clock::time_point lastSubscribe_;
    static constexpr double kHeartbeatIntervalSec = 10.0;
};

} // namespace retrospect
