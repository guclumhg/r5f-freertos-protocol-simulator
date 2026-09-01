#!/usr/bin/env python3
"""Turn a captured telemetry log into the measured-versus-calculated table.

    python3 scripts/analyze.py logs/session.ndjson

Reads newline delimited JSON as emitted by the firmware, and prints every
number this project claims to measure next to the number the specification
predicts. Standard library only.
"""
import json
import sys
from statistics import mean

# The predictions. These are the given constants, not fitted to the data.
LINE_RATE_BPS = 11520          # 64 byte frames at 180 Hz
SENSOR_HZ_NOMINAL = 180
SENSOR_HZ_UNDER_BURST = 164    # 180 minus 1024/64
EXPECTED_PEAK_BACKLOG = 796    # 1024 ingress minus 228 drained
RING_BYTES = 4096
CAN_BURST_MS = 19.845


def load(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return rows


def main(path):
    rows = load(path)
    if len(rows) < 2:
        sys.exit(f"{path}: need at least two telemetry samples, got {len(rows)}")

    first, last = rows[0], rows[-1]
    clk = first["hw"]["clk"]
    budget = first["hw"]["budget_cycles"]
    span_s = (last["t"] - first["t"]) / 1000.0

    def us(cycles):
        return cycles / clk * 1e6

    def delta(*keys):
        a, b = first, last
        for k in keys:
            a, b = a[k], b[k]
        return b - a

    isr_max = max(r["isr"]["max"] for r in rows)
    isr_mins = [r["isr"]["min"] for r in rows if r["isr"]["min"]]
    isr_min = min(isr_mins) if isr_mins else 0
    isr_mean = mean(r["isr"]["mean"] for r in rows if r["isr"]["mean"])
    interrupts = delta("isr", "n")

    bytes_rx = delta("wire", "rx")
    frames_ok = delta("frames", "ok")
    rx_peak = max(r["rx"]["peak"] for r in rows)
    bridge_peak = max(r["bridge"]["peak"] for r in rows)

    traces = [r["trace"] for r in rows if "trace" in r]

    print(f"file            : {path}")
    print(f"samples         : {len(rows)} over {span_s:.1f} s")
    print(f"clock           : {clk/1e6:.1f} MHz (read from hardware)")
    print(f"achieved baud   : {first['hw']['baud']} (nominal 115200)")
    print(f"loopback        : {first['hw']['loopback']}")
    print(f"cycle counter   : {'live' if first['hw']['cyccnt'] else 'DEAD - timings invalid'}")
    print()

    print("=" * 72)
    print(f"{'quantity':<34}{'measured':>18}{'expected':>18}")
    print("=" * 72)

    def row(name, measured, expected):
        print(f"{name:<34}{measured:>18}{expected:>18}")

    row("UART RX interrupt, best", f"{isr_min} cyc / {us(isr_min):.3f} us", "-")
    row("UART RX interrupt, mean", f"{isr_mean:.0f} cyc / {us(isr_mean):.3f} us", "-")
    row("UART RX interrupt, WORST", f"{isr_max} cyc / {us(isr_max):.3f} us", "-")
    row("byte budget", f"{budget} cyc / {us(budget):.1f} us", "86.8 us")
    row("worst-case margin", f"{100*(1-isr_max/budget):.2f} %", "-")
    row("worst-case headroom", f"{budget/max(isr_max,1):.0f}x", "-")
    print("-" * 72)
    row("RX interrupts", f"{interrupts} ({interrupts/span_s:.0f}/s)",
        f"{LINE_RATE_BPS}/s")
    row("bytes received", f"{bytes_rx} ({bytes_rx/span_s:.0f} B/s)",
        f"{LINE_RATE_BPS} B/s")
    row("frames verified", f"{frames_ok} ({frames_ok/span_s:.1f} Hz)",
        f"{SENSOR_HZ_NOMINAL} Hz idle / {SENSOR_HZ_UNDER_BURST} Hz burst")
    print("-" * 72)
    row("RX ring peak", f"{rx_peak} of {RING_BYTES} B", "~12 B")
    row("bridge ring peak", f"{bridge_peak} of {RING_BYTES} B",
        f"{EXPECTED_PEAK_BACKLOG} B")
    if bridge_peak:
        err = 100 * (bridge_peak - EXPECTED_PEAK_BACKLOG) / EXPECTED_PEAK_BACKLOG
        row("bridge peak vs prediction", f"{err:+.1f} %", "0 %")
    print("-" * 72)
    row("ring overflows", f"rx={last['rx']['ovf']} bridge={last['bridge']['ovf']}", "0")
    row("CRC errors", str(delta("frames", "crc")), "0")
    row("frame counter gaps", str(delta("frames", "gaps")), "0")
    row("resyncs", str(delta("frames", "resync")), "0")
    row("transmit stalls", str(delta("wire", "stall")), "0")
    row("UART overrun (per byte / RSR)",
        f"{delta('wire','ovr')} / {last['wire']['ovr_hw']}", "0")
    print("=" * 72)
    print()

    print("CPU usage, last sample (per mille):")
    for t in last.get("cpu", []):
        print(f"    {t['n']:<10} {t['p']/10:5.1f} %")
    print()

    if traces:
        print(f"burst traces captured: {len(traces)}")
        peaks = [max(t["d"]) for t in traces if t["d"]]
        if peaks:
            print(f"    sample period : {traces[0]['us']} us")
            print(f"    peak backlog  : min {min(peaks)} / mean {mean(peaks):.0f} / max {max(peaks)} B")
            print(f"    predicted     : {EXPECTED_PEAK_BACKLOG} B")
            print(f"    error         : {100*(mean(peaks)-EXPECTED_PEAK_BACKLOG)/EXPECTED_PEAK_BACKLOG:+.1f} %")
    else:
        print("burst traces captured: none (CAN simulator idle in this build)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
