#include "tui/Tui.h"
#include "core/LoopEngine.h"  // For OpType
#include <ncurses.h>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace retrospect {

Tui::Tui(EngineClient& client)
    : client_(client)
{
}

Tui::~Tui() {
    shutdown();
}

bool Tui::init() {
    initscr();
    if (!stdscr) return false;

    cbreak();             // Disable line buffering
    noecho();             // Don't echo input
    keypad(stdscr, TRUE); // Enable special keys
    nodelay(stdscr, TRUE); // Non-blocking input
    curs_set(0);          // Hide cursor

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN, -1);    // Playing
        init_pair(2, COLOR_YELLOW, -1);   // Muted
        init_pair(3, COLOR_RED, -1);      // Recording
        init_pair(4, COLOR_CYAN, -1);     // Selected
        init_pair(5, COLOR_WHITE, -1);    // Default
        init_pair(6, COLOR_MAGENTA, -1);  // Pending ops
        init_pair(7, COLOR_BLUE, -1);     // Header
    }

    getmaxyx(stdscr, termHeight_, termWidth_);
    initialized_ = true;
    needsRedraw_ = true;
    return true;
}

void Tui::shutdown() {
    if (initialized_) {
        endwin();
        initialized_ = false;
    }
}

bool Tui::update() {
    if (!initialized_) return false;

    // Poll client for latest state
    client_.poll();

    // Drain messages from client into our log
    for (const auto& msg : client_.snapshot().messages) {
        addMessage(msg);
    }

    // Handle resize
    int h, w;
    getmaxyx(stdscr, h, w);
    if (h != termHeight_ || w != termWidth_) {
        termHeight_ = h;
        termWidth_ = w;
        needsRedraw_ = true;
    }

    // Process all available input
    int key;
    while ((key = getch()) != ERR) {
        if (key == 'q' || key == 'Q') {
            return false;
        }
        handleKey(key);
        needsRedraw_ = true;
    }

    // Redraw
    draw();

    return true;
}

void Tui::draw() {
    const auto& snap = client_.snapshot();

    erase();

    int row = 0;
    drawHeader(row);
    row += 2;

    drawMetronome(row);
    row += 3;

    // Only show input channel section when live detection is active
    if (snap.liveThreshold > 0.0f && !snap.inputChannels.empty()) {
        drawInputChannels(row);
        row += 2;
    }

    drawLoops(row);
    row += snap.maxLoops + 2;

    drawPendingOps(row);
    row += static_cast<int>(std::min(snap.pendingOps.size(), size_t(3))) + 2;

    drawControls(row);
    row += 7;

    drawMessages(row);

    refresh();
    needsRedraw_ = false;
}

void Tui::drawHeader(int row) {
    const auto& snap = client_.snapshot();

    attron(A_BOLD | COLOR_PAIR(7));
    mvprintw(row, 0, "RETROSPECT");
    attroff(A_BOLD | COLOR_PAIR(7));
    mvprintw(row, 12, "v0.1.0 - Live Audio Looper");

    // Engine status
    std::ostringstream status;
    status << "Loops: " << snap.activeLoopCount << "/" << snap.maxLoops
           << "  SR: " << static_cast<int>(snap.sampleRate) << "Hz";
    mvprintw(row + 1, 0, "%s", status.str().c_str());
}

void Tui::drawMetronome(int row) {
    const auto& snap = client_.snapshot();
    const auto& met = snap.metronome;

    attron(A_BOLD);
    mvprintw(row, 0, "METRONOME");
    attroff(A_BOLD);

    std::ostringstream info;
    info << std::fixed << std::setprecision(1)
         << met.bpm << " BPM  "
         << met.beatsPerBar << "/4  "
         << (met.running ? "RUNNING" : "STOPPED");
    mvprintw(row, 12, "%s", info.str().c_str());

    // Beat visualization: show current bar.beat position
    std::ostringstream posStr;
    posStr << "Bar " << (met.bar + 1) << "  Beat " << (met.beat + 1);
    mvprintw(row + 1, 2, "%s", posStr.str().c_str());

    // Beat indicator: visual beat display
    std::string beatVis = "  ";
    for (int b = 0; b < met.beatsPerBar; ++b) {
        if (b == met.beat) {
            beatVis += "[X] ";
        } else {
            beatVis += "[ ] ";
        }
    }
    mvprintw(row + 1, 24, "%s", beatVis.c_str());

    // Quantize mode
    std::string qmode;
    switch (snap.defaultQuantize) {
        case Quantize::Free: qmode = "FREE"; break;
        case Quantize::Beat: qmode = "BEAT"; break;
        case Quantize::Bar:  qmode = "BAR"; break;
    }
    std::string midiStr = snap.midiOutputAvailable
        ? (snap.midiSyncEnabled ? "ON" : "OFF")
        : "N/A";
    int settingsLen = snprintf(nullptr, 0,
        "Quantize: %s  Lookback: %d bar(s)  Click: %s  MIDI: %s",
        qmode.c_str(), snap.lookbackBars,
        snap.clickEnabled ? "ON" : "OFF",
        midiStr.c_str());
    mvprintw(row + 2, 2, "Quantize: %s  Lookback: %d bar(s)  Click: %s  MIDI: %s",
             qmode.c_str(), snap.lookbackBars,
             snap.clickEnabled ? "ON" : "OFF",
             midiStr.c_str());

    // Recording indicator — placed after the settings text
    if (snap.isRecording) {
        attron(COLOR_PAIR(3) | A_BOLD);
        mvprintw(row + 2, 2 + settingsLen + 2, "** REC Loop %d **", snap.recordingLoopIndex);
        attroff(COLOR_PAIR(3) | A_BOLD);
    }
}

void Tui::drawInputChannels(int startRow) {
    const auto& snap = client_.snapshot();

    attron(A_BOLD);
    mvprintw(startRow, 0, "INPUT");
    attroff(A_BOLD);

    int col = 8;
    for (size_t ch = 0; ch < snap.inputChannels.size(); ++ch) {
        const auto& ic = snap.inputChannels[ch];

        // Channel label
        mvprintw(startRow, col, "%d:", static_cast<int>(ch + 1));
        col += 2;

        // Live indicator with color
        if (ic.live) {
            attron(COLOR_PAIR(1) | A_BOLD);  // Green bold = live
            mvprintw(startRow, col, "##");
            attroff(COLOR_PAIR(1) | A_BOLD);
        } else {
            attron(A_DIM);
            mvprintw(startRow, col, "..");
            attroff(A_DIM);
        }
        col += 3;
    }

    // Show threshold value
    mvprintw(startRow + 1, 8, "threshold: %.4f", snap.liveThreshold);
}

void Tui::drawLoops(int startRow) {
    const auto& snap = client_.snapshot();

    attron(A_BOLD);
    mvprintw(startRow, 0, "LOOPS");
    attroff(A_BOLD);

    mvprintw(startRow, 8, "# State      Bars   Layers Spd   Pos");

    for (int i = 0; i < snap.maxLoops; ++i) {
        int row = startRow + 1 + i;
        const auto& lp = snap.loops[static_cast<size_t>(i)];

        // Selection indicator
        if (i == selectedLoop_) {
            attron(COLOR_PAIR(4) | A_BOLD);
            mvprintw(row, 0, "> ");
            attroff(COLOR_PAIR(4) | A_BOLD);
        } else {
            mvprintw(row, 0, "  ");
        }

        // Loop number
        mvprintw(row, 2, "%d ", i + 1);

        // State with color
        std::string stateStr;
        int colorPair = 5;

        // Check if this loop is being classic-recorded by the engine
        bool classicRec = snap.isRecording && snap.recordingLoopIndex == i;

        if (classicRec) {
            stateStr = "REC...   ";
            colorPair = 3;
        } else {
            switch (lp.state) {
                case LoopState::Empty:
                    stateStr = "---      ";
                    break;
                case LoopState::Playing:
                    stateStr = "PLAYING  ";
                    colorPair = 1;
                    break;
                case LoopState::Muted:
                    stateStr = "MUTED    ";
                    colorPair = 2;
                    break;
                case LoopState::Recording:
                    stateStr = "OVERDUB  ";
                    colorPair = 3;
                    break;
            }
        }

        attron(COLOR_PAIR(colorPair));
        mvprintw(row, 4, "%-9s", stateStr.c_str());
        attroff(COLOR_PAIR(colorPair));

        if (!lp.isEmpty()) {
            // Bars
            mvprintw(row, 14, "%5.1f", lp.lengthInBars);

            // Layers
            mvprintw(row, 21, "%d/%d", lp.activeLayers, lp.layers);

            // Speed
            std::string spdStr;
            if (lp.speed == 1.0) spdStr = "1x";
            else if (lp.speed == 0.5) spdStr = "1/2x";
            else if (lp.speed == 2.0) spdStr = "2x";
            else {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(2) << lp.speed << "x";
                spdStr = ss.str();
            }
            mvprintw(row, 27, "%-5s", spdStr.c_str());

            // Reverse indicator
            if (lp.reversed) {
                mvprintw(row, 33, "R");
            }

            // Play position as percentage
            if (lp.lengthSamples > 0) {
                int pct = static_cast<int>(
                    100.0 * static_cast<double>(lp.playPosition) /
                    static_cast<double>(lp.lengthSamples));
                mvprintw(row, 35, "%3d%%", pct);
            }
        }
    }
}

void Tui::drawPendingOps(int startRow) {
    const auto& snap = client_.snapshot();

    attron(A_BOLD);
    mvprintw(startRow, 0, "PENDING");
    attroff(A_BOLD);

    if (snap.pendingOps.empty()) {
        mvprintw(startRow, 10, "(none)");
        return;
    }

    int shown = 0;
    for (const auto& op : snap.pendingOps) {
        if (shown >= 3) {
            mvprintw(startRow + 1 + shown, 2, "... and %d more",
                     static_cast<int>(snap.pendingOps.size()) - shown);
            break;
        }
        attron(COLOR_PAIR(6));
        std::string qstr = op.quantize == Quantize::Beat ? "beat" : "bar";
        mvprintw(startRow + 1 + shown, 2, "Loop %d: %s @next %s",
                 op.loopIndex, op.description.c_str(), qstr.c_str());
        attroff(COLOR_PAIR(6));
        ++shown;
    }
}

void Tui::drawControls(int startRow) {
    attron(A_BOLD);
    mvprintw(startRow, 0, "CONTROLS");
    attroff(A_BOLD);

    mvprintw(startRow + 1, 2, "1-8/Up/Dn: Loop     SPACE: Capture loop    r: Record/stop");
    mvprintw(startRow + 2, 2, "m: Mute/unmute      v: Reverse             o/O: Overdub on/off");
    mvprintw(startRow + 3, 2, "u: Undo layer       U: Redo layer          c: Clear loop");
    mvprintw(startRow + 4, 2, "[/]: Speed -/+      Tab: Quantize mode     +/-: BPM +/-5");
    mvprintw(startRow + 5, 2, "B/b: Lookback +/-   M: Click on/off        t: Tap tempo");
    mvprintw(startRow + 6, 2, "S: MIDI sync on/off Esc: Cancel pending    q: Quit");
}

void Tui::drawMessages(int startRow) {
    attron(A_BOLD);
    mvprintw(startRow, 0, "LOG");
    attroff(A_BOLD);

    std::lock_guard<std::mutex> lock(messageMutex_);
    int row = startRow + 1;
    for (const auto& msg : messages_) {
        if (row >= termHeight_ - 1) break;
        mvprintw(row, 2, "%s", msg.c_str());
        ++row;
    }
}

void Tui::handleKey(int key) {
    const auto& snap = client_.snapshot();
    Quantize q = snap.defaultQuantize;

    switch (key) {
        // Loop selection: 1-8
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8':
            selectedLoop_ = key - '1';
            break;

        // Loop navigation: up/down arrows (with wrap)
        case KEY_UP:
            selectedLoop_ = (selectedLoop_ - 1 + snap.maxLoops) % snap.maxLoops;
            break;
        case KEY_DOWN:
            selectedLoop_ = (selectedLoop_ + 1) % snap.maxLoops;
            break;

        // Capture loop from ring buffer
        case ' ':
            client_.scheduleCaptureLoop(selectedLoop_, q);
            break;

        // Classic record toggle
        case 'r':
            if (snap.isRecording && snap.recordingLoopIndex == selectedLoop_) {
                client_.scheduleStopRecord(selectedLoop_, q);
            } else if (!snap.isRecording) {
                client_.scheduleRecord(selectedLoop_, q);
            }
            break;

        // Mute/unmute
        case 'm':
            client_.scheduleOp(OpType::ToggleMute, selectedLoop_, q);
            break;

        // Toggle metronome click
        case 'M': {
            bool on = !snap.clickEnabled;
            client_.setMetronomeClickEnabled(on);
            addMessage(std::string("Metronome click: ") + (on ? "ON" : "OFF"));
            break;
        }

        // Toggle MIDI sync output
        case 'S': {
            bool on = !snap.midiSyncEnabled;
            client_.setMidiSyncEnabled(on);
            addMessage(std::string("MIDI sync: ") + (on ? "ON" : "OFF"));
            break;
        }

        // Reverse
        case 'v':
            client_.scheduleOp(OpType::Reverse, selectedLoop_, q);
            break;

        // Start overdub
        case 'o':
            client_.scheduleOp(OpType::StartOverdub, selectedLoop_, q);
            break;

        // Stop overdub
        case 'O':
            client_.scheduleOp(OpType::StopOverdub, selectedLoop_, q);
            break;

        // Undo layer
        case 'u':
            client_.scheduleOp(OpType::UndoLayer, selectedLoop_, Quantize::Free);
            break;

        // Redo layer
        case 'U':
            client_.scheduleOp(OpType::RedoLayer, selectedLoop_, Quantize::Free);
            break;

        // Clear loop
        case 'c':
            client_.executeOpNow(OpType::ClearLoop, selectedLoop_);
            break;

        // Speed decrease
        case '[': {
            const auto& lp = snap.loops[static_cast<size_t>(selectedLoop_)];
            double newSpeed = lp.speed * 0.5;
            client_.scheduleSetSpeed(selectedLoop_, newSpeed, q);
            break;
        }

        // Speed increase
        case ']': {
            const auto& lp = snap.loops[static_cast<size_t>(selectedLoop_)];
            double newSpeed = lp.speed * 2.0;
            client_.scheduleSetSpeed(selectedLoop_, newSpeed, q);
            break;
        }

        // Cycle quantize mode
        case '\t': {
            switch (snap.defaultQuantize) {
                case Quantize::Free:
                    client_.setDefaultQuantize(Quantize::Beat);
                    addMessage("Quantize: BEAT");
                    break;
                case Quantize::Beat:
                    client_.setDefaultQuantize(Quantize::Bar);
                    addMessage("Quantize: BAR");
                    break;
                case Quantize::Bar:
                    client_.setDefaultQuantize(Quantize::Free);
                    addMessage("Quantize: FREE");
                    break;
            }
            break;
        }

        // BPM adjust
        case '+':
        case '=':
            client_.setBpm(snap.metronome.bpm + 5.0);
            addMessage("BPM: " + std::to_string(static_cast<int>(snap.metronome.bpm + 5.0)));
            break;

        case '-':
            client_.setBpm(snap.metronome.bpm - 5.0);
            addMessage("BPM: " + std::to_string(static_cast<int>(snap.metronome.bpm - 5.0)));
            break;

        // Lookback bars adjust
        case 'B': {
            int actual = client_.setLookbackBars(snap.lookbackBars + 1);
            addMessage("Lookback: " + std::to_string(actual) + " bar(s)");
            break;
        }

        case 'b': {
            int actual = client_.setLookbackBars(snap.lookbackBars - 1);
            addMessage("Lookback: " + std::to_string(actual) + " bar(s)");
            break;
        }

        // Tap tempo
        case 't':
            handleTapTempo();
            break;

        // Cancel pending
        case 27: // Escape
            client_.cancelPending();
            break;

        default:
            break;
    }
}

void Tui::handleTapTempo() {
    auto now = std::chrono::steady_clock::now();

    // Reset if too long since last tap
    if (!tapTimes_.empty()) {
        double elapsed = std::chrono::duration<double>(now - tapTimes_.back()).count();
        if (elapsed > tapTimeoutSec_) {
            tapTimes_.clear();
        }
    }

    tapTimes_.push_back(now);

    // Keep only the most recent taps
    while (static_cast<int>(tapTimes_.size()) > maxTaps_) {
        tapTimes_.erase(tapTimes_.begin());
    }

    // Need at least 2 taps to compute BPM
    if (tapTimes_.size() < 2) {
        addMessage("Tap tempo: tap again...");
        return;
    }

    // Average the intervals
    double totalSec = std::chrono::duration<double>(
        tapTimes_.back() - tapTimes_.front()).count();
    double avgInterval = totalSec / (tapTimes_.size() - 1);
    double bpm = 60.0 / avgInterval;

    client_.setBpm(bpm);

    std::ostringstream msg;
    msg << std::fixed << std::setprecision(1) << "Tap tempo: " << bpm << " BPM";
    addMessage(msg.str());
}

void Tui::addMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(messageMutex_);
    messages_.push_front(msg);
    while (static_cast<int>(messages_.size()) > maxMessages_) {
        messages_.pop_back();
    }
}

void Tui::setSelectedLoop(int index) {
    int maxLoops = client_.snapshot().maxLoops;
    selectedLoop_ = std::clamp(index, 0, maxLoops - 1);
}

} // namespace retrospect
