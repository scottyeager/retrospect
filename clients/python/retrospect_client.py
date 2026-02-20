"""
retrospect_client - Python OSC client for the Retrospect looper.

Connects to the Retrospect OSC server, sends commands, and receives
real-time state updates via subscription.

Requires: python-osc (pip install python-osc)

Usage:
    from retrospect_client import RetrospectClient

    client = RetrospectClient("127.0.0.1", 7770)
    client.start()

    client.capture(0)           # Capture loop 0
    client.toggle_mute(0)       # Toggle mute on loop 0
    client.set_bpm(140.0)       # Change BPM

    state = client.state        # Current engine state
    print(state.metronome.bpm)

    client.stop()
"""

from __future__ import annotations

import enum
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

from pythonosc import osc_message_builder, osc_server, udp_client
from pythonosc.dispatcher import Dispatcher


# ---------------------------------------------------------------------------
# Enums
# ---------------------------------------------------------------------------

class Quantize(enum.IntEnum):
    FREE = 0
    BEAT = 1
    BAR = 2


class LoopState(enum.IntEnum):
    EMPTY = 0
    PLAYING = 1
    MUTED = 2
    RECORDING = 3


# ---------------------------------------------------------------------------
# State dataclasses (mirrors EngineSnapshot pushed by the server)
# ---------------------------------------------------------------------------

@dataclass
class MetronomeState:
    bar: int = 0
    beat: int = 0
    beat_fraction: float = 0.0
    bpm: float = 120.0
    beats_per_bar: int = 4
    running: bool = True


@dataclass
class LoopInfo:
    state: LoopState = LoopState.EMPTY
    length_in_bars: float = 0.0
    layers: int = 0
    active_layers: int = 0
    speed: float = 1.0
    reversed: bool = False
    play_position_pct: float = 0.0
    length_samples: int = 0

    @property
    def is_empty(self) -> bool:
        return self.state == LoopState.EMPTY

    @property
    def is_playing(self) -> bool:
        return self.state == LoopState.PLAYING

    @property
    def is_muted(self) -> bool:
        return self.state == LoopState.MUTED

    @property
    def is_recording(self) -> bool:
        return self.state == LoopState.RECORDING


@dataclass
class PendingOp:
    loop_index: int = -1
    quantize: Quantize = Quantize.BAR
    description: str = ""


@dataclass
class EngineState:
    metronome: MetronomeState = field(default_factory=MetronomeState)
    loops: list[LoopInfo] = field(default_factory=lambda: [LoopInfo() for _ in range(8)])
    pending_ops: list[PendingOp] = field(default_factory=list)
    is_recording: bool = False
    recording_loop_index: int = -1
    default_quantize: Quantize = Quantize.BAR
    lookback_bars: int = 1
    click_enabled: bool = True
    sample_rate: int = 44100
    messages: list[str] = field(default_factory=list)

    @property
    def active_loop_count(self) -> int:
        return sum(1 for lp in self.loops if not lp.is_empty)


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------

class RetrospectClient:
    """OSC client for controlling a Retrospect looper server.

    Args:
        host: Server hostname or IP address.
        port: Server OSC port (default 7770).
        listen_port: Local port for receiving state pushes. 0 = auto-assign.
    """

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 7770,
        listen_port: int = 0,
    ) -> None:
        self._host = host
        self._port = port
        self._listen_port = listen_port

        self._state = EngineState()
        self._lock = threading.Lock()

        self._client = udp_client.SimpleUDPClient(host, port)
        self._server: Optional[osc_server.ThreadingOSCUDPServer] = None
        self._server_thread: Optional[threading.Thread] = None
        self._running = False

        # Callbacks
        self._on_state_update: Optional[Callable[[EngineState], None]] = None
        self._on_log: Optional[Callable[[str], None]] = None

        # Build dispatcher
        self._dispatcher = Dispatcher()
        self._dispatcher.map("/retro/state/metronome", self._handle_metronome)
        self._dispatcher.map("/retro/state/loop", self._handle_loop)
        self._dispatcher.map("/retro/state/recording", self._handle_recording)
        self._dispatcher.map("/retro/state/settings", self._handle_settings)
        self._dispatcher.map("/retro/state/pending_clear", self._handle_pending_clear)
        self._dispatcher.map("/retro/state/pending_op", self._handle_pending_op)
        self._dispatcher.map("/retro/state/log", self._handle_log)

    # -- Lifecycle ------------------------------------------------------------

    def start(self) -> int:
        """Start receiving state updates from the server.

        Returns the local port being used for receiving.
        """
        self._server = osc_server.ThreadingOSCUDPServer(
            ("0.0.0.0", self._listen_port), self._dispatcher
        )
        actual_port = self._server.server_address[1]
        self._listen_port = actual_port

        self._running = True
        self._server_thread = threading.Thread(
            target=self._server.serve_forever, daemon=True
        )
        self._server_thread.start()

        # Subscribe with the server
        self._subscribe()

        # Start heartbeat thread
        self._heartbeat_thread = threading.Thread(
            target=self._heartbeat_loop, daemon=True
        )
        self._heartbeat_thread.start()

        return actual_port

    def stop(self) -> None:
        """Unsubscribe and stop the listener."""
        self._running = False
        if self._server:
            self._unsubscribe()
            self._server.shutdown()
            self._server = None

    @property
    def state(self) -> EngineState:
        """Current engine state snapshot (thread-safe copy)."""
        with self._lock:
            # Return a shallow reference; individual fields are replaced
            # atomically in handlers so this is safe for reading.
            return self._state

    @property
    def host(self) -> str:
        return self._host

    @property
    def port(self) -> int:
        return self._port

    @property
    def listen_port(self) -> int:
        return self._listen_port

    # -- Callbacks ------------------------------------------------------------

    def on_state_update(self, callback: Optional[Callable[[EngineState], None]]) -> None:
        """Register a callback invoked on each state push from the server."""
        self._on_state_update = callback

    def on_log(self, callback: Optional[Callable[[str], None]]) -> None:
        """Register a callback invoked for each log message from the server."""
        self._on_log = callback

    # -- Loop commands --------------------------------------------------------

    def capture(
        self,
        loop: int,
        quantize: Quantize = Quantize.BAR,
        lookback_bars: int = 0,
    ) -> None:
        """Capture audio from the ring buffer into a loop.

        Args:
            loop: Loop index (0-7).
            quantize: Quantization mode.
            lookback_bars: How many bars back to capture. 0 uses the current
                lookback_bars setting.
        """
        self._client.send_message("/retro/loop/capture", [loop, int(quantize), lookback_bars])

    def record(self, loop: int, quantize: Quantize = Quantize.BAR) -> None:
        """Start classic recording on a loop."""
        self._client.send_message("/retro/loop/record", [loop, int(quantize)])

    def stop_record(self, loop: int, quantize: Quantize = Quantize.BAR) -> None:
        """Stop classic recording on a loop."""
        self._client.send_message("/retro/loop/stop_record", [loop, int(quantize)])

    def mute(self, loop: int, quantize: Quantize = Quantize.BAR) -> None:
        """Mute a loop."""
        self._client.send_message("/retro/loop/mute", [loop, int(quantize)])

    def unmute(self, loop: int, quantize: Quantize = Quantize.BAR) -> None:
        """Unmute a loop."""
        self._client.send_message("/retro/loop/unmute", [loop, int(quantize)])

    def toggle_mute(self, loop: int, quantize: Quantize = Quantize.BAR) -> None:
        """Toggle mute on a loop."""
        self._client.send_message("/retro/loop/toggle_mute", [loop, int(quantize)])

    def reverse(self, loop: int, quantize: Quantize = Quantize.BAR) -> None:
        """Toggle reverse playback on a loop."""
        self._client.send_message("/retro/loop/reverse", [loop, int(quantize)])

    def overdub_start(self, loop: int, quantize: Quantize = Quantize.BAR) -> None:
        """Start overdubbing on a loop."""
        self._client.send_message("/retro/loop/overdub/start", [loop, int(quantize)])

    def overdub_stop(self, loop: int, quantize: Quantize = Quantize.BAR) -> None:
        """Stop overdubbing on a loop."""
        self._client.send_message("/retro/loop/overdub/stop", [loop, int(quantize)])

    def undo(self, loop: int) -> None:
        """Undo the last overdub layer on a loop."""
        self._client.send_message("/retro/loop/undo", [loop])

    def redo(self, loop: int) -> None:
        """Redo the last undone layer on a loop."""
        self._client.send_message("/retro/loop/redo", [loop])

    def set_speed(
        self,
        loop: int,
        speed: float,
        quantize: Quantize = Quantize.FREE,
    ) -> None:
        """Set playback speed for a loop (0.25 - 4.0)."""
        self._client.send_message("/retro/loop/speed", [loop, float(speed), int(quantize)])

    def clear(self, loop: int) -> None:
        """Clear a loop (delete all audio)."""
        self._client.send_message("/retro/loop/clear", [loop])

    # -- Global commands ------------------------------------------------------

    def set_bpm(self, bpm: float) -> None:
        """Set the metronome BPM."""
        self._client.send_message("/retro/metronome/bpm", [float(bpm)])

    def set_click(self, enabled: bool) -> None:
        """Enable or disable the metronome click."""
        self._client.send_message("/retro/metronome/click", [1 if enabled else 0])

    def set_quantize(self, quantize: Quantize) -> None:
        """Set the default quantization mode."""
        self._client.send_message("/retro/settings/quantize", [int(quantize)])

    def set_lookback_bars(self, bars: int) -> None:
        """Set the number of lookback bars for capture."""
        self._client.send_message("/retro/settings/lookback_bars", [bars])

    def cancel_pending(self) -> None:
        """Cancel all pending (queued) operations."""
        self._client.send_message("/retro/cancel_pending", [])

    # -- Internal: subscription -----------------------------------------------

    def _subscribe(self) -> None:
        self._client.send_message(
            "/retro/client/subscribe", ["localhost", self._listen_port]
        )

    def _unsubscribe(self) -> None:
        self._client.send_message(
            "/retro/client/unsubscribe", ["localhost", self._listen_port]
        )

    def _heartbeat_loop(self) -> None:
        while self._running:
            time.sleep(10.0)
            if self._running:
                self._subscribe()

    # -- Internal: OSC handlers -----------------------------------------------

    def _handle_metronome(self, address: str, *args) -> None:
        with self._lock:
            m = self._state.metronome
            m.bar = args[0]
            m.beat = args[1]
            m.beat_fraction = args[2]
            m.bpm = args[3]
            m.beats_per_bar = args[4]
            m.running = bool(args[5])
        if self._on_state_update:
            self._on_state_update(self._state)

    def _handle_loop(self, address: str, *args) -> None:
        idx = args[0]
        with self._lock:
            if idx >= len(self._state.loops):
                self._state.loops.extend(
                    LoopInfo() for _ in range(idx + 1 - len(self._state.loops))
                )
            lp = self._state.loops[idx]
            lp.state = LoopState(args[1])
            lp.length_in_bars = args[2]
            lp.layers = args[3]
            lp.active_layers = args[4]
            lp.speed = args[5]
            lp.reversed = bool(args[6])
            lp.play_position_pct = args[7]
            lp.length_samples = args[8]

    def _handle_recording(self, address: str, *args) -> None:
        with self._lock:
            self._state.is_recording = bool(args[0])
            self._state.recording_loop_index = args[1]

    def _handle_settings(self, address: str, *args) -> None:
        with self._lock:
            self._state.default_quantize = Quantize(args[0])
            self._state.lookback_bars = args[1]
            self._state.click_enabled = bool(args[2])
            self._state.sample_rate = args[3]

    def _handle_pending_clear(self, address: str, *args) -> None:
        with self._lock:
            self._state.pending_ops.clear()

    def _handle_pending_op(self, address: str, *args) -> None:
        op = PendingOp(
            loop_index=args[0],
            quantize=Quantize(args[1]),
            description=args[2],
        )
        with self._lock:
            self._state.pending_ops.append(op)

    def _handle_log(self, address: str, *args) -> None:
        msg = args[0]
        with self._lock:
            self._state.messages.append(msg)
        if self._on_log:
            self._on_log(msg)
