# Ölçümler

Bu belge, panelde görünen her sayının nereden geldiğini ve hesapla nasıl
karşılaştırıldığını anlatır. Tablolar gerçek bir oturumdan üretilmiştir,
elle yazılmamıştır:

```sh
python3 scripts/analyze.py logs/session.ndjson
```

> **Zamanlama sayıları hedef donanıma taşınmaz.** Hedef, Zynq UltraScale+
> içindeki kilit adımlı Cortex-R5F; kod orada TCM'den koşacak. Burada ölçülen
> şey, flash önbelleğinin arkasındaki bir Cortex-M33. **Taşınan şey
> `firmware/src/engine/` altındaki kod, protokol motorunun yapısı ve yük
> altındaki tampon davranışıdır.**

## Ölçüm koşulları

| | |
|---|---|
| Kart | Raspberry Pi Pico 2W, RP2350 Cortex-M33 |
| Çekirdek saati | 150,0 MHz — varsayılmadı, `clock_get_hz(clk_sys)` ile okundu |
| Seri hat | UART0, 115200 8N1, FIFO'lar **kapalı** |
| Gerçekleşen baud | **115.207** (nominal 115.200; bölücü tam tutmuyor, +61 ppm) |
| Loopback | PL011 dahili, `UARTCR.LBE` — jumper yok |
| Derleme | `RelWithDebInfo`, `-O2`, GCC 14.2.1, 52 KB flash / 118 KB RAM |
| Çekirdek | FreeRTOS, `RP2350_ARM_NTZ` portu, tek çekirdek |
| Oturum | 106,9 s, 1069 telemetri örneği, 105 CAN patlaması |

**FIFO'ların kapalı olması bir ihmal değil, ölçümün kendisi.** FIFO açık olsaydı
donanım baytları toplar ve RX kesmesi öbek başına bir kez atardı. Biz bayt
başına atmasını istiyoruz, çünkü hedefte 86,8 µs'ye sığması gereken şey tam da
bayt başına koşan yol.

## Ana tablo

### Son teslim tarihi

| Nicelik | Ölçülen | Hesap |
|---|---|---|
| UART RX kesmesi, en iyi | 37 çevrim / 0,247 µs | — |
| UART RX kesmesi, tipik | 37 çevrim / 0,247 µs | — |
| **UART RX kesmesi, en kötü** | **38 çevrim / 0,253 µs** | — |
| Seğirme (en kötü − en iyi) | **1 çevrim** | — |
| Bayt bütçesi | 13.020 çevrim / 86,8 µs | 86,8 µs |
| **En kötü durumda marj** | **%99,71** | — |
| Pay | **343 kat** | — |

### Hat

| Nicelik | Ölçülen | Hesap |
|---|---|---|
| RX kesmesi sayısı | 11.521 /s | 11.520 /s |
| Hatta geçen bayt | 11.521 B/s | 11.520 B/s |
| — sensör payı | 10.497 B/s | 10.496 B/s |
| — köprü payı | 1.024 B/s | 1.024 B/s |
| Sensör çerçevesi, boşta | 180,0 Hz | 180 Hz |
| **Sensör çerçevesi, CAN yükü altında** | **164,3 Hz** | **164 Hz** |

11.521 ile 11.520 arasındaki fark uydurma değil: baud bölücüsü 115.200 yerine
115.207 üretiyor, yani hat nominalden 61 ppm hızlı. Ölçüm bunu görüyor.

Sensörün 180'den 164'e düşmesi bir arıza değil, **tasarımın kendisi**. Hat
aşırı abone: 11.520 B/s sensör + 1.024 B/s CAN, kapasite 11.520 B/s. Hakem
köprüye mutlak öncelik veriyor, bedeli sensör ödüyor — saniyede tam
1024 / 64 = **16 çerçeve**.

### CAN tarafı

| Nicelik | Ölçülen | Hesap |
|---|---|---|
| Patlama | 105 | 105 (saniyede bir) |
| CAN çerçevesi | 15.435 | 15.435 = 105 × 147 |
| Köprü yükü | 107.520 B | 107.520 B = 105 × 1024 |
| Yeniden birleştirilen ISO-TP birimi | 1.680 | 1.680 = 105 × 16 |
| **Birleştirme hatası** | **0** | 0 |

Üçü de **tam** tutuyor, yuvarlama yok. 147 çerçevenin dağılımı:

```
yuva 0            First Frame
yuva 1..144       16 blok × (1 Flow Control + 8 Consecutive Frame)
yuva 145          kapanış Flow Control
yuva 146          blok sonu
                  ────────────────────────────────────────────────
                  128 veri çerçevesi × 8 bayt   = 1024 bayt
                  19 protokol çerçevesi
                  147 × 135 µs                  = 19,845 ms
```

### Tamponlar

| Nicelik | Ölçülen | Hesap |
|---|---|---|
| RX halkası tepe | 12 / 4096 B | ~12 B |
| **Köprü halkası tepe** | **878 B** | 796 B → **798–885 B** |
| Aritmetiğe göre sapma | %+10,3 | %+1,8 … %+9,7 |
| Kalan halka payı | **4,7 kat** | ~5 kat |
| Halka taşması | 0 | 0 |

RX halkasının 12'de kalması bir sonuçtur: protokol görevi hat hızını halkanın
**%0,3**'ü ile karşılıyor. 1 ms'lik yoklama periyodunda hatta 11,5 bayt
birikiyor, görev hepsini alıp gidiyor.

### Hata sayaçları

| | Ölçülen | Hesap |
|---|---|---|
| Sensör CRC hatası | 0 | 0 |
| Sensör çerçeve sayacı boşluğu | 0 | 0 |
| Yeniden senkron | 0 | 0 |
| Köprü birimi CRC hatası | 0 | 0 |
| ISO-TP birleştirme hatası | 0 | 0 |
| RX / köprü halka taşması | 0 / 0 | 0 |
| TX tıkanması | 0 | 0 |
| UART overrun (bayt başına / durum yazmacı) | 0 / 0 | 0 |

1,2 milyondan fazla bayt, tek hata yok. UART overrun'ı iki bağımsız yoldan
sayıyoruz: her okunan baytın veri yazmacındaki bayrağından, ve ayrıca durum
yazmacından. İkincisi, kesmenin hiç koşmadığı bir durumu bile yakalar.

### Talep–cevap

| | Ölçülen | Hesap |
|---|---|---|
| Talep | 1.946 | — |
| Anlık görüntüden cevaplanan | 1.946 | 1.946 |
| Başarısız | 0 | 0 |
| Cevap gecikmesi, ortalama | 529 µs | ~500 µs |
| Cevap gecikmesi, en kötü | 1.167 µs | 1 ms yoklama + zamanlama payı |

Ortalamanın 529 µs çıkması beklenen: talepler rastgele anlarda geliyor, görev
1 ms'de bir yokluyor, yani gecikme [0, 1 ms] aralığında düzgün dağılıyor —
ortalaması yarısı. **Kuyruk bozulmuyor:** cevap, protokol görevinin tuttuğu son
geçerli sensör çerçevesi anlık görüntüsünden veriliyor, hiçbir halka indisi
kıpırdamıyor.

### Görev başına CPU

| Görev | Kullanım |
|---|---|
| IDLE | %94,3 |
| telemetri | %3,3 |
| protokol | %1,1 |
| sensör | %1,0 |
| zamanlayıcı servisi | %0,0 |

Kesmeler bu tabloda görünmez (FreeRTOS onları görev saymaz). RX kesmesinin
yükü ayrıca hesaplanabilir: 11.521 kesme/s × 38 çevrim = 437.798 çevrim/s,
yani 150 MHz'in **%0,29**'u.

---

## İki bulgu

### 1. En kötü durumu kodun ne yaptığı değil, nerede durduğu belirliyor

İlk sürümde kesme gövdesini RAM'e almıştım ama çağırdığı `engine_on_rx_byte`
flash'ta kalmıştı. 690.000 örnek üzerinden:

| | Flash'ta (XIP) | RAM'de (`.time_critical`) |
|---|---|---|
| Tipik | 42 çevrim | 38 çevrim |
| **En kötü** | **152 çevrim** | **39 çevrim** |
| **Seğirme** | **110 çevrim** | **1 çevrim** |
| En kötü marj | %98,83 | %99,70 |
| Pay | 86 kat | 334 kat |

110 çevrimlik fark iş değil, **flash önbellek ıskası**. Kesme her seferinde
aynı işi yapıyor; bazen komutları getirmek için bekliyor.

Bu, hedefe en doğrudan taşınan bulgu. Şartname R5F'te kodun TCM'de koşacağını
söylüyor; **bu tablo o kararın ne kazandırdığının ölçülmüş hâli.** `port.h`
içindeki `PORT_HOT` makrosu bugün RP2350'de SRAM'e, hedefte TCM'e yerleştirir.

### 2. 796 baytlık tahmin %10 içinde doğru, farkın tamamı çerçeveleme

Aritmetik şöyle: 19,845 ms'de 1024 bayt giriyor, UART aynı pencerede 228 bayt
boşaltıyor, geriye **796** kalıyor. Bu hesap, **baytın geldiği anda
çıkabildiğini** varsayıyor. Gerçek hatta iki gecikme var:

1. **Köprü yarım birim gönderemez.** İlk 64 baytlık birim ancak 8 ardışık
   çerçeve geldikten sonra tamamlanıyor: 1 Flow Control + 8 Consecutive Frame
   = 9 yuva = **1,215 ms**.
2. **Hatta akmakta olan sensör çerçevesini kesemez.** Bir birim başladıysa
   bitmeli, yoksa karşı taraf onu yeniden birleştiremez. En kötü bekleme
   63 bayt süresi = **5,47 ms**.

Yani boşaltma, patlamanın 1,215 ms ile 6,683 ms'i arasında başlıyor:

| | |
|---|---|
| Boşaltılabilen bayt | 151 … 214 (228 değil) |
| **Beklenen tepe** | **810 … 873 bayt** |
| İz örnekleme çözünürlüğü | ±12 bayt (1041,6 µs = 12 bayt süresi) |
| **Toplam bant** | **798 … 885 bayt** |
| **Ölçülen** | min 804 / ortalama 840 / max 876 |
| **Bandın içinde** | **105 / 105** |

Aritmetik yanlış değildi — **eksikti**. Ölçüm eksik olanı gösterdi ve fark
tamamen açıklandı. 4096 baytlık halkada hâlâ **4,7 kat** pay var; şartnamedeki
"yaklaşık 5 kat marj" ifadesi ayakta.

Patlama profilinin şekli de tutuyor: tepe ~18,7 ms'de (1024 baytın son
Consecutive Frame'i 19,845 ms'de geliyor), sıfıra dönüş ~92 ms'de
(878 bayt × 86,8 µs = 76 ms boşaltma + patlama süresi).

---

## Ölçüm yönteminin kendisi

**Çevrim sayacı.** Cortex-M33'ün DWT CYCCNT'si, 150 MHz'de 6,67 ns
çözünürlük. Başlatıldıktan sonra gerçekten saydığı **çalışma zamanında
doğrulanıyor** (`hw.cyccnt` alanı) — sessizce ölü bir sayaç, bu projedeki her
zamanlama sayısını sıfıra çevirirdi.

**Ölçüm penceresinin sınırı.** Kesme girişinde ve asıl iş bittikten hemen
sonra zaman damgası alınıyor; **istatistik güncellemesi pencerenin dışında**.
GPIO probu (GP15) ise istatistik güncellemesi dahil tüm kesmeyi kapsıyor.
Böylece osiloskop bir tekrar değil, **bağımsız bir çapraz kontrol** oluyor.

**Ham çevrim gönderiliyor, mikrosaniye değil.** Firmware çevrim sayısını ve
ölçtüğü saat frekansını gönderiyor; bölmeyi host yapıyor. Saat konusunda yanlış
bir varsayım böylece panelde **yanlış saat** olarak görünür, sessizce yanlış
mikrosaniye olarak değil.

**Telemetri ölçümü bozmuyor.** Telemetri görevi sistemin en düşük önceliklisi
ve sayaçları okurken kesmeleri kapatmıyor — tek istisna 64 bitlik toplamların
okunduğu birkaç çevrim. Host dinlemiyorsa satır atılıyor, beklenmiyor.

**Sabitler birbirini denetliyor.** `config.h` içindeki her türetilmiş değer
derleme zamanında doğrulanıyor:

```c
_Static_assert(CAN_BURST_BYTES - UART_DRAIN_PER_BURST == EXPECTED_PEAK_BACKLOG,
               "expected peak backlog is not ingress minus drain");
_Static_assert(BYTE_TIME_NS * CYCLES_PER_US / 1000u == BYTE_TIME_CYCLES,
               "byte tick is not a whole number of cycles");
```

Bir sabit yanlışlıkla değiştirilirse **derleme kırılır**, ölçtüğümüzü
sandığımız şey sessizce değişmez. Ayrıca `port_rp2350.c`, `config.h`'nin saat
sabitini SDK'nın kendi `SYS_CLK_HZ` tanımıyla karşılaştırıyor.

**Zamanlayıcılar tam isabetli.** 86,8 µs × 150 MHz = 13.020 çevrim,
135 µs × 150 MHz = 20.250 çevrim; ikisi de tam sayı ve 16 bit'e sığıyor, bu
yüzden PWM dilimi olarak üretiliyorlar. 1 µs çözünürlüklü donanım alarmı
86 ile 87 µs arasında gidip gelmek zorunda kalırdı.

## Bilinen sapmalar

| Sapma | Büyüklük | Sebep |
|---|---|---|
| Baud 115.207, 115.200 değil | +61 ppm | PL011 bölücüsü tam tutmuyor; bildiriliyor, düzeltilmiyor |
| Patlama periyodu 999,945 ms | −55 ppm | 135 µs bir saniyeyi tam bölmüyor (7407 yuva) |
| İz örnekleme 1041,6 µs | 12 bayt süresi | 12 bayt tikinde bir örnek; yuvarlak 1 ms değil |
| Tepe 878, 796 değil | %+10,3 | Yukarıdaki 2. bulgu; tamamı açıklandı |

## CAN simülatörünün önceliği — bilinçli sapma

Şartname öncelik sırasını `RX ISR > protokol görevi > CAN simülatörü >
telemetri` diye veriyor. Uygulamada CAN simülatörü bir **kesme** (NVIC 0x80),
yani protokol görevinin *üstünde*.

Gerekçe: CAN tarafı **test uyarıcısı**, ölçülen sistemin parçası değil.
Uyguladığı yük tarafından geciktirilebilen bir uyarıcı, ölçümü anlamsız
kılardı — patlama profili artık CAN'in ne gönderdiğini değil, kartın ne kadar
meşgul olduğunu ölçerdi. Yuva başına yaptığı iş çok küçük (135 µs'de ~%0,25
CPU), hiçbir şeyi aç bırakamaz.

## Hedefe ne taşınır

| Taşınır | Taşınmaz |
|---|---|
| `engine/` altındaki kodun tamamı | Çevrim sayıları, mikrosaniyeler |
| Halka tampon boyutlandırması ve tepe davranışı | 150 MHz'e bağlı her şey |
| ISO-TP çerçeve muhasebesi ve birleştirme | RP2350'nin flash önbellek davranışı |
| Kesmenin çekirdek tavanının üstünde durması | NVIC öncelik sayıları |
| Hot path'in hızlı bellekte durması gerektiği | `.time_critical` bölüm adı |
| Tek üretici/tek tüketici halka mantığı | PL011'e özgü her şey |

Hedefte değişecek tek dosya `port/port_rp2350.c`; yerine `port_r5f.c` gelir ve
aynı dokuz fonksiyonu `XUartPs`, bir Triple Timer Counter ve PMU çevrim sayacı
üzerinden gerçekler.
