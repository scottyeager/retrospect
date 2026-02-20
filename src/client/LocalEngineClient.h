#pragma once

#include "client/EngineClient.h"
#include "core/LoopEngine.h"

#include <mutex>
#include <vector>
#include <string>

namespace retrospect {

/// In-process EngineClient that wraps a LoopEngine& directly.
/// Used in standalone mode and server+TUI mode.
class LocalEngineClient : public EngineClient {
public:
    explicit LocalEngineClient(LoopEngine& engine);

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
    int setLookbackBars(int bars) override;
    void setMetronomeClickEnabled(bool on) override;
    void setMidiSyncEnabled(bool on) override;
    void setBpm(double bpm) override;

    // State
    const EngineSnapshot& snapshot() const override { return snap_; }
    void poll() override;

private:
    LoopEngine& engine_;
    EngineSnapshot snap_;

    // Message buffer (filled from engine callback, drained in poll)
    std::mutex msgMutex_;
    std::vector<std::string> pendingMessages_;
};

} // namespace retrospect
