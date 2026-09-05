# Pico 1 (RP2040) 向けビルド

通常の Raspberry Pi Pico / RP2040-Zero(RP2040、標準クロック)向けの`.uf2`です。

| ファイル | 書き込み先 |
|---|---|
| `master.uf2` | Raspberry Pi Pico (マスター) |
| `slave_sn76489.uf2` | RP2040-Zero (SN76489) |
| `slave_ay8910.uf2` | RP2040-Zero (AY-3-8910) |
| `slave_scc.uf2` | RP2040-Zero (K051649/SCC) |
| `slave_segapcm.uf2` | RP2040-Zero (Sega PCM) |

マスターと、この4チップのスレーブはRP2040標準クロックで実機確認済みです。**YM2612 / YM2151 /
YM2203 / YM2413 のスレーブはここには置いていません** — 実機検証でRP2040標準クロックでは
リアルタイム処理が追いつかない(音程が均一に低くなる/テンポが乱れる)ことが確認済みのため、
[../pico2/](../pico2/) の Pico 2 (RP2350) 版を使ってください。詳細は
[../pico2/README.md](../pico2/README.md) と [docs/design-notes.md §5](../../docs/design-notes.md)。

それでもRP2040でこの4チップを試したい場合(オーバークロックする等)は、ソースから
`-DPICO_BOARD=pico`(既定)で再ビルドすれば作れます。ビルド済みは同梱していません。
