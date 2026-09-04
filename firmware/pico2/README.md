# Pico 2 (RP2350) 向けビルド

RP2040(genuine PicoやRP2040-Zero)標準クロック(133MHz)では、YM2151のリアルタイム演算が
実測で2.32倍遅れる(`RATE CHECK`ログで確認、`../debug/README.md`参照)ことが判明しました。
ymfmのOPM(YM2151)には精度を落として軽くする設定(YM2203にある`OPN_FIDELITY_MIN`のようなもの)が
存在せず、ビルドも`-O3`最適化済みだったため、これはソフトウェア側で追い込める限界でした。

Pico 2(RP2350)はクロックが高く(標準150MHz)、コア(Cortex-M33)もRP2040のCortex-M0+より
1命令あたりの処理効率が高いため、この不足を解消できる可能性があります。**ここにあるのは
YM2151/YM2203/YM2612(design-notes.mdでCPU負荷が高いと指摘していた3チップ)+ Sega PCM
(2026-08-28、音程等の不具合報告を受けて追加。ただしSega PCMのネイティブレートは約31.25kHzと
YM2151よりずっと低く、単純なテーブル読み出し中心の処理なので、本当にCPU負荷が原因かは
`debug/slave_segapcm_VERBOSE.uf2`の`RATE CHECK`で個別に確認してください。CPU起因でなければ
このPico 2化・オーバークロックだけでは直らず、クロックプリセットやSPIのCS間隔
(`src/master/src/slave_bus.c`)側を疑う必要があります)を、Pico 2のRP2350向けに再ビルドしたものです**。
他のチップ(SN76489/AY-3-8910/YM2413/SCC)はRP2040標準クロックで十分間に合っているため、Pico 2版は
用意していません(`../`にあるRP2040版をそのまま使ってください)。

## 書き込み方法

配線・ピン番号はRP2040版と同じです(Pico 2はPico 1とピン配置・フォームファクタ互換)。
Pico 2をBOOTSELボタンを押しながらUSB接続してUF2ドライブとしてマウントし、対応する`.uf2`を
コピーしてください。

## RP2040版(`../`)は削除していません

RP2040側にはまだ`set_sys_clock_khz()`によるオーバークロックという選択肢が残っています
(design-notes.md §5参照、200MHz超で動作する個体が多いという報告あり)。Pico 2で十分な性能が
出るか実測してから、どちらの方針を採るか判断してください。

## オーバークロック設定(2026-08-31時点)

`src/slave_common/include/slave_overclock.h` が全 RP2350 重量スレーブ共通で
`set_sys_clock_khz()` +(必要なら)`vreg_set_voltage()` +
**`clk_peri` を PLL_USB 由来の 48MHz に固定**します。最後の1点が重要で、これをしないと
オーバークロック時に PL022 SPI まで 300MHz 超で駆動され、**SPI スレーブがフレームをデコード
できなくなって無音になります**(core1 は動き続けるので `RATE CHECK` は出続ける)。

| チップ | 既定クロック / 電圧 | 根拠 |
|---|---|---|
| **YM2203** | **320 MHz / 1.15 V** | 166kHz 描画は全チップ中最重。実機確認済み(250/300 では下ずる) |
| YM2151 / YM2612 / SegaPCM | 250 MHz / 1.10 V | YM2151 は実機で 250MHz 十分。他は同等以下 |

CMake 変数: `VGM_YM2203_SYSCLK_KHZ`(既定320000)/`VGM_YM2203_VREG_MV`(既定1150)は YM2203 専用、
`VGM_RP2350_SYSCLK_KHZ`(既定250000)/`VGM_RP2350_VREG_MV`(既定0=1.10V)は他3チップ用。
電圧を上げるほど発熱するので安定する最小値で。SDK の安全上限は 1.30V(`VREG_VOLTAGE_1_30`)。

## `debug/*_VERBOSE.uf2` / `debug/slave_ym2203_350MHz_1v20.uf2`

`VGM_SLAVE_VERBOSE_LOG=ON` の `RATE CHECK` 計測ログ付きビルドです。Pico 2 実機で実際に
間に合っているか確認するために使ってください(printf 自体が数%の計測誤差を足す点に注意)。
使い方は `../debug/README.md` 参照。

- `slave_ym2151_250MHz_VERBOSE.uf2` / `slave_ym2612_250MHz_VERBOSE.uf2` / `slave_segapcm_VERBOSE.uf2`
  … 既定設定での計測用
- `slave_ym2612_300MHz_VERBOSE.uf2` … YM2612 も下ずる疑いが出たとき用(250 と比較)
- `slave_ym2203_320MHz_1v15_VERBOSE.uf2` … YM2203 の既定設定での計測用
- `slave_ym2203_350MHz_1v20.uf2` … YM2203 が 320/1.15V でもまだ僅かに低いとき用(要 1.20V)

## YM2151で実際にあった対処の流れ(参考)

Pico 2への移行だけでは終わらず、以下の順で追い込みました(`docs/design-notes.md` §5参照):

1. Pico 2標準クロック(150MHz)→まだ1.257倍不足
2. 200MHzオーバークロック→ほぼ間に合うが僅かに不足。かつこのタイミングでCS間隔(`gap_us`)
   不足によるSPIバイト欠落も新たに発覚(AY-3-8910の80µsでもこの基板では足りず、120µsで解消)
3. 250MHzオーバークロック→実測・実際の視聴とも問題解消

Sega PCMでも同様に、オーバークロックだけで直らない場合はCS間隔のチューニングも疑ってください。

## ビルド方法

このディレクトリの中身は、通常のビルドに`-DPICO_BOARD=pico2`を追加しただけです(ソースコードは
RP2040版と共通):

```sh
export PICO_SDK_PATH=~/pico-sdk
cmake -S . -B ~/build-vgmplay-pico2 -DPICO_BOARD=pico2
cmake --build ~/build-vgmplay-pico2 --target slave_ym2151 slave_ym2203 slave_ym2612 slave_segapcm
```

`VGM_SLAVE_VERBOSE_LOG=ON`を追加すれば`debug/`向けのビルドに、
`-DVGM_RP2350_SYSCLK_KHZ=300000`等を追加すればオーバークロック値を変えたビルドになります
(既定250000。RP2040版には影響しません)。
