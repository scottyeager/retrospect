#!/usr/bin/env python3
"""
retro_cli - Interactive command-line controller for the Retrospect looper.

Connects to the Retrospect OSC server and provides a live status display
with keyboard commands that mirror the ncurses TUI.

Usage:
    python retro_cli.py [--host HOST] [--port PORT]
"""

from __future__ import annotations

import argparse
import sys
import threading
import time

from retrospect_client import (
    EngineState,
    LoopState,
    Quantize,
    RetrospectClient,
)

# ANSI color codes
RESET = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
RED = "\033[31m"
CYAN = "\033[36m"
MAGENTA = "\033[35m"
BLUE = "\033[34m"
WHITE = "\033[37m"
CLEAR_SCREEN = "\033[2J\033[H"

QUANTIZE_NAMES = {Quantize.FREE: "Free", Quantize.BEAT: "Beat", Quantize.BAR: "Bar"}
STATE_DISPLAY = {
    LoopState.EMPTY: (DIM + "---" + RESET, ""),
    LoopState.PLAYING: ("PLY", GREEN),
    LoopState.MUTED: ("MUT", YELLOW),
    LoopState.RECORDING: ("REC", RED),
}


def format_state(state: EngineState, selected_loop: int) -> str:
    """Render the engine state as an ANSI string for the terminal."""
    lines: list[str] = []

    # Header
    met = state.metronome
    lines.append(
        f"{BOLD}{BLUE}RETROSPECT{RESET}  "
        f"{met.bpm:.1f} BPM  {met.beats_per_bar}/4  "
        f"Bar {met.bar + 1}  Beat {met.beat + 1}  "
        f"Q={QUANTIZE_NAMES[state.default_quantize]}  "
        f"Lookback={state.lookback_bars}  "
        f"Click={'ON' if state.click_enabled else 'OFF'}"
    )

    # Beat visualization
    beat_bar = ""
    for b in range(met.beats_per_bar):
        if b == met.beat:
            beat_bar += f"{BOLD}{WHITE}[X]{RESET} "
        else:
            beat_bar += f"{DIM}[ ]{RESET} "
    lines.append(f"  {beat_bar}")
    lines.append("")

    # Loop table header
    lines.append(
        f"  {'#':>2}  {'State':6} {'Bars':>5} {'Layers':>7} "
        f"{'Speed':>6} {'Rev':>3} {'Pos':>5}"
    )
    lines.append(f"  {'─' * 48}")

    for i, lp in enumerate(state.loops):
        label, color = STATE_DISPLAY.get(lp.state, ("???", ""))
        sel = f"{CYAN}>{RESET}" if i == selected_loop else " "

        if lp.is_empty:
            lines.append(f" {sel}{i + 1:>2}  {DIM}---{RESET}")
        else:
            rev = "R" if lp.reversed else " "
            pos_pct = lp.play_position_pct * 100
            layer_str = f"{lp.active_layers}/{lp.layers}"
            lines.append(
                f" {sel}{i + 1:>2}  {color}{label:6}{RESET} "
                f"{lp.length_in_bars:5.1f} {layer_str:>7} "
                f"{lp.speed:6.2f}x {rev:>3} {pos_pct:5.1f}%"
            )

    # Pending ops
    if state.pending_ops:
        lines.append("")
        lines.append(f"  {MAGENTA}Pending:{RESET}")
        for op in state.pending_ops:
            lines.append(f"    {MAGENTA}{op.description}{RESET}")

    # Recording indicator
    if state.is_recording:
        lines.append("")
        lines.append(
            f"  {RED}{BOLD}● RECORDING loop {state.recording_loop_index + 1}{RESET}"
        )

    # Recent messages
    if state.messages:
        lines.append("")
        for msg in state.messages[-4:]:
            lines.append(f"  {DIM}{msg}{RESET}")

    # Controls help
    lines.append("")
    lines.append(
        f"  {DIM}1-8:select  space:capture  r:record  m:mute  "
        f"v:reverse  o/O:overdub  u/U:undo/redo{RESET}"
    )
    lines.append(
        f"  {DIM}c:clear  [/]:speed  +/-:bpm  tab:quantize  "
        f"B/b:lookback  M:click  esc:cancel  q:quit{RESET}"
    )

    return "\n".join(lines)


def run_display(client: RetrospectClient, selected_loop_ref: list[int]) -> None:
    """Refresh the terminal display at ~15Hz."""
    while True:
        state = client.state
        output = format_state(state, selected_loop_ref[0])
        sys.stdout.write(CLEAR_SCREEN + output + "\n")
        sys.stdout.flush()
        time.sleep(1 / 15)


def main() -> None:
    parser = argparse.ArgumentParser(description="Retrospect looper CLI controller")
    parser.add_argument("--host", default="127.0.0.1", help="Server host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=7770, help="Server OSC port (default: 7770)")
    args = parser.parse_args()

    client = RetrospectClient(args.host, args.port)
    listen_port = client.start()
    print(f"Connected to {args.host}:{args.port}, listening on port {listen_port}")
    time.sleep(0.3)  # Let initial state arrive

    selected = [0]  # Mutable ref for display thread

    # Start display thread
    display_thread = threading.Thread(target=run_display, args=(client, selected), daemon=True)
    display_thread.start()

    # Read keyboard input
    try:
        import tty
        import termios

        fd = sys.stdin.fileno()
        old_settings = termios.tcgetattr(fd)
        tty.setcbreak(fd)

        try:
            while True:
                ch = sys.stdin.read(1)
                s = client.state
                q = s.default_quantize
                idx = selected[0]

                if ch == "q":
                    break
                elif ch in "12345678":
                    selected[0] = int(ch) - 1
                elif ch == " ":
                    client.capture(idx, q)
                elif ch == "r":
                    if s.is_recording and s.recording_loop_index == idx:
                        client.stop_record(idx, q)
                    else:
                        client.record(idx, q)
                elif ch == "m":
                    client.toggle_mute(idx, q)
                elif ch == "v":
                    client.reverse(idx, q)
                elif ch == "o":
                    client.overdub_start(idx, q)
                elif ch == "O":
                    client.overdub_stop(idx, q)
                elif ch == "u":
                    client.undo(idx)
                elif ch == "U":
                    client.redo(idx)
                elif ch == "c":
                    client.clear(idx)
                elif ch == "[":
                    lp = s.loops[idx]
                    client.set_speed(idx, max(0.25, lp.speed / 2.0))
                elif ch == "]":
                    lp = s.loops[idx]
                    client.set_speed(idx, min(4.0, lp.speed * 2.0))
                elif ch == "+":
                    client.set_bpm(s.metronome.bpm + 5.0)
                elif ch == "-":
                    client.set_bpm(max(30.0, s.metronome.bpm - 5.0))
                elif ch == "\t":
                    next_q = Quantize((int(q) + 1) % 3)
                    client.set_quantize(next_q)
                elif ch == "B":
                    client.set_lookback_bars(min(8, s.lookback_bars + 1))
                elif ch == "b":
                    client.set_lookback_bars(max(1, s.lookback_bars - 1))
                elif ch == "M":
                    client.set_click(not s.click_enabled)
                elif ch == "\x1b":
                    client.cancel_pending()
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)

    except ImportError:
        # Windows or no tty - fall back to line input
        print("Interactive mode not available. Use line commands:")
        print("  capture <loop>  record <loop>  mute <loop>  clear <loop>")
        print("  bpm <value>     quit")
        while True:
            try:
                line = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                break
            parts = line.split()
            if not parts:
                continue
            cmd = parts[0].lower()
            if cmd == "quit":
                break
            elif cmd == "capture" and len(parts) > 1:
                client.capture(int(parts[1]))
            elif cmd == "record" and len(parts) > 1:
                client.record(int(parts[1]))
            elif cmd == "stop" and len(parts) > 1:
                client.stop_record(int(parts[1]))
            elif cmd == "mute" and len(parts) > 1:
                client.toggle_mute(int(parts[1]))
            elif cmd == "clear" and len(parts) > 1:
                client.clear(int(parts[1]))
            elif cmd == "bpm" and len(parts) > 1:
                client.set_bpm(float(parts[1]))
            else:
                print(f"Unknown command: {line}")

    client.stop()
    print("\nBye!")


if __name__ == "__main__":
    main()
