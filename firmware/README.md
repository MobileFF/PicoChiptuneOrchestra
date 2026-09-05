# ビルド済みファームウェア (.uf2)

`cmake --build` の出力をそのまま置いたものです。ソースを一切変更していなければ、この場で
BOOTSELドライブにコピーするだけで書き込めます。

ボードの種類でフォルダを分けています:

- **[pico1/](pico1/)** — 通常の Raspberry Pi Pico / RP2040-Zero(RP2040)向け。マスターと、
  RP2040標準クロックで実機確認済みのスレーブ(SN76489, AY-3-8910, K051649/SCC, Sega PCM)。
- **[pico2/](pico2/)** — Pico 2(RP2350)向け。RP2040では実機検証でリアルタイム処理が追いつかない
  ことが確認されたスレーブ(YM2612, YM2151, YM2203, YM2413)はこちらのみに置いています。
  RP2040版のビルド自体は残っているので、オーバークロックして試したい場合はソースから
  `-DPICO_BOARD=pico` (既定)で再ビルドしてください(推奨はしません、詳細は
  [pico2/README.md](pico2/README.md))。

対応表と詳細は[README.md クイックスタート](../README.md#クイックスタート)参照。

**注意: このディレクトリは自動更新されません。** `src/master/src/`や`src/slave_*/src/`、
`src/protocol/vgm_spi_protocol.h`などを編集した場合、ここの`.uf2`は古いままになります。
[docs/design-notes.md §7](../docs/design-notes.md)の手順で再ビルドしたあと、
`~/build-vgmplay/src/<ターゲット名>/<ターゲット名>.uf2` を該当フォルダに上書きコピーして
ください。

書き込み方法: 各Pico/Pico2/RP2040-ZeroをBOOTSELボタンを押しながらUSB接続するとUF2ドライブとして
マウントされるので、対応する`.uf2`をコピーするだけです。

**`vgmplay.ini`**: チップごとの有効/無効とCSピンのGPIO番号を再ビルドなしで変えるための設定
ファイルのひな形です。SDカードのルート(`.vgm`/`.vgz`と同じ場所)にコピーして編集してください。
無くても既定値で動きます。`vgmplay.ini`が無ければ`vgmplay*.ini`(例: `vgmplay_scc.ini`)の
最初の1つを読みます。書式は[docs/circuit.md 1.2](../docs/circuit.md)参照。GUIエディタ:
`python3 tools/config_gui/vgmplay_config_gui.py`。

**ログについて**: すべて`VGM_SLAVE_VERBOSE_LOG=OFF`(デフォルト)でビルドしています。書き込み用
USBケーブルをそのまま挿してシリアルターミナルを開けば、起動バナー・RESET/MUTEイベントなどの
軽量なログは常に出ます。スレーブが受け取ったレジスタ書き込みを1件ずつ見たい場合は、配線確認用に
`-DVGM_SLAVE_VERBOSE_LOG=ON`で該当スレーブだけ別途ビルドしてください(音声のリアルタイム性が
崩れるため、確認が終わったらここにある通常版に書き戻すことを推奨します)。詳細は
[docs/design-notes.md §8](../docs/design-notes.md#8-ログの確認方法)参照。

**[debug/](debug/)**: 個別の不具合調査用ビルド(RP2040版・Pico2版が混在)。詳細は
[debug/README.md](debug/README.md)参照。
