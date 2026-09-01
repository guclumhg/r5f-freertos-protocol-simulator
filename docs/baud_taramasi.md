# Baud taraması

Bu belge `scripts/sweep_report.py` tarafından ölçüm kaydından
üretilmiştir; tablolar elle yazılmamıştır.

- Çekirdek saati: **150 MHz**
- Sistemin kendi yükü (hiçbir şey sürülmezken): **%1.4**
- Ölçüm noktası: 47

## Yöntem — iki sütun neden farklı

`yük (ISR)` çevrim sayacıyla, işleyicinin **içinden** ölçülüyor:
kesme/saniye × ortalama ISR süresi ÷ çekirdek saati.

Kayıp, geçen bayt hacmine oranlanıyor: **1000 ppm** altındaki
kayıp kırılma sayılmıyor. Her nokta vericinin tamponunu baştan
başlatmasıyla açılıyor ve alıcı yeniden başlık arıyor; bu geçiş sabit
bir SÜRE aldığı için kaybettiği bayt sayısı baud ile büyüyor
(115200'de 1, 6 Mbaud'da 286) ama **oran olarak sabit**: 160 ppm'i
hiç aşmıyor. Gerçekten tıkanan bir nokta ise ~190.000 ppm kaybediyor.
Eşik, artefaktın üç mertebe üstünde ve arızanın üç mertebe altında.

`yük (gerçek)` boşta kalma döngüsünün kaybolan payından ölçülüyor.
İstisna girişi/çıkışını, FPU tembel yığınlamasını, veri yolu
çekişmesini de içerir — yani işleyicinin içinden **görülemeyen**
her şeyi. Aradaki fark bir hata değil, ölçülmüş bir büyüklüktür.

## Sentetik tarama — işleyici doygunluğu

Aynı alma işi bir zamanlayıcıdan, UART'ın üretebileceğinden
hızlı tetikleniyor. Eksen **bayt başına çekirdek çevrimi**:
saatten bağımsız olduğu için hedef donanıma taşınabilen tek
büyüklük bu.

| çevrim/bayt | eşdeğer hat hızı | kesme/s | ISR ort | ISR maks | p99,9 | yük (ISR) | yük (gerçek) | kayıp bayt | kırıldı |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|
| 1500 | 1,000,000 | 100,000 | 37 | 38 | 38 | %2.5 | **%11.7** | 0 | — |
| 1000 | 1,500,000 | 150,000 | 37 | 38 | 38 | %3.7 | **%17.7** | 0 | — |
| 700 | 2,142,857 | 214,392 | 37 | 38 | 38 | %5.3 | **%25.1** | 0 | — |
| 500 | 3,000,000 | 300,000 | 37 | 38 | 38 | %7.5 | **%35.1** | 0 | — |
| 350 | 4,285,714 | 428,785 | 37 | 38 | 38 | %10.6 | **%49.8** | 0 | — |
| 300 | 5,000,000 | 500,000 | 37 | 38 | 38 | %12.4 | **%58.5** | 0 | — |
| 250 | 6,000,000 | 600,000 | 37 | 38 | 38 | %14.9 | **%70.4** | 0 | — |
| 220 | 6,818,181 | 682,159 | 37 | 38 | 38 | %16.9 | **%79.7** | 0 | — |
| 200 | 7,500,000 | 750,000 | 37 | 38 | 38 | %18.6 | **%87.5** | 0 | — |
| 180 | 8,333,333 | 833,749 | 37 | 38 | 38 | %20.7 | **%97.6** | 0 | — |
| 160 | 9,375,000 | 937,500 | 36 | 38 | 38 | %22.8 | **%100.0** | 455,448 | veri kaybı |
| 150 | 10,000,000 | 1,000,000 | 35 | 38 | 38 | %23.9 | **%100.0** | 872,799 | veri kaybı |
| 140 | 10,714,285 | 1,071,811 | 35 | 38 | 38 | %25.2 | **%100.0** | 1,257,558 | veri kaybı |
| 130 | 11,538,461 | 1,154,068 | 34 | 38 | 36 | %26.2 | **%100.0** | 1,995,904 | veri kaybı |
| 120 | 12,500,000 | 1,250,000 | 34 | 38 | 36 | %28.3 | **%100.0** | 1,995,903 | veri kaybı |
| 90 | 16,666,666 | 1,666,667 | 34 | 38 | 36 | %37.8 | **%100.0** | 1,995,905 | veri kaybı |
| 78 | 19,230,769 | 1,923,077 | 34 | 38 | 36 | %43.6 | **%100.0** | 1,995,905 | veri kaybı |
| 68 | 22,058,823 | 2,207,506 | 34 | 38 | 36 | %50.0 | **%100.0** | 1,995,905 | veri kaybı |
| 60 | 25,000,000 | 2,500,001 | 34 | 38 | 36 | %56.7 | **%100.0** | 1,995,905 | veri kaybı |

### Kırılma noktası

- Dayandığı en düşük değer: **180 çevrim/bayt** (≈ 8,333,333 baud), gerçek yük %97.6
- İlk kırıldığı değer: **160 çevrim/bayt** (≈ 9,375,000 baud), veri kaybı
- Yani sınır **160 ile 180 çevrim/bayt arasında**.

### Bir kesmenin gerçek maliyeti

| çevrim/bayt | ISR gövdesi | toplam (ölçülen) | görünmeyen pay |
|---:|---:|---:|---:|
| 1500 | 37 | 156 | 119 |
| 1000 | 37 | 163 | 126 |
| 700 | 37 | 166 | 129 |
| 500 | 37 | 169 | 132 |
| 350 | 37 | 169 | 132 |
| 300 | 37 | 172 | 135 |
| 250 | 37 | 173 | 136 |
| 220 | 37 | 172 | 135 |
| 200 | 37 | 172 | 135 |
| 180 | 37 | 173 | 136 |

## UART taraması — gerçek baud, üç alma modu

### PER_BYTE

| hedef baud | gerçekleşen | sapma | çevrim/bayt | kesme/s | ISR ort | ISR maks | yük (ISR) | yük (gerçek) | en kötü | kayıp | CRC | kırıldı |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|
| 115,200 | 115,207 | %+0.006 | 13020 | 11,511 | 46 | 46 | %0.4 | %3.0 | %0.4 | 1 | 0 | — |
| 230,400 | 230,414 | %+0.006 | 6510 | 23,022 | 46 | 46 | %0.7 | %4.6 | %0.7 | 1 | 0 | — |
| 460,800 | 460,829 | %+0.006 | 3255 | 46,045 | 46 | 52 | %1.4 | %7.8 | %1.6 | 2 | 0 | — |
| 921,600 | 921,658 | %+0.006 | 1627 | 92,090 | 46 | 52 | %2.8 | %14.2 | %3.2 | 2 | 0 | — |
| 2,000,000 | 2,000,000 | %+0.000 | 750 | 199,925 | 46 | 52 | %6.2 | %29.3 | %6.9 | 59 | 26 | — |
| 4,000,000 | 4,000,000 | %+0.000 | 375 | 399,851 | 46 | 54 | %12.3 | %56.9 | %14.4 | 127 | 50 | — |
| 6,000,000 | 6,000,000 | %+0.000 | 250 | 599,976 | 46 | 54 | %18.5 | %85.1 | %21.6 | 283 | 136 | — |
| 8,000,000 | 8,000,000 | %+0.000 | 187 | 797,842 | 46 | 52 | %24.5 | %100.0 | %27.8 | 452,279 | 16727 | veri kaybı |
| 9,375,000 | 9,375,000 | %+0.000 | 160 | 936,771 | 45 | 52 | %28.1 | %99.9 | %32.5 | 2,822,146 | 0 | veri kaybı |

**Kırılma baud'u: 8,000,000** — veri kaybı

### FIFO_TH

| hedef baud | gerçekleşen | sapma | çevrim/bayt | kesme/s | ISR ort | ISR maks | yük (ISR) | yük (gerçek) | en kötü | kayıp | CRC | kırıldı |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|
| 115,200 | 115,207 | %+0.006 | 13020 | 719 | 685 | 510 | %0.3 | %2.3 | %0.2 | 2 | 0 | — |
| 230,400 | 230,414 | %+0.006 | 6510 | 1,438 | 685 | 510 | %0.7 | %3.2 | %0.5 | 1 | 0 | — |
| 460,800 | 460,829 | %+0.006 | 3255 | 2,880 | 685 | 510 | %1.3 | %4.9 | %1.0 | 1 | 0 | — |
| 921,600 | 921,658 | %+0.006 | 1627 | 5,760 | 685 | 510 | %2.6 | %8.2 | %2.0 | 0 | 0 | — |
| 2,000,000 | 2,000,000 | %+0.000 | 750 | 12,497 | 685 | 510 | %5.7 | %16.1 | %4.2 | 1 | 0 | — |
| 4,000,000 | 4,000,000 | %+0.000 | 375 | 22,222 | 769 | 510 | %11.4 | %30.4 | %8.5 | 2 | 0 | — |
| 6,000,000 | 6,000,000 | %+0.000 | 250 | 31,580 | 811 | 510 | %17.1 | %44.7 | %12.8 | 2 | 0 | — |
| 8,000,000 | 8,000,000 | %+0.000 | 187 | 40,018 | 853 | 510 | %22.8 | %58.9 | %17.0 | 2 | 32 | — |
| 9,375,000 | 9,375,000 | %+0.000 | 160 | 44,701 | 897 | 510 | %26.8 | %68.7 | %19.9 | 2 | 184 | — |

**Bu modda kırılma yok.** En yüksek nokta 9,375,000 baud, gerçek yük %68.7. UART'ın tavanı burada; daha yükseği donanımdan çıkmıyor.

### DMA_RX

| hedef baud | gerçekleşen | sapma | çevrim/bayt | kesme/s | ISR ort | ISR maks | yük (ISR) | yük (gerçek) | en kötü | kayıp | CRC | kırıldı |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|
| 115,200 | 115,207 | %+0.006 | 13020 | 44 | 39 | 38 | %0.0 | %1.8 | %0.0 | 0 | 1 | — |
| 230,400 | 230,414 | %+0.006 | 6510 | 90 | 39 | 38 | %0.0 | %2.4 | %0.0 | 0 | 2 | — |
| 460,800 | 460,829 | %+0.006 | 3255 | 180 | 39 | 38 | %0.0 | %3.4 | %0.0 | 0 | 2 | — |
| 921,600 | 921,658 | %+0.006 | 1627 | 360 | 39 | 38 | %0.0 | %5.2 | %0.0 | 0 | 1 | — |
| 2,000,000 | 2,000,000 | %+0.000 | 750 | 781 | 39 | 38 | %0.0 | %9.5 | %0.0 | 0 | 2 | — |
| 4,000,000 | 4,000,000 | %+0.000 | 375 | 1,562 | 39 | 38 | %0.0 | %16.9 | %0.0 | 0 | 3336 | — |
| 6,000,000 | 6,000,000 | %+0.000 | 250 | 2,343 | 39 | 38 | %0.1 | %25.5 | %0.1 | 0 | 1 | — |
| 8,000,000 | 8,000,000 | %+0.000 | 187 | 3,125 | 39 | 40 | %0.1 | %33.5 | %0.1 | 0 | 2 | — |
| 9,375,000 | 9,375,000 | %+0.000 | 160 | 3,662 | 39 | 38 | %0.1 | %38.9 | %0.1 | 0 | 326 | — |

**Bu modda kırılma yok.** En yüksek nokta 9,375,000 baud, gerçek yük %38.9. UART'ın tavanı burada; daha yükseği donanımdan çıkmıyor.

## Sabit

PL011 16× örnekler, yani en yüksek baud `clk_peri/16` ve bir bayt
**hiçbir koşulda 160 çekirdek çevriminden sık gelemez**. Saati
yükseltmek baud tavanını da çekirdek hızını da aynı oranda büyütür,
oran değişmez. Bu yüzden UART taraması tek başına kırılma noktasını
bulamaz ve sentetik tarama gerekir.

