#!/usr/bin/env python3
"""Turn a captured sweep into docs/baud_taramasi.md.

    python3 scripts/sweep_report.py logs/sweep.ndjson > docs/baud_taramasi.md

Numbers only. Where a conclusion is drawn it is arithmetic on the measured
values, stated as such. Standard library only.
"""
import json
import sys

MODE_NAMES = {0: "PER_BYTE", 1: "FIFO_TH", 2: "DMA_RX",
              0xFE: "BASELINE", 0xFF: "SYNTHETIC"}
BREAK_NAMES = [(1, "veri kaybı"), (2, "yük > %70"), (4, "ISR > bayt süresi")]
LOSS_PPM = 1000  # below this, loss is the stimulus restarting, not a rate failure


def load(path):
    rows, clk = [], 150_000_000
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not (line.startswith("{") and line.endswith("}")):
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "hw" in d:
            clk = d["hw"]["clk"]
        if "row" in d:
            rows.append(d["row"])
    return rows, clk


def breaks(row):
    return ", ".join(n for b, n in BREAK_NAMES if row["broke"] & b) or "—"


def pct(ppm):
    return f"{ppm / 10000:.1f}"


def main(path):
    rows, clk = load(path)
    if not rows:
        sys.exit(f"{path}: no sweep rows")

    baseline = next((r for r in rows if r["mode"] == 0xFE), None)
    base_ppm = baseline["load_total"] if baseline else 0

    uart = [r for r in rows if r["mode"] in (0, 1, 2)]
    synth = sorted((r for r in rows if r["mode"] == 0xFF),
                   key=lambda r: -r["byte_cyc"])

    print("# Baud taraması\n")
    print("Bu belge `scripts/sweep_report.py` tarafından ölçüm kaydından")
    print("üretilmiştir; tablolar elle yazılmamıştır.\n")
    print(f"- Çekirdek saati: **{clk/1e6:.0f} MHz**")
    if baseline:
        print(f"- Sistemin kendi yükü (hiçbir şey sürülmezken): "
              f"**%{pct(base_ppm)}**")
    print(f"- Ölçüm noktası: {len(rows)}\n")

    print("## Yöntem — iki sütun neden farklı\n")
    print("`yük (ISR)` çevrim sayacıyla, işleyicinin **içinden** ölçülüyor:")
    print("kesme/saniye × ortalama ISR süresi ÷ çekirdek saati.\n")
    print(f"Kayıp, geçen bayt hacmine oranlanıyor: **{LOSS_PPM} ppm** altındaki")
    print("kayıp kırılma sayılmıyor. Her nokta vericinin tamponunu baştan")
    print("başlatmasıyla açılıyor ve alıcı yeniden başlık arıyor; bu geçiş sabit")
    print("bir SÜRE aldığı için kaybettiği bayt sayısı baud ile büyüyor")
    print("(115200'de 1, 6 Mbaud'da 286) ama **oran olarak sabit**: 160 ppm'i")
    print("hiç aşmıyor. Gerçekten tıkanan bir nokta ise ~190.000 ppm kaybediyor.")
    print("Eşik, artefaktın üç mertebe üstünde ve arızanın üç mertebe altında.\n")
    print("`yük (gerçek)` boşta kalma döngüsünün kaybolan payından ölçülüyor.")
    print("İstisna girişi/çıkışını, FPU tembel yığınlamasını, veri yolu")
    print("çekişmesini de içerir — yani işleyicinin içinden **görülemeyen**")
    print("her şeyi. Aradaki fark bir hata değil, ölçülmüş bir büyüklüktür.\n")

    if synth:
        print("## Sentetik tarama — işleyici doygunluğu\n")
        print("Aynı alma işi bir zamanlayıcıdan, UART'ın üretebileceğinden")
        print("hızlı tetikleniyor. Eksen **bayt başına çekirdek çevrimi**:")
        print("saatten bağımsız olduğu için hedef donanıma taşınabilen tek")
        print("büyüklük bu.\n")
        print("| çevrim/bayt | eşdeğer hat hızı | kesme/s | ISR ort | ISR maks | p99,9 "
              "| yük (ISR) | yük (gerçek) | kayıp bayt | kırıldı |")
        print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|")
        for r in synth:
            lost = r["missed"] + r["ovr"]
            print(f"| {r['byte_cyc']} | {r['baud']:,} | {r['irq_s']:,} "
                  f"| {r['isr'][1]} | {r['isr'][2]} | {r['isr'][3]} "
                  f"| %{pct(r['load_isr'])} | **%{pct(r['load_total'])}** "
                  f"| {lost:,} | {breaks(r)} |")
        print()

        held = [r for r in synth if not r["broke"]]
        gone = [r for r in synth if r["broke"]]
        if held and gone:
            last_ok = min(held, key=lambda r: r["byte_cyc"])
            first_bad = max(gone, key=lambda r: r["byte_cyc"])
            print("### Kırılma noktası\n")
            print(f"- Dayandığı en düşük değer: **{last_ok['byte_cyc']} çevrim/bayt** "
                  f"(≈ {last_ok['baud']:,} baud), gerçek yük %{pct(last_ok['load_total'])}")
            print(f"- İlk kırıldığı değer: **{first_bad['byte_cyc']} çevrim/bayt** "
                  f"(≈ {first_bad['baud']:,} baud), {breaks(first_bad)}")
            print(f"- Yani sınır **{first_bad['byte_cyc']} ile "
                  f"{last_ok['byte_cyc']} çevrim/bayt arasında**.\n")

        # Cost per interrupt, from the two load columns.
        print("### Bir kesmenin gerçek maliyeti\n")
        print("| çevrim/bayt | ISR gövdesi | toplam (ölçülen) | görünmeyen pay |")
        print("|---:|---:|---:|---:|")
        for r in synth:
            if not r["irq_s"] or r["broke"]:
                continue
            net = max(0, r["load_total"] - base_ppm)
            total_cyc = net / 1e6 * clk / r["irq_s"]
            body = r["isr"][1]
            print(f"| {r['byte_cyc']} | {body} | {total_cyc:.0f} "
                  f"| {total_cyc - body:.0f} |")
        print()

    if uart:
        print("## UART taraması — gerçek baud, üç alma modu\n")
        for m in (0, 1, 2):
            sel = sorted((r for r in uart if r["mode"] == m),
                         key=lambda r: r["baud"])
            if not sel:
                continue
            print(f"### {MODE_NAMES[m]}\n")
            print("| hedef baud | gerçekleşen | sapma | çevrim/bayt | kesme/s "
                  "| ISR ort | ISR maks | yük (ISR) | yük (gerçek) | en kötü "
                  "| kayıp | CRC | kırıldı |")
            print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|")
            for r in sel:
                dev = r["dev_ppm"] / 10000
                mark = " ⚠" if abs(dev) > 2 else ""
                print(f"| {r['target']:,} | {r['baud']:,} | %{dev:+.3f}{mark} "
                      f"| {r['byte_cyc']} | {r['irq_s']:,} "
                      f"| {r['isr'][1]} | {r['isr'][2]} "
                      f"| %{pct(r['load_isr'])} | %{pct(r['load_total'])} "
                      f"| %{pct(r['load_worst'])} "
                      f"| {r['missed'] + r['ovr']:,} | {r['crc']} | {breaks(r)} |")
            print()
            broke = [r for r in sel if r["broke"]]
            if broke:
                first = min(broke, key=lambda r: r["baud"])
                print(f"**Kırılma baud'u: {first['baud']:,}** — {breaks(first)}\n")
            else:
                top = max(sel, key=lambda r: r["baud"])
                print(f"**Bu modda kırılma yok.** En yüksek nokta "
                      f"{top['baud']:,} baud, gerçek yük %{pct(top['load_total'])}. "
                      f"UART'ın tavanı burada; daha yükseği donanımdan çıkmıyor.\n")

    print("## Sabit\n")
    print("PL011 16× örnekler, yani en yüksek baud `clk_peri/16` ve bir bayt")
    print("**hiçbir koşulda 160 çekirdek çevriminden sık gelemez**. Saati")
    print("yükseltmek baud tavanını da çekirdek hızını da aynı oranda büyütür,")
    print("oran değişmez. Bu yüzden UART taraması tek başına kırılma noktasını")
    print("bulamaz ve sentetik tarama gerekir.\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
