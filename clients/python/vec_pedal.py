"""
vec_pedal.py — Control Retrospect with two VEC USB transcription foot pedals.

Each VEC pedal is an HID device with 3 pedals. Presses are reported as a
single byte: 1=left, 2=center, 4=right (bitmask, so simultaneous presses add).
A value of 0 means all pedals released.

Both pedals are opened by their unique HID paths, so two identical devices
(same VID/PID) work simultaneously. All actions target the engine's selected
loops (set via the TUI or OSC).

Usage:
    python vec_pedal.py [HOST] [PORT]

    Defaults: HOST=127.0.0.1, PORT=7770

Find your pedals' VID/PID by running:
    python -c "import hid; [print(hex(d['vendor_id']), hex(d['product_id']), d['path'], d['product_string']) for d in hid.enumerate()]"
"""

import sys
import time
from threading import Thread

import hid

from retrospect_client import RetrospectClient, Quantize

# ---------------------------------------------------------------------------
# VEC pedal HID identifiers — adjust to match your device
# ---------------------------------------------------------------------------
VEC_VENDOR_ID  = 0x05F3   # PI Engineering (common VEC pedal manufacturer)
VEC_PRODUCT_ID = 0x00FF   # Adjust if needed; check hid.enumerate()

BUTTON_MAP = {1: "left", 2: "center", 4: "right"}


# ---------------------------------------------------------------------------
# Pedal actions — all target selected loops (index -1)
# ---------------------------------------------------------------------------

def on_clear(client: RetrospectClient) -> None:
    print("Clear selected loops")
    client.clear(-1)


def on_record(client: RetrospectClient) -> None:
    state = client.state
    if state.is_recording:
        print("Stop recording")
        client.stop_record(state.recording_loop_index)
    else:
        print("Record on selected loops")
        client.record(-1)


def on_capture(client: RetrospectClient) -> None:
    print("Capture selected loops")
    client.capture(-1)


ACTIONS = {
    "left":   on_clear,
    "center": on_record,
    "right":  on_capture,
}


# ---------------------------------------------------------------------------
# HID
# ---------------------------------------------------------------------------

def find_pedal_paths() -> list[bytes]:
    """Find all HID paths matching the VEC pedal VID/PID."""
    paths = []
    for info in hid.enumerate(VEC_VENDOR_ID, VEC_PRODUCT_ID):
        paths.append(info["path"])
    return paths


def open_pedal_by_path(path: bytes) -> hid.device | None:
    try:
        dev = hid.Device(path=path)
        print(f"Opened pedal at {path}")
        return dev
    except OSError:
        print(f"Could not open pedal at {path}")
        return None


def pedal_loop(
    path: bytes,
    client: RetrospectClient,
    actions: dict,
    label: str,
) -> None:
    """Read loop for a single pedal. Runs in its own thread."""
    dev = None
    pressed: set[str] = set()

    while True:
        if dev is None:
            dev = open_pedal_by_path(path)
            if dev is None:
                time.sleep(2)
                continue

        try:
            data = dev.read(8)
        except (OSError, ValueError):
            print(f"[{label}] Disconnected, retrying…")
            dev = None
            pressed.clear()
            time.sleep(2)
            continue

        if not data:
            continue

        byte = data[0]
        now_pressed: set[str] = {
            name for bit, name in BUTTON_MAP.items() if byte & bit
        }

        for name in now_pressed - pressed:
            action = actions.get(name)
            if action:
                try:
                    action(client)
                except Exception as exc:
                    print(f"[{label}] Error in {name} handler: {exc}")

        pressed = now_pressed


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7770

    client = RetrospectClient(host, port)
    listen_port = client.start()
    print(f"Connected to Retrospect at {host}:{port} (listening on :{listen_port})")
    client.on_log(lambda msg: print(f"[server] {msg}"))

    paths = find_pedal_paths()
    if not paths:
        print(f"No VEC pedals found (VID={VEC_VENDOR_ID:#06x} PID={VEC_PRODUCT_ID:#06x})")
        print("Run with: python -c \"import hid; [print(hex(d['vendor_id']), hex(d['product_id']), d['path'], d['product_string']) for d in hid.enumerate()]\"")
        client.stop()
        return

    print(f"Found {len(paths)} pedal(s)")

    threads = []
    for i, path in enumerate(paths):
        label = f"pedal{i+1}"
        t = Thread(target=pedal_loop, args=(path, client, ACTIONS, label), daemon=True)
        t.start()
        threads.append(t)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nExiting.")
    finally:
        client.stop()


if __name__ == "__main__":
    main()
