# PicoChiptuneOrchestra

Raspberry Pi Pico で実現する分散マルチMCU方式のレトロサウンドチップ・エミュレータ。
[VGMPlay](https://github.com/vgmrips/vgmplay) 本家とは無関係の独自実装です。

[VGM_multi_MCU_design.md](VGM_multi_MCU_design.md) の設計(マスター1台+チップごとのスレーブ)を
実装したもの。マスター (Raspberry Pi Pico) がSDカードからVGM/VGZを読み、コマンドをSPIでスレーブ
(チップ1台につき1枚)に配信、各スレーブが担当チップをソフトウェアエミュレートしてPWM出力、
アナログ段でミキシングします。スレーブ基板は基本 RP2040-Zero ですが、**YM2612 / YM2151 /
YM2203 の3チップはRP2040標準クロックだと実機検証でリアルタイム処理が追いつかないことが
確認済み**のため Pico 2 (RP2350) を使います(詳細は下記クイックスタートと
[firmware/pico2/README.md](firmware/pico2/README.md))。Sega PCMはRP2040のままで実機確認済みです。

対応チップ: **SN76489** / **AY-3-8910** / **YM2612**\* / **YM2413** / **YM2151**\* / **YM2203**\* /
**K051649 (SCC)** / **Sega PCM**(サンプルROMは~192KBまで、詳細は[docs/design-notes.md](docs/design-notes.md)参照)。
\* Pico 2 (RP2350) 推奨/必須。

- 回路・配線・BOM: [docs/circuit.md](docs/circuit.md)
- ソフトウェア設計・プロトコル・既知の制約: [docs/design-notes.md](docs/design-notes.md)

## クイックスタート

ビルド済みの`.uf2`が[firmware/](firmware/)にあります(直近のソース状態でビルドしたもの。ただし
自動更新はされないので、ソースを変更した場合は下記手順で再ビルドしてください)。すぐ書き込みたい
だけならビルド手順は読み飛ばして構いません。

```sh
git clone --branch 2.1.1 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init --depth 1 \
    lib/tinyusb lib/mbedtls lib/cyw43-driver lib/lwip lib/btstack lib/cmsis

export PICO_SDK_PATH=~/pico-sdk
cmake -S . -B ~/build-vgmplay
cmake --build ~/build-vgmplay -j$(nproc)
```

これで以下の9つの`.uf2`がビルドツリーの `src/<ターゲット>/` に生成されます
(`~/build-vgmplay/src/master/master.uf2` など)。この既定コマンドは全スレーブを **RP2040**
(通常のPico/RP2040-Zeroと同じCortex-M0+)向けにビルドしますが、下表で※を付けた3チップ
(YM2612/YM2151/YM2203)は実機検証でRP2040標準クロックでは処理が追いつかない
ことが確認済みです(音程が均一に低くなる/テンポが乱れる形で症状化)。この3つは下記の
Pico 2 向け追加ビルドで作った`.uf2`を使ってください(Sega PCMはRP2040のままで問題なく
鳴ることを実機で確認済みなので対象外です):

| ファイル | 書き込み先 |
|---|---|
| `src/master/master.uf2` | Raspberry Pi Pico (マスター) |
| `src/slave_sn76489/slave_sn76489.uf2` | RP2040-Zero (SN76489) |
| `src/slave_ay8910/slave_ay8910.uf2` | RP2040-Zero (AY-3-8910) |
| `src/slave_ym2413/slave_ym2413.uf2` | RP2040-Zero (YM2413) |
| `src/slave_scc/slave_scc.uf2` | RP2040-Zero (K051649/SCC) |
| `src/slave_segapcm/slave_segapcm.uf2` | RP2040-Zero (Sega PCM) |
| `src/slave_ym2612/slave_ym2612.uf2` | **Pico 2 (RP2350)**※ (YM2612) |
| `src/slave_ym2151/slave_ym2151.uf2` | **Pico 2 (RP2350)**※ (YM2151) |
| `src/slave_ym2203/slave_ym2203.uf2` | **Pico 2 (RP2350)**※ (YM2203) |

※ 上の既定ビルドで生成されるのはRP2040版で、書き込んでも一応動きます(RP2040-Zeroに書き込む
選択肢自体は残しています)が、上記の性能不足があるので推奨しません。Pico 2 向けは追加で:

```sh
cmake -S . -B ~/build-vgmplay-pico2 -DPICO_BOARD=pico2
cmake --build ~/build-vgmplay-pico2 --target slave_ym2612 slave_ym2151 slave_ym2203 -j$(nproc)
```

`~/build-vgmplay-pico2/src/<ターゲット>/<ターゲット>.uf2` が生成されます。ビルド済みのものは
[firmware/pico2/](firmware/pico2/) にあります(Sega PCMのPico 2版も参考として置いてありますが、
RP2040版で十分です)。配線・ピン番号はRP2040版と同じ(Pico 2はPicoとピン配置互換)なので、
そのまま載せ替えられます。詳細・オーバークロック設定・RP2040のままで粘る場合の選択肢は
[firmware/pico2/README.md](firmware/pico2/README.md)参照。

各ボードをBOOTSELボタンを押しながらUSB接続し、対応する`.uf2`をUF2ドライブにコピーしてください。
配線は[docs/circuit.md](docs/circuit.md)の通り。8スレーブ全部を組む必要はなく、使わないチップの
スレーブは省略可。どのチップを有効にするか・各チップのCSピン番号は、SDカードルートに
`vgmplay.ini`(ひな形: [firmware/vgmplay.ini](firmware/vgmplay.ini))を置けば再ビルドなしで
変更できます([docs/circuit.md 1.2](docs/circuit.md))。`vgmplay.ini`が無ければ`vgmplay*.ini`の
最初の1つを読むので、カードごとに`vgmplay_scc.ini`のような名前で用意すると分かりやすいです。
既定値は`src/master/src/slave_bus.c`。
編集用のGUIツール: `python3 tools/config_gui/vgmplay_config_gui.py`
([tools/config_gui/](tools/config_gui/)、Python標準ライブラリのみ)。

SDカードのルート直下に`.vgm`/`.vgz`ファイルを置けば、ファイル名順に自動再生し、末尾で最初の曲に
戻ります。`vgmplay.ini`で`enabled = no`にした(またはスレーブ自体を用意していない)チップを使う
曲は、そのパートが欠けたまま鳴らすのではなく**曲ごとスキップ**します。GPIO2(→GND)がスキップ
ボタンです。masterの動作ログは書き込み用USBケーブルをそのまま
挿してシリアルターミナルで見られます(追加配線不要。詳細は[docs/design-notes.md §8](docs/design-notes.md#8-ログの確認方法))。

## リポジトリ構成

ファームウェアのソースは `src/` にまとめてあります。

```
src/
  protocol/         master<->slave 共通プロトコル定義 (ヘッダのみ)
  master/           マスターファームウェア (SDカード読込・VGMパース・SPI配信)
  slave_common/     スレーブ共通基盤 (SPI受信 + サンプル駆動オーディオエンジン + PWM出力)
  slave_sn76489/    SN76489スレーブ (自作エミュレータ)
  slave_ay8910/     AY-3-8910スレーブ (自作エミュレータ)
  slave_ym2612/     YM2612スレーブ (ymfm使用)
  slave_ym2413/     YM2413スレーブ (ymfm使用)
  slave_ym2151/     YM2151スレーブ (ymfm使用)
  slave_ym2203/     YM2203スレーブ (ymfm使用)
  slave_scc/        K051649/SCCスレーブ (自作エミュレータ)
  slave_segapcm/    Sega PCMスレーブ (自作エミュレータ、サンプルROMは実行時アップロード)
docs/               回路図・設計メモ
firmware/           ビルド済み.uf2 + vgmplay.ini ひな形 (詳細はfirmware/README.md)
tools/              ホスト上で動く検証用プログラム、設定GUI、SPI疎通テスト
third_party/        vendoring: ymfm (BSD-3), no-OS-FatFS-SD-SPI-RPi-Pico (Apache-2.0/FatFsライセンス),
                    miniz_tinfl (MIT)
```

## 検証状況

9つのファームウェア(master + 8スレーブ)すべてarm-none-eabi-gccでのクロスビルドを確認済みです。
またマスター側のVGMパース・gzip(VGZ)展開ロジックは、実ソースコードをホスト上でビルドしたテストで
動作確認しています(`tools/host_tests/`)。実機(Pico / RP2040-Zero / Pico 2)でも複数曲・複数
チップ構成で動作確認済みで、その過程で見つかった不具合と対処(SPI転送方式、クロックプリセット、
YM2151ほか3チップのPico 2移行など)は[docs/design-notes.md](docs/design-notes.md)に記録して
います。ただし全チップ・全構成を網羅した検証ではないため、未確認の組み合わせで問題が出る
可能性はあります。

## ライセンス

このプロジェクト自身のコードは [MIT License](LICENSE) です。`third_party/` に同梱している
依存コード(ymfm: BSD-3-Clause、no-OS-FatFS-SD-SPI-RPi-Pico: Apache-2.0/FatFsライセンス、
miniz_tinfl: MIT相当)はそれぞれのディレクトリ内の `LICENSE` に従います。いずれも寛容な
ライセンスで、本プロジェクトの MIT ライセンスと衝突しません。

`tools/offline_render/vendor/`(Nuked-OPM, LGPL-2.1)はホスト側の比較検証専用で、ビルド成果物
(`src/`・`firmware/`)には一切リンクされておらず、リポジトリにも含まれません
(`.gitignore` 参照、[tools/offline_render/vendor/README.md](tools/offline_render/vendor/README.md))。
