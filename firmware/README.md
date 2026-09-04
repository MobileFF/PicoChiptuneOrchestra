# ビルド済みファームウェア (.uf2)

`cmake --build` の出力をそのまま置いたものです。ソースを一切変更していなければ、この場で
BOOTSELドライブにコピーするだけで書き込めます。

| ファイル | 書き込み先 |
|---|---|
| `master.uf2` | Raspberry Pi Pico (マスター) |
| `slave_sn76489.uf2` | RP2040-Zero (SN76489) |
| `slave_ym2612.uf2` | RP2040-Zero (YM2612) |
| `slave_ay8910.uf2` | RP2040-Zero (AY-3-8910) |
| `slave_ym2413.uf2` | RP2040-Zero (YM2413) |
| `slave_ym2151.uf2` | RP2040-Zero (YM2151) |
| `slave_ym2203.uf2` | RP2040-Zero (YM2203) |
| `slave_scc.uf2` | RP2040-Zero (K051649/SCC) |
| `slave_segapcm.uf2` | RP2040-Zero (Sega PCM) |

**注意: このディレクトリは自動更新されません。** `src/master/src/`や`src/slave_*/src/`、
`src/protocol/vgm_spi_protocol.h`などを編集した場合、ここの`.uf2`は古いままになります。
[docs/design-notes.md §7](../docs/design-notes.md)の手順で再ビルドしたあと、
`~/build-vgmplay/src/<ターゲット名>/<ターゲット名>.uf2` をこのディレクトリに上書きコピーして
ください。

書き込み方法: 各Pico/RP2040-ZeroをBOOTSELボタンを押しながらUSB接続するとUF2ドライブとして
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
