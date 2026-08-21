# Pengujian lapangan TriageBox — protokol dan angka untuk laporan

Dokumen ini untuk menghasilkan angka yang bisa dipertanggungjawabkan di PKP2:
jarak, latensi, kapasitas node, dan daya tahan baterai. Bukan estimasi — prosedur
pengukuran, plus angka mana yang sudah bisa dihitung sekarang dan angka mana yang
**wajib** diukur.

Aturan yang dipakai di seluruh dokumen: setiap angka di slide harus punya metode.
Angka tanpa metode akan ditanya asalnya, dan "hasil perhitungan" bukan jawaban
kalau yang ditanya jarak.

## Yang sudah bisa dihitung sekarang

Jalankan di repo station — tidak butuh hardware, tidak butuh ESP-IDF:

```
tools/run_selftests.sh
```

Keluarannya tiga hal: kontrak payload JSON, anggaran radio (airtime, slot, duty
cycle, link budget per SF dan per jumlah node), dan anggaran baterai.

Semua rumus radio dari datasheet SX1276/78 §4.1.1 dan §6.4, dan tiga angka airtime
yang dipakai di `docs/lora-air-protocol.md` di-assert oleh tool itu — jadi kalau
dokumen dan rumus berbeda, tool-nya gagal build.

## 1. Uji jarak

### Yang diukur

Bukan "sampai berapa meter masih nyambung". Itu satu titik yang berpindah tiap
kali diulang. Yang diukur:

| Besaran | Dari mana | Kenapa ini |
| --- | --- | --- |
| **PDR** (packet delivery ratio) | `scripts/field-test.js` | angka utama. Di bawah ~90% link tidak layak untuk triase |
| RSSI rata-rata & terburuk | node status MQTT | seberapa dekat ke batas |
| SNR rata-rata & terburuk | node status MQTT | jarak ke demodulator floor SF7 = −7,5 dB |
| Gap `packet_counter` | `scripts/field-test.js` | membedakan link marginal dari node mati |

PDR harus dibagi dengan **jumlah yang seharusnya datang**, bukan dengan jumlah
yang datang. Satu poll per node per 15 detik, jadi 5 menit = 20 paket per node.
Menghitung hanya yang muncul selalu menghasilkan 100%.

### Prosedur

Di laptop yang menjalankan broker:

```bash
cd triagebox-backend
npm run field-test -- --label "25m LOS" --minutes 5
npm run field-test -- --label "50m LOS" --minutes 5
npm run field-test -- --label "100m LOS" --minutes 5
npm run field-test -- --label "50m 1 dinding" --minutes 5
```

Tiap run menambah baris ke `/tmp/triagebox-fieldtest.csv` — langsung bisa
di-import ke Excel untuk grafik. Pola titik ukur yang cukup untuk satu grafik:

- **Line of sight**: 10, 25, 50, 100, 150, 200 m sampai PDR jatuh di bawah 90%
- **Terhalang**: 25 dan 50 m dengan 1 dinding, lalu 2 dinding
- **Tinggi antena**: 50 m dengan antena station di 1 m dan di 3 m — ini biasanya
  perubahan terbesar dan paling murah di lapangan

Antena node harus **vertikal dan tidak menempel badan**. Antena 433 MHz λ/4 itu
~17 cm; tubuh manusia menyerap kuat di band ini, jadi node di kantong akan
mengukur penyerapan, bukan jarak.

### Bentuk grafik untuk slide

Sumbu X jarak (m), dua sumbu Y: PDR (%) garis penuh, RSSI (dBm) garis putus. Titik
di mana PDR menembus 90% adalah **jarak operasional** — itu angka yang dikutip,
bukan jarak maksimum di mana satu paket pernah lolos.

## 2. Uji latensi

Latensi harus dipecah di batas radio, kalau tidak siklus LoRa (0–15 detik) akan
menelan bagian yang dikontrol software (milidetik) dan tidak ada yang tahu bagian
mana yang perlu diperbaiki.

| Tahap | Besaran | Sumber |
| --- | --- | --- |
| Sensor → paket siap | 1,28 s (blok PPG) sampai 4 s (jendela ECG) | firmware node |
| Node menunggu poll | **0–15 s, rata-rata 7,5 s** | `LORA_POLL_PERIOD_MS` |
| Transmisi LoRa | 82 ms | dihitung, di-assert |
| Station → MQTT → REST | **ukur** | `scripts/field-test.js` kolom `pipeline_ms` |
| Socket.IO → dashboard | **ukur** | `scripts/ws-listen.js` |

Yang perlu diukur hanya dua baris terakhir. Sisanya properti protokol.

Untuk slide, sebutkan **dua** angka dan jangan dijumlahkan jadi satu:

- **Latensi pipeline** (MQTT → tampil): puluhan milidetik. Ini yang dikontrol.
- **Latensi end-to-end**: didominasi siklus poll, rata-rata 7,5 s, maksimum 15 s.

Batas 15 detik itu **lantai yang tidak bisa dihilangkan** oleh pekerjaan backend
apa pun, karena node tidak pernah memancar tanpa diminta — keputusan sadar supaya
paket tak diminta tidak menabrak balasan node lain. Sebut alasannya di slide;
kalau tidak, 15 detik terlihat seperti kelambatan, padahal itu harga determinisme.

Kalau 15 detik terlalu lambat untuk kasus RED, opsinya menurunkan
`LORA_POLL_PERIOD_MS` — dan biayanya terlihat di tabel duty cycle: periode 15→10 s
menaikkan duty station dari 4,1% ke 6,2% di SF7.

## 3. Kapasitas node

Sudah dihitung, dan jawabannya tergantung SF karena slot budget = periode/N:

| Node | SF maks | Siklus | TX duty | Jarak relatif |
| --- | --- | --- | --- | --- |
| 1 | SF12 | 3,0 s | 5,5% | ×2,61 |
| 2 | SF11 | 3,4 s | 6,6% | ×2,15 |
| 5 | SF10 | 4,5 s | 8,3% | ×1,78 |
| 10 | SF9 | 5,5 s | 8,3% | ×1,47 |
| 20 | SF8 | 7,2 s | 8,3% | ×1,21 |

Di SF7 yang dipakai sekarang, **20 node adalah batas praktis** dan siklusnya 5,4 s
dari periode 15 s — masih 64% idle. Batas kerasnya `LORA_POLL_NODE_MAX` = 20 dan
alamat node 1 byte, jadi menaikkan ke 40 node adalah perubahan array plus periode
yang lebih panjang, bukan perubahan protokol.

Untuk slide: "20 node per station, 15 s per siklus, 36% okupansi kanal" — dan
angka okupansi itu yang menunjukkan sistemnya belum jenuh.

## 4. Uji baterai

### Yang harus diukur, bukan dihitung

`tools/battery_budget.c` menghitung dua cara dan **hasilnya berbeda 2,1×**:

- Diturunkan dari klaim 11 jam pada 2× 18650: beban ~546 mA
- Dijumlahkan dari datasheet tiap rail: ~266 mA

Salah satu dari tiga hal ini benar, dan hanya multimeter yang bisa memutuskan:
bebannya memang ~546 mA dan estimasi ini kelewat rendah; klaim 11 jam terlalu
konservatif; atau 11 jam belum pernah diukur.

**Jangan taruh salah satu tabel itu di slide.** Ukur dulu.

### Prosedur

Alat: USB power meter inline (yang murah cukup) atau multimeter mode arus DC seri
dengan baterai. Yang dicari **arus rata-rata**, bukan puncak.

1. Node menyala normal: LCD hidup di kecerahan yang akan dipakai di lapangan, PPG
   aktif dengan jari terpasang, LoRa menjawab poll, satu scan RFID per menit.
2. Catat arus tiap 30 detik selama 10 menit, lalu rata-ratakan. Kalau alatnya bisa
   mAh terakumulasi, itu lebih baik — bagi dengan durasinya.
3. Ulangi dengan **backlight mati**. Selisihnya biasanya kejutan terbesar.
4. Runtime = kapasitas × 0,85 ÷ arus rata-rata. Faktor 0,85 karena LiPo habis di
   ~3,0 V di bawah beban, proteksi memutus lebih awal, dan regulator berhenti
   menahan 3,3 V sebelum sel benar-benar kosong.

Uji tuntas (kalau ada waktu): nyalakan sampai mati sendiri, catat jamnya. Satu
angka terukur mengalahkan tabel perhitungan mana pun.

### Yang sudah pasti dari perhitungan

Ini tidak tergantung estimasi beban, karena runtime skala dengan **watt-hour**:

| Pack | Energi | Relatif |
| --- | --- | --- |
| 2× 18650 3500 mAh (proposal) | 25,9 Wh | 100% |
| 1× LiPo 3000 mAh | 11,1 Wh | **43%** |
| 1× LiPo 4000 mAh | 14,8 Wh | 57% |
| 1× LiPo 5000 mAh | 18,5 Wh | 71% |

**Pindah dari 2× 18650 ke satu LiPo 3000 mAh memotong energi jadi kurang dari
setengah.** Kalau 11 jam itu nyata, jadi 4,7 jam. 5000 mAh mengembalikannya ke
~7,9 jam. Tidak ada sel tunggal di daftar itu yang mencapai 11 jam pada beban yang
sama — butuh ~7000 mAh.

Jadi kalau klaim 11 jam harus dipertahankan: **pilih 5000 mAh dan turunkan beban**,
atau ubah klaimnya. Menurunkan beban bukan soal radio — LoRa hanya 12 mA dari
estimasi 266 mA (4%). Yang menghabiskan baterai:

| Perubahan | Hemat | Runtime di 5000 mAh |
| --- | --- | --- |
| Backlight mati saat idle | 60 mA | 18,6 jam |
| ESP32 light-sleep antar blok PPG 1,28 s | 80 mA | 20,5 jam |
| LED MAX30102 hanya saat ada jari | 1 mA | 14,4 jam |

Dua yang pertama masing-masing lebih besar daripada seluruh konsumsi radio.
Menaikkan atau menurunkan daya pancar LoRa **bukan keputusan baterai** di board
ini.

Station tidak perlu dihitung: PoE dan adaptor 5 V, jadi tidak ada anggaran
baterai — dan itu justru alasan station boleh lebih "boros" di radio daripada node.

## 5. Uji dua station

Kalau di PKP2 ada dua station, ini yang wajib diuji dan wajib disebut.

Dua station **tidak boleh** berbagi frekuensi. Sudah dihitung di
`tools/lora_budget.c`:

- Jendela rawan station A per slot: 268 ms (poll 31 ms + menunggu 237 ms)
- Transmisi station B per periode: 20 poll + 20 balasan
- Perkiraan tumpang tindih per slot: 0,87
- **Peluang satu slot rusak: 58%**

Jadi berbagi 433 MHz antara dua station 20-node kehilangan sekitar separuh
bacaan, dan kehilangannya **tak terlihat** — frame yang bertabrakan gagal CRC dan
dihitung sebagai miss, identik dengan node mati.

Ada masalah kedua yang lebih berbahaya dari kehilangan data: `lora_poll_for_me()`
tidak memeriksa `station_id`, jadi node menjawab poll dari station mana pun, dan
kedua station mem-poll alamat radio mulai dari 1. Station B akan mempublish node
milik station A di bawah rentang ID-nya sendiri — **vital satu pasien tercatat di
rekam pasien lain.**

Solusinya dua kanal: **433 dan 434 MHz** (regulasi Indonesia mengalokasikan
433,05–434,79 MHz untuk SRD izin kelas — konfirmasi ke sumber Komdigi sebelum
dipakai untuk keputusan akhir). Ganti sync word **tidak** menyelesaikan: filter
sync word bekerja setelah demodulasi, jadi ia memperbaiki atribusi tapi tidak
tabrakan.

Untuk slide, ini justru bahan bagus: menunjukkan batas sistem diketahui dan
terukur, bukan ditemukan saat demo.

## 6. Tabel ringkas untuk slide

Isi kolom "terukur" setelah pengujian; kolom "dihitung" sudah bisa dipakai
sekarang dengan catatan metodenya.

| Parameter | Dihitung | Terukur | Metode |
| --- | --- | --- | --- |
| Jarak operasional (PDR ≥ 90%) | — | ___ m | `field-test.js`, LOS |
| RSSI di jarak operasional | — | ___ dBm | node status MQTT |
| Sensitivitas penerima | −124,5 dBm | — | datasheet SF7/BW125 |
| Link budget | 152,5 dB | — | 17 dBm + 8 dBi + 3 dBi |
| Latensi end-to-end | 0–15 s (rata-rata 7,5 s) | ___ s | periode poll |
| Latensi pipeline (MQTT→UI) | — | ___ ms | `field-test.js` |
| Node per station | 20 | ___ | `LORA_POLL_NODE_MAX` |
| Okupansi kanal, 20 node | 36% | — | siklus 5,4 s / periode 15 s |
| Duty cycle TX station | 4,1% | — | 20 × 31 ms / 15 s |
| Duty cycle TX node | 0,55% | — | 82 ms / 15 s |
| Daya pancar | 17 dBm | ___ | `RegPaDac` di nilai reset |
| Konsumsi node | ~266 mA (estimasi) | ___ mA | multimeter, 10 menit |
| Daya tahan | tergantung ukuran di atas | ___ jam | ukur sampai mati |

## 7. Kesalahan yang paling mudah terjadi

- **Menguji di bench 2 node lalu mengklaim untuk 20.** Slot budget = periode/N,
  jadi hasilnya tidak bisa dipindah. Uji pada jumlah node yang akan diklaim.
- **Mengutip jarak maksimum, bukan jarak operasional.** Satu paket lolos di 300 m
  bukan berarti sistem bekerja di 300 m.
- **PDR dihitung dari yang datang.** Selalu 100%. Pembaginya harus dari cadence.
- **Antena node di kantong atau menempel badan.** Menyerap kuat di 433 MHz;
  hasilnya mengukur tubuh, bukan link.
- **Menjumlahkan latensi radio dan pipeline jadi satu angka.** Menyembunyikan
  bagian mana yang bisa diperbaiki.
- **Mengutip 11 jam tanpa mengukur ulang setelah pindah pack.** Energinya turun
  ke 43%.
