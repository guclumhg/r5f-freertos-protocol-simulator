#!/usr/bin/env python3
"""Turn a captured telemetry log into the measured-versus-calculated table.

    python3 scripts/analyze.py logs/session.ndjson

Reads newline delimited JSON as emitted by the firmware and prints every
number this project claims to measure next to the number the specification
predicts. Standard library only.
"""
import json
import sys
from statistics import mean

# The predictions. These are the given constants, not fitted to the data.
LINE_RATE_BPS = 11520              # 115200 baud, 8N1
FRAME_LEN = 69                     # 2 header + 1 counter + 64 data + 2 crc
# The sensor stream is continuous, so its rate is the line divided by the
# frame size rather than a setting. Neither comes out whole.
SENSOR_HZ_NOMINAL = LINE_RATE_BPS / FRAME_LEN            # 166.96
EXPECTED_PEAK_BACKLOG = 796        # 1024 ingress minus 228 drained
RING_BYTES = 4096
CAN_FRAMES_PER_BURST = 147
CAN_BYTES_PER_BURST = 1024
SENSOR_HZ_UNDER_BURST = (LINE_RATE_BPS - CAN_BYTES_PER_BURST) / FRAME_LEN  # 152.12

# The 796 figure assumes the drain starts the instant the burst does. The
# bridge sends its bytes unframed, so there is no unit to assemble first and
# nothing delays it except one thing: it can only take the line at a sensor
# frame boundary, because a frame cut in half could not be reassembled at the
# far end. Worst case it has just missed one and waits 68 byte times.
#
# So 796 is the ideal case and also the lower edge of the band.
BURST_US = 19845.0
BYTE_US = 86.8
DELAY_MIN_US = 0.0
DELAY_MAX_US = (FRAME_LEN - 1) * BYTE_US            # 5902 us
PEAK_MIN = CAN_BYTES_PER_BURST - int((BURST_US - DELAY_MIN_US) / BYTE_US)
PEAK_MAX = CAN_BYTES_PER_BURST - int((BURST_US - DELAY_MAX_US) / BYTE_US)

# The burst trace samples every 12 byte ticks, so an observed peak can sit up
# to twelve bytes either side of the true one.
TRACE_RESOLUTION_B = 12


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
            a, b = a.get(k, 0), b.get(k, 0)
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

    traces = [r["trace"] for r in rows if "trace" in r and r["trace"].get("d")]
    has_can = last.get("can", {}).get("bursts", 0) > 0

    print(f"file            : {path}")
    print(f"samples         : {len(rows)} over {span_s:.1f} s")
    print(f"clock           : {clk/1e6:.1f} MHz (read from hardware)")
    print(f"achieved baud   : {first['hw']['baud']} (nominal 115200)")
    print(f"loopback        : {first['hw']['loopback']}")
    print(f"cycle counter   : {'live' if first['hw']['cyccnt'] else 'DEAD - timings invalid'}")
    print()

    W = 78
    print("=" * W)
    print(f"{'quantity':<36}{'measured':>20}{'expected':>22}")
    print("=" * W)

    def row(name, measured, expected):
        print(f"{name:<36}{measured:>20}{expected:>22}")

    print("-- the deadline " + "-" * (W - 16))
    row("UART RX interrupt, best", f"{isr_min} cyc / {us(isr_min):.3f} us", "-")
    row("UART RX interrupt, mean", f"{isr_mean:.0f} cyc / {us(isr_mean):.3f} us", "-")
    row("UART RX interrupt, WORST", f"{isr_max} cyc / {us(isr_max):.3f} us", "-")
    row("jitter (worst - best)", f"{isr_max - isr_min} cyc", "-")
    row("byte budget", f"{budget} cyc / {us(budget):.1f} us", "86.8 us")
    row("worst-case margin", f"{100*(1-isr_max/budget):.2f} %", "-")
    row("worst-case headroom", f"{budget/max(isr_max,1):.0f}x", "-")

    print("-- the wire " + "-" * (W - 12))
    row("RX interrupts", f"{interrupts} ({interrupts/span_s:.0f}/s)", f"{LINE_RATE_BPS}/s")
    row("bytes received", f"{bytes_rx} ({bytes_rx/span_s:.0f} B/s)", f"{LINE_RATE_BPS} B/s")
    row("  of which sensor", f"{delta('wire','tx_sensor')}", "-")
    row("  of which bridge", f"{delta('wire','tx_bridge')}",
        f"{CAN_BYTES_PER_BURST}/s")
    row("sensor frames verified", f"{frames_ok} ({frames_ok/span_s:.2f} Hz)",
        f"{SENSOR_HZ_UNDER_BURST:.2f} Hz under load" if has_can
        else f"{SENSOR_HZ_NOMINAL:.2f} Hz idle")

    if has_can:
        print("-- the CAN side " + "-" * (W - 16))
        bursts = delta("can", "bursts")
        row("bursts", str(bursts), f"{span_s:.0f} (one per second)")
        row("CAN frames", f"{delta('can','frames')}",
            f"{bursts * CAN_FRAMES_PER_BURST} ({bursts} x {CAN_FRAMES_PER_BURST})")
        row("bridge payload bytes", f"{delta('can','data')}",
            f"{bursts * CAN_BYTES_PER_BURST} ({bursts} x {CAN_BYTES_PER_BURST})")
        row("bursts reassembled at the far end", f"{delta('can','reassembled')}",
            f"{bursts} (all of them)")

    print("-- the buffers " + "-" * (W - 15))
    row("RX ring peak", f"{rx_peak} of {RING_BYTES} B", "~12 B")
    row("bridge ring peak", f"{bridge_peak} of {RING_BYTES} B",
        f"{PEAK_MIN}..{PEAK_MAX} B")
    if bridge_peak:
        err = 100 * (bridge_peak - EXPECTED_PEAK_BACKLOG) / EXPECTED_PEAK_BACKLOG
        row("  vs the 796 arithmetic", f"{err:+.1f} %",
            f"+{100*(PEAK_MIN-796)/796:.1f} .. +{100*(PEAK_MAX-796)/796:.1f} %")
        row("  remaining ring margin", f"{RING_BYTES/bridge_peak:.1f}x", "~5x")

    print("-- errors, all should be zero " + "-" * (W - 30))
    for label, value in [
        ("sensor CRC errors", delta("frames", "crc")),
        ("sensor counter gaps", delta("frames", "gaps")),
        ("resyncs", delta("frames", "resync")),
        ("payload content errors", delta("frames", "payload")),
        ("ISO-TP reassembly errors", delta("can", "isotp_err")),
        ("RX ring overflow", last["rx"]["ovf"]),
        ("bridge ring overflow", last["bridge"]["ovf"]),
        ("transmit stalls", delta("wire", "stall")),
        ("UART overrun, per byte", delta("wire", "ovr")),
        ("UART overrun, status reg", last["wire"]["ovr_hw"]),
    ]:
        row(label, str(value), "0")

    if "req" in last:
        print("-- request / response " + "-" * (W - 22))
        row("requests", str(delta("req", "sent")), "-")
        row("answered from snapshot", str(delta("req", "answered")),
            str(delta("req", "sent")))
        row("failed", str(delta("req", "failed")), "0")
        answered = delta("req", "answered")
        row("payload bytes handed over", str(delta("req", "bytes")),
            f"{answered * 64} ({answered} x 64)")
        row("answer latency, mean",
            f"{us(last['req']['lat_mean']):.0f} us", "~500 us (1 ms poll)")
        # One poll period plus however long the task waits to be scheduled,
        # so a little over a tick rather than exactly a tick.
        row("answer latency, worst",
            f"{us(last['req']['lat_max']):.0f} us", "1 ms poll + jitter")
    print("=" * W)
    print()

    print("CPU usage, last sample:")
    for t in sorted(last.get("cpu", []), key=lambda x: -x["p"]):
        print(f"    {t['n']:<10} {t['p']/10:5.1f} %")
    print()

    if traces:
        peaks = [max(t["d"]) for t in traces]
        print(f"burst profiles captured : {len(traces)}")
        print(f"    sample period       : {traces[0]['us']} us x {len(traces[0]['d'])} samples")
        print(f"    peak backlog        : min {min(peaks)} / mean {mean(peaks):.0f} / max {max(peaks)} B")
        print(f"    arithmetic          : {EXPECTED_PEAK_BACKLOG} B (drain starts instantly)")
        print(f"    with frame atomicity: {PEAK_MIN}..{PEAK_MAX} B")
        lo, hi = PEAK_MIN - TRACE_RESOLUTION_B, PEAK_MAX + TRACE_RESOLUTION_B
        print(f"    plus trace sampling : {lo}..{hi} B")
        inside = sum(1 for p in peaks if lo <= p <= hi)
        print(f"    inside that band    : {inside}/{len(peaks)}")

        # where the last trace crosses back down through zero
        d = traces[-1]["d"]
        us_per = traces[-1]["us"]
        pk = d.index(max(d))
        drained = next((i for i in range(pk, len(d)) if d[i] == 0), None)
        print(f"    last burst: peak {max(d)} B at {pk*us_per/1000:.1f} ms"
              + (f", drained by {drained*us_per/1000:.1f} ms" if drained else ""))
    else:
        print("burst profiles captured : none")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
