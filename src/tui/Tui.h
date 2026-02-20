#pragma once

#include "client/EngineClient.h"
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <chrono>

namespace retrospect {

/// Basic ncurses TUI for testing the loop engine.
/// Displays loop states, metronome position, and accepts keyboard input.
class Tui {
public:
    explicit Tui(EngineClient& client);
    ~Tui();

    /// Initialize ncurses
    bool init();

    /// Shut down ncurses
    void shutdown();

    /// Process one frame of the TUI: handle input, redraw.
    /// Returns false if the user wants to quit.
    bool update();

    /// Add a message to the log
    void addMessage(const std::string& msg);

    /// Set the selected loop index
    void setSelectedLoop(int index);
    int selectedLoop() const { return selectedLoop_; }

private:
    void draw();
    void drawHeader(int row);
    void drawMetronome(int row);
    void drawLoops(int startRow);
    void drawPendingOps(int startRow);
    void drawControls(int startRow);
    void drawMessages(int startRow);
    void handleKey(int key);
    void handleTapTempo();

    EngineClient& client_;
    int selectedLoop_ = 0;
    bool initialized_ = false;
    bool needsRedraw_ = true;

    std::mutex messageMutex_;
    std::deque<std::string> messages_;
    static constexpr int maxMessages_ = 8;

    int termWidth_ = 80;
    int termHeight_ = 24;

    // Tap tempo state
    std::vector<std::chrono::steady_clock::time_point> tapTimes_;
    static constexpr int maxTaps_ = 8;
    static constexpr double tapTimeoutSec_ = 2.0;
};

} // namespace retrospect
