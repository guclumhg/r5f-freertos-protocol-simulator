#!/usr/bin/env python3
"""Show how a sweep progressed through time, to find where it stalls."""
import json
import sys

MODE = {0: "PER_BYTE", 1: "FIFO_TH", 2: "DMA_RX", 0xFE: "BASELINE", 0xFF: "SYNTH"}

rows, marks, last = [], [], None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    line = line.strip()
    if not (line.startswith("{") and line.endswith("}")):
        continue
    try:
        d = json.loads(line)
    except json.JSONDecodeError:
        continue
    sw = d.get("sweep")
    if sw:
        key = (sw["point"], sw["mode"], sw["baud"])
        if key != last:
            marks.append((d["t"], sw))
            last = key
    if "row" in d:
        rows.append((d["t"], d["row"]))

print(f"{len(marks)} durum degisimi, {len(rows)} sonuc\n")
prev_t = None
for t, sw in marks:
    gap = f"{(t - prev_t)/1000:6.1f}s" if prev_t is not None else "     -"
    prev_t = t
    print(f"  t={t/1000:8.1f}s  +{gap}  nokta {sw['point']:>2}/{sw['total']:>2} "
          f"mod={MODE.get(sw['mode'], sw['mode']):<9} baud={sw['baud']:,}")

print()
for t, r in rows:
    print(f"  t={t/1000:8.1f}s  {MODE.get(r['mode'], r['mode']):<9} "
          f"baud={r['baud']:>9,} irq/s={r['irq_s']:>9,} "
          f"yukTOP=%{r['load_total']/1e4:5.1f} kayip={r['missed'] + r['ovr']:,}")
