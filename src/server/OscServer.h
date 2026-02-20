#pragma once

#include "core/LoopEngine.h"
#include "client/EngineClient.h"
#include <lo/lo.h>

#include <vector>
#include <mutex>
#include <string>
#include <chrono>

namespace retrospect {

/// A subscribed OSC client that receives state pushes
struct OscSubscriber {
    lo_address addr = nullptr;
    std::chrono::steady_clock::time_point lastSeen;
};

/// OSC server that wraps a LoopEngine, receives commands via OSC,
/// and pushes state to subscribed clients at ~30Hz.
class OscServer {
public:
    OscServer(LoopEngine& engine, const std::string& port = "7770");
    ~OscServer();

    /// Start the OSC listener thread
    bool start();

    /// Stop the OSC listener thread
    void stop();

    /// Push current engine state to all subscribed clients.
    /// Call this from the main loop at ~30Hz.
    void pushState();

    /// Get the port the server is listening on
    std::string port() const { return port_; }

private:
    // OSC handler callbacks (static trampolines)
    static int handleCaptureLoop(const char* path, const char* types,
                                 lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleRecord(const char* path, const char* types,
                            lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleStopRecord(const char* path, const char* types,
                                lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleMute(const char* path, const char* types,
                          lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleUnmute(const char* path, const char* types,
                            lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleToggleMute(const char* path, const char* types,
                                lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleReverse(const char* path, const char* types,
                             lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleOverdubStart(const char* path, const char* types,
                                  lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleOverdubStop(const char* path, const char* types,
                                 lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleUndo(const char* path, const char* types,
                          lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleRedo(const char* path, const char* types,
                          lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleSpeed(const char* path, const char* types,
                           lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleClear(const char* path, const char* types,
                           lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleBpm(const char* path, const char* types,
                         lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleClick(const char* path, const char* types,
                           lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleMidiSync(const char* path, const char* types,
                              lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleQuantize(const char* path, const char* types,
                              lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleLookbackBars(const char* path, const char* types,
                                  lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleCancelPending(const char* path, const char* types,
                                   lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleSubscribe(const char* path, const char* types,
                               lo_arg** argv, int argc, lo_message msg, void* user);
    static int handleUnsubscribe(const char* path, const char* types,
                                 lo_arg** argv, int argc, lo_message msg, void* user);
    static void errorHandler(int num, const char* msg, const char* path);

    /// Schedule a simple 2-arg op (loopIdx, quantize)
    void handleSimpleOp(OpType type, lo_arg** argv);

    /// Add or refresh a subscriber
    void addSubscriber(const char* url, int port);

    /// Remove a subscriber
    void removeSubscriber(const char* url, int port);

    /// Prune subscribers that haven't been seen recently
    void pruneSubscribers();

    /// Send state to a single subscriber
    void pushStateTo(lo_address addr);

    LoopEngine& engine_;
    std::string port_;
    lo_server_thread serverThread_ = nullptr;

    std::mutex subMutex_;
    std::vector<OscSubscriber> subscribers_;
    static constexpr double kSubscriberTimeoutSec = 30.0;

    // Message buffer for pushing log messages
    std::mutex msgMutex_;
    std::vector<std::string> pendingMessages_;
};

} // namespace retrospect
