#include "tui/Tui.h"
#include <ncurses.h>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace retrospect {

Tui::Tui(LoopEngine& engine)
    : engine_(engine)
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
    erase();

    int row = 0;
    drawHeader(row);
    row += 2;

    drawMetronome(row);
    row += 3;

    drawLoops(row);
    row += engine_.maxLoops() + 2;

    drawPendingOps(row);
    row += static_cast<int>(std::min(engine_.pendingOps().size(), size_t(3))) + 2;

    drawControls(row);
    row += 6;

    drawMessages(row);

    refresh();
    needsRedraw_ = false;
}

void Tui::drawHeader(int row) {
    attron(A_BOLD | COLOR_PAIR(7));
    mvprintw(row, 0, "RETROSPECT");
    attroff(A_BOLD | COLOR_PAIR(7));
    mvprintw(row, 12, "v0.1.0 - Live Audio Looper");

    // Engine status
    std::ostringstream status;
    status << "Loops: " << engine_.activeLoopCount() << "/" << engine_.maxLoops()
           << "  SR: " << static_cast<int>(engine_.sampleRate()) << "Hz";
    mvprintw(row + 1, 0, "%s", status.str().c_str());
}

void Tui::drawMetronome(int row) {
    const auto& met = engine_.metronome();
    auto pos = met.position();

    attron(A_BOLD);
    mvprintw(row, 0, "METRONOME");
    attroff(A_BOLD);

    std::ostringstream info;
    info << std::fixed << std::setprecision(1)
         << met.bpm() << " BPM  "
         << met.beatsPerBar() << "/4  "
         << (met.isRunning() ? "RUNNING" : "STOPPED");
    mvprintw(row, 12, "%s", info.str().c_str());

    // Beat visualization: show current bar.beat position
    std::ostringstream posStr;
    posStr << "Bar " << (pos.bar + 1) << "  Beat " << (pos.beat + 1);
    mvprintw(row + 1, 2, "%s", posStr.str().c_str());

    // Beat indicator: visual beat display
    std::string beatVis = "  ";
    for (int b = 0; b < met.beatsPerBar(); ++b) {
        if (b == pos.beat) {
            beatVis += "[X] ";
        } else {
            beatVis += "[ ] ";
        }
    }
    mvprintw(row + 1, 24, "%s", beatVis.c_str());

    // Quantize mode
    std::string qmode;
    switch (engine_.defaultQuantize()) {
        case Quantize::Free: qmode = "FREE"; break;
        case Quantize::Beat: qmode = "BEAT"; break;
        case Quantize::Bar:  qmode = "BAR"; break;
    }
    mvprintw(row + 2, 2, "Quantize: %s  Lookback: %d bar(s)",
             qmode.c_str(), engine_.lookbackBars());

    // Recording indicator
    if (engine_.isRecording()) {
        attron(COLOR_PAIR(3) | A_BOLD);
        mvprintw(row + 2, 40, "** REC Loop %d **", engine_.recordingLoopIndex());
        attroff(COLOR_PAIR(3) | A_BOLD);
    }
}

void Tui::drawLoops(int startRow) {
    attron(A_BOLD);
    mvprintw(startRow, 0, "LOOPS");
    attroff(A_BOLD);

    mvprintw(startRow, 8, "# State      Bars   Layers Spd   Pos");

    for (int i = 0; i < engine_.maxLoops(); ++i) {
        int row = startRow + 1 + i;
        const auto& lp = engine_.loop(i);

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
        bool classicRec = engine_.isRecording() &&
                          engine_.recordingLoopIndex() == i;

        if (classicRec) {
            stateStr = "REC...   ";
            colorPair = 3;
        } else {
            switch (lp.state()) {
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
            mvprintw(row, 14, "%5.1f", lp.lengthInBars());

            // Layers
            mvprintw(row, 21, "%d/%d",
                     lp.activeLayerCount(), lp.layerCount());

            // Speed
            std::string spdStr;
            if (lp.speed() == 1.0) spdStr = "1x";
            else if (lp.speed() == 0.5) spdStr = "1/2x";
            else if (lp.speed() == 2.0) spdStr = "2x";
            else {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(2) << lp.speed() << "x";
                spdStr = ss.str();
            }
            mvprintw(row, 27, "%-5s", spdStr.c_str());

            // Reverse indicator
            if (lp.isReversed()) {
                mvprintw(row, 33, "R");
            }

            // Play position as percentage
            if (lp.lengthSamples() > 0) {
                int pct = static_cast<int>(
                    100.0 * static_cast<double>(lp.playPosition()) /
                    static_cast<double>(lp.lengthSamples()));
                mvprintw(row, 35, "%3d%%", pct);
            }
        }
    }
}

void Tui::drawPendingOps(int startRow) {
    attron(A_BOLD);
    mvprintw(startRow, 0, "PENDING");
    attroff(A_BOLD);

    const auto& ops = engine_.pendingOps();
    if (ops.empty()) {
        mvprintw(startRow, 10, "(none)");
        return;
    }

    int shown = 0;
    for (const auto& op : ops) {
        if (shown >= 3) {
            mvprintw(startRow + 1 + shown, 2, "... and %d more",
                     static_cast<int>(ops.size()) - shown);
            break;
        }
        attron(COLOR_PAIR(6));
        std::string qstr = op.quantize == Quantize::Beat ? "beat" : "bar";
        mvprintw(startRow + 1 + shown, 2, "Loop %d: %s @next %s",
                 op.loopIndex, op.description().c_str(), qstr.c_str());
        attroff(COLOR_PAIR(6));
        ++shown;
    }
}

void Tui::drawControls(int startRow) {
    attron(A_BOLD);
    mvprintw(startRow, 0, "CONTROLS");
    attroff(A_BOLD);

    mvprintw(startRow + 1, 2, "1-8: Select loop    SPACE: Capture loop    R: Record/stop");
    mvprintw(startRow + 2, 2, "m: Mute/unmute      r: Reverse             o/O: Overdub on/off");
    mvprintw(startRow + 3, 2, "u: Undo layer       U: Redo layer          c: Clear loop");
    mvprintw(startRow + 4, 2, "[/]: Speed -/+      Tab: Quantize mode     +/-: BPM +/-5");
    mvprintw(startRow + 5, 2, "B/b: Lookback +/-   Esc: Cancel pending    q: Quit");
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
    Quantize q = engine_.defaultQuantize();

    switch (key) {
        // Loop selection: 1-8
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8':
            selectedLoop_ = key - '1';
            break;

        // Capture loop from ring buffer
        case ' ':
            engine_.scheduleCaptureLoop(selectedLoop_, q);
            break;

        // Classic record toggle
        case 'R':
            if (engine_.isRecording() &&
                engine_.recordingLoopIndex() == selectedLoop_) {
                engine_.scheduleStopRecord(selectedLoop_, q);
            } else if (!engine_.isRecording()) {
                engine_.scheduleRecord(selectedLoop_, q);
            }
            break;

        // Mute/unmute
        case 'm':
        case 'M':
            engine_.scheduleOp(OpType::ToggleMute, selectedLoop_, q);
            break;

        // Reverse
        case 'r':
            engine_.scheduleOp(OpType::Reverse, selectedLoop_, q);
            break;

        // Start overdub
        case 'o':
            engine_.scheduleOp(OpType::StartOverdub, selectedLoop_, q);
            break;

        // Stop overdub
        case 'O':
            engine_.scheduleOp(OpType::StopOverdub, selectedLoop_, q);
            break;

        // Undo layer
        case 'u':
            engine_.scheduleOp(OpType::UndoLayer, selectedLoop_, Quantize::Free);
            break;

        // Redo layer
        case 'U':
            engine_.scheduleOp(OpType::RedoLayer, selectedLoop_, Quantize::Free);
            break;

        // Clear loop
        case 'c':
            engine_.executeOpNow(OpType::ClearLoop, selectedLoop_);
            break;

        // Speed decrease
        case '[': {
            const auto& lp = engine_.loop(selectedLoop_);
            double newSpeed = lp.speed() * 0.5;
            engine_.scheduleSetSpeed(selectedLoop_, newSpeed, q);
            break;
        }

        // Speed increase
        case ']': {
            const auto& lp = engine_.loop(selectedLoop_);
            double newSpeed = lp.speed() * 2.0;
            engine_.scheduleSetSpeed(selectedLoop_, newSpeed, q);
            break;
        }

        // Cycle quantize mode
        case '\t': {
            switch (engine_.defaultQuantize()) {
                case Quantize::Free:
                    engine_.setDefaultQuantize(Quantize::Beat);
                    addMessage("Quantize: BEAT");
                    break;
                case Quantize::Beat:
                    engine_.setDefaultQuantize(Quantize::Bar);
                    addMessage("Quantize: BAR");
                    break;
                case Quantize::Bar:
                    engine_.setDefaultQuantize(Quantize::Free);
                    addMessage("Quantize: FREE");
                    break;
            }
            break;
        }

        // BPM adjust
        case '+':
        case '=':
            engine_.metronome().setBpm(engine_.metronome().bpm() + 5.0);
            addMessage("BPM: " + std::to_string(static_cast<int>(engine_.metronome().bpm())));
            break;

        case '-':
            engine_.metronome().setBpm(engine_.metronome().bpm() - 5.0);
            addMessage("BPM: " + std::to_string(static_cast<int>(engine_.metronome().bpm())));
            break;

        // Lookback bars adjust
        case 'B':
            engine_.setLookbackBars(engine_.lookbackBars() + 1);
            addMessage("Lookback: " + std::to_string(engine_.lookbackBars()) + " bar(s)");
            break;

        case 'b':
            engine_.setLookbackBars(engine_.lookbackBars() - 1);
            addMessage("Lookback: " + std::to_string(engine_.lookbackBars()) + " bar(s)");
            break;

        // Cancel pending
        case 27: // Escape
            engine_.cancelPending();
            break;

        default:
            break;
    }
}

void Tui::addMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(messageMutex_);
    messages_.push_front(msg);
    while (static_cast<int>(messages_.size()) > maxMessages_) {
        messages_.pop_back();
    }
}

void Tui::setSelectedLoop(int index) {
    selectedLoop_ = std::clamp(index, 0, engine_.maxLoops() - 1);
}

} // namespace retrospect
