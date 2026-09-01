#!/usr/bin/env python3
"""R5F FreeRTOS Protocol Simulator - telemetry server.

Reads newline delimited JSON from the Pico's USB serial port and pushes it to
browsers over Server-Sent Events. Serves the dashboard from the same port.

Standard library only: no pip, no venv, no network needed to install it. That
is deliberate - this has to come up in a room with no internet.

    python3 server/app.py                  # find the board and serve on :8000
    python3 server/app.py --demo           # no board: synthesise a session
    python3 server/app.py --replay FILE    # no board: replay a captured log
    python3 server/app.py --log FILE       # also record what arrives
"""
from __future__ import annotations

import argparse
import http.server
import json
import os
import queue
import socket
import subprocess
import sys
import termios
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DASHBOARD = os.path.join(ROOT, "dashboard", "index.html")

HISTORY = 600          # ~60 s at 10 Hz, sent to a browser when it connects
PICO_VID = "2e8a"


# --------------------------------------------------------------------- fan-out

class Hub:
    """Latest sample, a short history, and a queue per connected browser."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._subs: set[queue.Queue] = set()
        self.history: list[dict] = []
        self.latest: dict | None = None
        self.connected = False
        self.source = "starting"
        self.received = 0
        self.bad_lines = 0

    def subscribe(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=64)
        with self._lock:
            self._subs.add(q)
        return q

    def unsubscribe(self, q: queue.Queue) -> None:
        with self._lock:
            self._subs.discard(q)

    def publish(self, sample: dict) -> None:
        with self._lock:
            self.latest = sample
            self.received += 1
            self.history.append(sample)
            if len(self.history) > HISTORY:
                del self.history[: len(self.history) - HISTORY]
            subs = list(self._subs)
        for q in subs:
            try:
                q.put_nowait(sample)
            except queue.Full:
                # A browser that cannot keep up loses samples rather than
                # slowing down the reader.
                pass

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "connected": self.connected,
                "source": self.source,
                "received": self.received,
                "bad_lines": self.bad_lines,
                "history": list(self.history),
            }


HUB = Hub()


# ---------------------------------------------------------------- serial input

def find_port() -> str | None:
    """Prefer a tty that really is a Raspberry Pi device, else any ttyACM."""
    candidates = sorted(
        p for p in os.listdir("/dev") if p.startswith(("ttyACM", "ttyUSB"))
    )
    for name in candidates:
        vid_path = f"/sys/class/tty/{name}/device/../idVendor"
        try:
            with open(vid_path, encoding="ascii") as fh:
                if fh.read().strip().lower() == PICO_VID:
                    return f"/dev/{name}"
        except OSError:
            continue
    return f"/dev/{candidates[0]}" if candidates else None


def open_raw(path: str) -> int:
    """Open a CDC ACM port in raw mode.

    CLOCAL is what stops the open blocking on carrier detect, and raw mode is
    what stops the line discipline eating or rewriting bytes.
    """
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        iflag, oflag, cflag, lflag, _, _, cc = termios.tcgetattr(fd)
        iflag = 0
        oflag = 0
        lflag = 0
        cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
        cc = list(cc)
        cc[termios.VMIN] = 1
        cc[termios.VTIME] = 0
        termios.tcsetattr(
            fd, termios.TCSANOW,
            [iflag, oflag, cflag, lflag, termios.B115200, termios.B115200, cc],
        )
        termios.tcflush(fd, termios.TCIFLUSH)
        os.set_blocking(fd, True)
    except Exception:
        os.close(fd)
        raise
    return fd


def serial_reader(explicit: str | None, log_path: str | None) -> None:
    """Read forever, reopening whenever the board is unplugged or reflashed."""
    log = open(log_path, "a", encoding="utf-8") if log_path else None
    buf = b""

    while True:
        path = explicit or find_port()
        if not path or not os.path.exists(path):
            HUB.connected = False
            HUB.source = "waiting for the board"
            time.sleep(1.0)
            continue

        try:
            fd = open_raw(path)
        except OSError as exc:
            HUB.connected = False
            HUB.source = f"cannot open {path}: {exc}"
            time.sleep(1.0)
            continue

        HUB.connected = True
        HUB.source = path
        print(f"[serial] reading {path}", flush=True)
        buf = b""

        try:
            while True:
                chunk = os.read(fd, 4096)
                if not chunk:
                    raise OSError("port closed")
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.strip()
                    if not text:
                        continue
                    try:
                        sample = json.loads(text)
                    except (ValueError, UnicodeDecodeError):
                        HUB.bad_lines += 1
                        continue
                    if log:
                        log.write(text.decode("utf-8", "replace") + "\n")
                        log.flush()
                    HUB.publish(sample)
                if len(buf) > 1 << 20:      # never grow without bound
                    buf = b""
        except OSError as exc:
            print(f"[serial] {path} dropped: {exc}", flush=True)
            HUB.connected = False
            HUB.source = "reconnecting"
        finally:
            try:
                os.close(fd)
            except OSError:
                pass
        time.sleep(0.5)


# ------------------------------------------------------- offline input sources

def replay_reader(path: str) -> None:
    """Replay a captured log at its original rate, looping forever."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        samples = []
        for line in fh:
            line = line.strip()
            if line.startswith("{"):
                try:
                    samples.append(json.loads(line))
                except ValueError:
                    pass
    if not samples:
        print(f"[replay] {path} has no usable samples", flush=True)
        return

    HUB.connected = True
    HUB.source = f"replay of {os.path.basename(path)}"
    print(f"[replay] {len(samples)} samples from {path}", flush=True)
    while True:
        for sample in samples:
            HUB.publish(sample)
            time.sleep(0.1)


def demo_reader() -> None:
    """Synthesise a plausible session so the dashboard can be shown without
    the board attached. Clearly labelled as demo everywhere it surfaces."""
    HUB.connected = True
    HUB.source = "demo (no board attached)"
    print("[demo] generating synthetic telemetry", flush=True)

    clk, budget, ring, peak_expect = 150_000_000, 13020, 4096, 796
    t0 = time.time()
    seq = 0
    rx = frames = 0
    trace_seq = 0

    while True:
        t = time.time() - t0
        phase = t % 1.0
        bursting = phase < 0.089
        bridge = 0
        if bursting:
            if phase < 0.019845:
                bridge = int(peak_expect * phase / 0.019845)
            else:
                bridge = int(peak_expect * (1 - (phase - 0.019845) / 0.0691))
        bridge = max(0, bridge)

        rx += 1152
        frames += 16 if bursting else 18
        sample = {
            "v": 1, "t": int(t * 1000), "seq": seq, "demo": True,
            "hw": {"clk": clk, "baud": 115207, "loopback": "internal",
                   "cyccnt": 1, "budget_cycles": budget, "byte_time_ns": 86800},
            "limits": {"ring": ring, "expect_peak": peak_expect,
                       "burst_ms_x10": 198, "sensor_hz": 180,
                       "sensor_hz_burst": 164},
            "isr": {"last": 38, "min": 38, "mean": 38,
                    "max": 39 + (1 if seq % 37 == 0 else 0), "n": rx},
            "rx": {"cur": 6, "peak": 12, "ovf": 0},
            "bridge": {"cur": bridge, "peak": peak_expect, "ovf": 0},
            "wire": {"rx": rx, "tx_sensor": rx, "tx_bridge": 1024 * int(t),
                     "idle": 0, "stall": 0, "ovr": 0, "ovr_hw": 0},
            "frames": {"ok": frames, "crc": 0, "gaps": 0, "resync": 0,
                       "built": frames + 4},
            "cpu": [{"n": "IDLE", "p": 960}, {"n": "proto", "p": 11},
                    {"n": "sensor", "p": 10}, {"n": "telem", "p": 16},
                    {"n": "Tmr Svc", "p": 0}],
        }
        if seq % 10 == 3:
            trace_seq += 1
            data = []
            for i in range(128):
                ms = i * 1.0416
                if ms < 19.845:
                    data.append(int(peak_expect * ms / 19.845))
                elif ms < 88.9:
                    data.append(max(0, int(peak_expect * (1 - (ms - 19.845) / 69.1))))
                else:
                    data.append(0)
            sample["trace"] = {"seq": trace_seq, "us": 1041, "d": data}

        HUB.publish(sample)
        seq += 1
        time.sleep(0.1)


# ----------------------------------------------------------------------- HTTP

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):        # quieter than the default
        pass

    def _send(self, code, body: bytes, ctype: str, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):                          # noqa: N802
        path = self.path.split("?", 1)[0]

        if path in ("/", "/index.html"):
            try:
                with open(DASHBOARD, "rb") as fh:
                    body = fh.read()
            except OSError:
                body = b"<h1>dashboard/index.html is missing</h1>"
            self._send(200, body, "text/html; charset=utf-8",
                       [("Cache-Control", "no-store")])
            return

        if path == "/api/state":
            body = json.dumps(HUB.snapshot()).encode()
            self._send(200, body, "application/json",
                       [("Cache-Control", "no-store")])
            return

        if path == "/healthz":
            body = b"ok\n" if HUB.connected else b"no telemetry\n"
            self._send(200 if HUB.connected else 503, body, "text/plain")
            return

        if path == "/events":
            self._sse()
            return

        self._send(404, b"not found\n", "text/plain")

    def _sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache, no-transform")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        q = HUB.subscribe()
        try:
            snap = HUB.snapshot()
            self._event("hello", {k: v for k, v in snap.items()
                                  if k != "history"})
            if snap["history"]:
                self._event("history", snap["history"])

            last_ping = time.time()
            while True:
                try:
                    sample = q.get(timeout=5.0)
                    self._event("sample", sample)
                except queue.Empty:
                    pass
                now = time.time()
                if now - last_ping > 10.0:
                    # Keeps phones and proxies from dropping an idle stream,
                    # and tells the page the board went quiet.
                    self.wfile.write(b": ping\n\n")
                    self._event("status", {"connected": HUB.connected,
                                           "source": HUB.source})
                    last_ping = now
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            HUB.unsubscribe(q)

    def _event(self, name: str, payload) -> None:
        body = json.dumps(payload, separators=(",", ":"))
        self.wfile.write(f"event: {name}\ndata: {body}\n\n".encode())
        self.wfile.flush()


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# ---------------------------------------------------------------------- start

def local_addresses() -> list[tuple[str, str]]:
    out = []
    try:
        ts = subprocess.run(["tailscale", "ip", "-4"], capture_output=True,
                            text=True, timeout=3)
        for line in ts.stdout.split():
            if line.strip():
                out.append(("tailscale", line.strip()))
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        out.append(("lan", s.getsockname()[0]))
        s.close()
    except OSError:
        pass
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--serial", help="serial device, default: autodetect")
    ap.add_argument("--log", help="also append every received line here")
    ap.add_argument("--demo", action="store_true",
                    help="no board: synthesise a session")
    ap.add_argument("--replay", help="no board: replay a captured .ndjson")
    args = ap.parse_args()

    if args.demo:
        target = demo_reader, ()
    elif args.replay:
        target = replay_reader, (args.replay,)
    else:
        target = serial_reader, (args.serial, args.log)

    threading.Thread(target=target[0], args=target[1], daemon=True).start()

    srv = Server((args.host, args.port), Handler)
    print(f"R5F protocol simulator dashboard on port {args.port}")
    for kind, addr in local_addresses():
        print(f"    {kind:<10} http://{addr}:{args.port}")
    print(f"    {'local':<10} http://127.0.0.1:{args.port}")
    if args.demo:
        print("    MODE: demo, no board attached")
    print("Ctrl-C to stop", flush=True)

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping", flush=True)


if __name__ == "__main__":
    main()
