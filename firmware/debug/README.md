# デバッグ用ビルド

## 既知の問題と対処 (解決済み)

「SN76489スレーブに繋いだが音が出ない」問題を実機で調査した結果、原因は**このリポジトリの
開発・検証に使ったRP2040(Pico)同士のマスター/スレーブ組み合わせで、CSをlowに保ったまま
3バイト連続でSPI送信すると、1バイト目しかスレーブに届かず2・3バイト目が何のエラーもなく
静かに欠落する**という現象でした。切り分けには本ディレクトリの`spi_bringup_test_*.uf2`
(下記)を使い、原因を特定した上で対処済みです。

**対処**: `master/src/slave_bus.c`の`send_frame()`を、3バイトを一括でCS-lowのまま送るのではなく
**1バイトごとにCSをlow→write→high**させる方式に変更しました(パルス間隔は実機テストで20µsに
調整済み。50µsだと安全だがテンポの乱れが出るほど遅く、5µsだとバイト欠落が再発する)。
`firmware/master.uf2`・`firmware/debug/master_VERBOSE.uf2`とも既にこの対処を含んだビルドです。
プロトコルレベルの詳細は[docs/design-notes.md §2](../../docs/design-notes.md#2-通信プロトコル)、
コードのコメントは`master/src/slave_bus.c`の`send_frame()`参照。

この対処により、Sega PCMのROMアップロード(1バイトずつ送る仕様)が従来の想定より低速になった点に
注意してください([docs/design-notes.md §5](../../docs/design-notes.md#5-既知の簡略化精度に関する注記)参照)。

## `slave_ym2151_VERBOSE.uf2` / `slave_ym2413_VERBOSE.uf2` (VGM_SLAVE_VERBOSE_LOG=ON、レート実測ログ付き)

YM2151/YM2413で「音程が(均一に)低く聞こえる」問題の調査用です。クロックプリセット選択の修正
(2026-08-27)後も改善しなかったため、**RP2040がこのチップのネイティブサンプルレート(YM2151で
約5.6万〜6.3万Hz、YM2413で約5万Hz)でのリアルタイム演算に本当に追いついているか**を実測する
ログを`slave_common/src/slave_engine.c`に追加しました。約1秒(サンプルレート分のサンプル)ごとに
以下の形式で出ます:

```
[YM2151] RATE CHECK: 55930 samples in 1050000us (target 1000000us @ 55930 Hz), fell_behind=812/55930 ticks
```

`elapsed_us`が`target_us`より明らかに大きい、または`fell_behind`が多い場合、CPU側の演算が
追いついていないことを示します(この場合、実時間に対して波形がゆっくり進む=音程が低く聞こえる、
という今回の症状と一致します)。改善策としては`docs/design-notes.md`§5にある
`set_sys_clock_khz()`でのオーバークロックが候補です。

## `spi_bringup_test_master.uf2` / `spi_bringup_test_slave.uf2`

VGM解析・チップエミュレーション・マルチコア連携などを全部取り除いた、**SPI疎通確認だけ**を行う
最小構成のペアです。ピン配置は本体スレーブと合わせてあり(マスター: SCK=GPIO10, MOSI=GPIO11,
CS=GPIO12 / スレーブ: SPI1 で SCK=GPIO10, MOSI/RX=GPIO8, CS=GPIO9)、RP2040-Zeroのピンヘッダで
配線できます。ソースは
`tools/spi_bringup_test/`(本体プロジェクトから独立したミニマムなCMakeプロジェクト)。
上記の原因究明に使ったツールで、`master.uf2`と同じく1バイトごとにCSをパルスする方式**で
実装済み**です(修正前の挙動を再現したい場合はgit履歴の`tools/spi_bringup_test/master/src/main.c`
旧版を参照してください)。

マスターは0.5秒ごとに`{0x1_, 0x2_, 0x3_}`(下位ニブルがカウンタ、上位ニブルがバイト位置)という
パターンを送り続け、自分が送った内容をログに出します。スレーブは受信した3バイトをそのままログに
出すだけです。両方書き込んで、マスターの`SEND ..`とスレーブの`RECV# ..`を見比べてください:

- **一致する(スレーブのRECVがSENDと同じ値、カウンタが変化する)** → SPI自体は正常。VGMPlay本体側
  (`master.uf2`/各スレーブ)で別の問題を疑ってください
- **一致しない/2・3バイト目が固定値になる** → 今回発見したのと同種のSPI疎通問題の可能性が高い

### バーストモード検証版 (`_BURST_cs20` / `_BURST_cs21` / `_slave_pico2`)

`vgmplay.ini` の `gap = 0`(CS を 3 バイト保持したまま連送、約 8 倍速。OutRun のような
FM 密度の高い曲で YM2151 が追いつくために必要)を、そのスレーブ基板のリンクが確実に
クロックできるかを事前確認するための版です。

- `spi_bringup_test_master_BURST_cs20.uf2` / `_cs21.uf2` — マスター(RP2040)用。CS を
  3 バイト分保持して一括送信する。`cs20` は本番配線の YM2151 CS=GPIO20、`cs21` は
  YM2151 を YM2203 基板に載せ替えて CS=21 にした構成用。
- `spi_bringup_test_master_cs20.uf2` — 1 バイトごと CS パルスの基準版(比較用)。
- `spi_bringup_test_slave_pico2.uf2` — **スレーブを RP2350 / Pico2 でビルドした版**。
  YM2151 スレーブ基板は Pico2 なので、RP2040 版 `spi_bringup_test_slave.uf2` は書けない。
  こちらを使う。

手順: YM2151 の Pico2 基板に `spi_bringup_test_slave_pico2.uf2`、マスター Pico に
配線に合った `_BURST_cs2x.uf2` を書き、`SEND` と `RECV#` を見比べる。

- **一致する(3 バイトとも正しくカウンタも変化)** → この基板は連送を取りこぼさない →
  `vgmplay.ini` の `[ym2151]` に `gap = 0` を入れて本番運用してよい。
- **2・3 バイト目が固定/化ける** → 連送は不可。`gap` は非ゼロのまま。

**2026-09-02 の結果: モード0(現行)のバーストは失敗**(`調査用/spi_bring_up_test1.log`)。
スレーブは各バーストの 1 バイト目しか取り込めなかった。PL022 のモード0スレーブは
バイト間に CS エッジが要る。→ 下の MODE1 版で再検証。

### SPI モード1 版 (`_BURST_MODE1_cs21` / `_slave_MODE1_pico2`)

CPHA=1 の PL022 スレーブは **CS 保持のまま連続バーストを受けられる**(モード0の失敗を回避)。
4 MHz のまま 1 フレーム約 6µs になり、FM 密度の高い曲でもマスターが追いつく。8 枚
フラッシュのフラグデー前にこれで連送の可否を確認する。

- `spi_bringup_test_master_BURST_MODE1_cs21.uf2` — マスター(RP2040)。BURST + SPI モード1、
  CS=GPIO21。
- `spi_bringup_test_slave_MODE1_pico2.uf2` — スレーブを **Pico2 + SPI モード1**でビルドした版。

手順: YM2151 の Pico2 基板に `_slave_MODE1_pico2.uf2`、マスターに `_BURST_MODE1_cs21.uf2` を
書き、`SEND` と `RECV#` を比較。

- **3 バイトとも一致・カウンタも変化** → モード1バースト OK → 本番コードを 2 行変更
  (`slave_spi_rx.c` の `SPI_CPHA_0`→`SPI_CPHA_1`、`slave_bus.c` に同じ `spi_set_format`)、
  master + 全 8 スレーブを再ビルド・再フラッシュ、`[ym2151] gap = 0`。
- **化ける** → モード1でも不可。option (b) 4 バイト同期化へ。

## `slave_ym2151_USBDIAG_pico2.uf2` (YM2151 スレーブ・USB 診断)

「曲全体が約2半音フラット」= master の `RESET` フレーム取りこぼしでスレーブが起動時
プリセット(0 = 55930Hz)のまま固定、という仮説を USB シリアルで確認するための Pico2 版。

通常ファームとの違い:
- 起動時に **USB-CDC 端末の接続を最大5秒待つ**( boot バナーと最初の RESET 行を取りこぼさない)
- `sys clock = … Hz` を表示(オーバークロックが効いているか)
- `VGM_SLAVE_RATE_CHECK` を ON でビルド → `slave_engine` が毎秒
  `[YM2151] RATE CHECK: … @ Y Hz …` を出す

読みかた:
- `@ 62500 Hz` → master の `RESET(preset=1)` は届いている。フラットの原因は別
- `@ 55930 Hz` → RESET 未達。`slave_bus.c` の 3 回送出でも届いていない(CS 配線 / スレーブ長時間起動待ち)
- `@ 62500 Hz` だが `… samples in <target をはるかに超える>us` → コアが描画レートに追いつけていない(これもフラットの原因)
- `[YM2151] RESET preset=X (Y Hz)` 行(無条件)も一緒に見えるはず

ビルド: `-DPICO_BOARD=pico2 -DVGM_SLAVE_RATE_CHECK=ON -DVGM_YM2151_USB_DIAG=ON`
(build dir `~/dev/build-vgmplay-pico2-diag`)。`VGM_YM2151_USB_DIAG` 未定義なら `main.c` は
通常と同一(`#ifdef` ガード)。

**それでも USB に何も出ない場合**は ChromeOS が実行中 Pico2 の CDC を Linux(Crostini)に
渡していない。設定 → About ChromeOS → デベロッパー → Linux → USB デバイス で当該デバイスを
Linux 側に有効化、または ChromeOS 側のシリアル端末(Chrome の Serial Terminal / Web Serial)で読む。

### 起動順序を保証する診断ペア (`master_BOOTWAIT10.uf2` + 5秒待ち版 USBDIAG)

「RESET が届かない」のがタイミング(スレーブが曲頭の <1ms の RESET バーストの時に
まだ SPI を聞いていない)なのか、フレーム自体の転送不良なのかを切り分けるペア:

- `master_BOOTWAIT10.uf2` — master を `-DVGM_MASTER_BOOT_DELAY_S=10` でビルド。起動後
  10 秒カウントダウンしてから SD マウント→再生(＝RESET 送出)。
- `slave_ym2151_USBDIAG_pico2.uf2` — `-DVGM_YM2151_BOOT_DELAY_MS=5000` 込み。起動後
  5 秒待ってから core1/SPI-RX 開始。

master(10s) > スレーブ(5s) なので、master が RESET を送る時点でスレーブは確実に
SPI を聞いている。**2026-09-03 の結果**: RESET x3 到達・`reset_rx=3`・`@ 62500 Hz` =
原因は起動順序で確定。かつ 250MHz では `fell_behind` が 79〜20910/62500 と大きく
描画が間に合っていないことも判明。

→ 恒久対策を投入済み(フラグデー、全ファーム再フラッシュ):
- `VGMSPI_OP_CLOCK`(チップ非リセットでレートだけ再設定)を追加。master が曲の1秒ごとに
  再送 → 遅れて起動したスレーブも次の1秒で自動矯正
- YM2151 スレーブを 320MHz/1.15V へ(YM2203 と同じ)

`slave_ym2151_USBDIAG_nowait_pico2.uf2` = 起動待ち無しの診断版。通常順序で電源投入し、
曲頭から 約1秒後に `[YM2151] CLOCK preset=1 (62500 Hz) -- late correction` と
`RATE CHECK … @ 62500 Hz`(fell_behind 小)が出れば OP_CLOCK 自己矯正＋320MHz が効いている。

`VGM_MASTER_BOOT_DELAY_S` / `VGM_YM2151_BOOT_DELAY_MS` は未指定なら 0(通常動作)。

## `oled_test.uf2` (SSD1306 OLED 単体表示テスト)

ステータス表示用OLEDが点灯しない場合に、**ハード(配線・電源・プルアップ・パネル不良・
コントローラ違い)かソフト(`master/src/oled_ui.c`・core1描画ループ・初期化順序)か**を
切り分けるための、本体から完全に独立したテストです。SD・SPIスレーブ・マルチコア・
`vgm_player`は一切リンクせず、I2C0とOLEDだけを使います。ソースは
`tools/oled_test/`(独立したミニマムなCMakeプロジェクト)。

配線は本体と同一(`docs/circuit.md` §1): OLED VCC→3V3 / GND→GND / SDA→GPIO0 / SCL→GPIO1。

**マスターのPico**に書き込み、USB CDCシリアルを開いて**ログと画面の両方**を見ます。以下を
無限ループします: I2Cバス復旧 → I2C全アドレススキャン(素の基板なら`0x3C`が1つだけ出る)→
`0x3C`/`0x3D`でSSD1306初期化 → 表示パターン(`0xA5`全点灯=RAM経由せず, 反転, 全消灯, 全点灯,
枠, 市松, 縞, フォント/文字列, コントラストスイープ)。

判定:

- **スキャンで何も出ない/初期化失敗** → ハード。電源、SDA/SCLの入れ違い、GPIO0/1への結線、
  プルアップ(無ければSDA/SCL→3V3に4.7k)。ケーブルを短く、または
  `-DOLED_TEST_SLOW_I2C=1`(100kHz)で再ビルド
- **スキャンで`0x3C`は出るが「init: NO ACK」** → ほぼハード。コマンド経路の接触不良か、
  SSD1306ではない(SH1106等はinitと列オフセットが別物)
- **初期化OKだが画面は真っ暗(特に`0xA5`全点灯でも)** → ハード。チャージポンプ/パネルVCC。
  基板によっては`0x8D,0x14`ではなく`0x8D,0x10`(外部VCC)が必要。あるいはパネル不良
- **初期化OKで全パターンが正しく表示される** → ハードは正常。本体で真っ暗なのはソフト側
  (`master/src/oled_ui.c`のcore1起動/`panel_up`ステートマシン、`master/src/main.c`の
  初期化順序、bring-up時のバス競合)を疑う

ビルド:
```sh
PICO_SDK_PATH=~/dev/pico-sdk cmake -S tools/oled_test -B ~/dev/build-oled-test
cmake --build ~/dev/build-oled-test -j4   # -> ~/dev/build-oled-test/oled_test.uf2
```
詳細は`tools/oled_test/README.md`。

## `slave_sn76489_VERBOSE.uf2` (VGM_SLAVE_VERBOSE_LOG=ON)

受信した全SPIフレーム(`WRITE0`など)を1件ずつログに出すSN76489スレーブです。配線・SPI疎通の
確認専用で、**音声のリアルタイム性が崩れる**ため、確認が終わったら`../slave_sn76489.uf2`
(通常版)に書き戻してください。使い方は
[docs/design-notes.md §8.2](../../docs/design-notes.md#82-スレーブ-rp2040-zero--pico)参照。

## `slave_ay8910_VERBOSE.uf2` (VGM_SLAVE_VERBOSE_LOG=ON、生バイト単位トレース付き)

AY-3-8910スレーブが不安定(演奏中に不正なMUTEが発生する、レジスタ値が化ける等)な問題の調査用に、
`slave_common/src/slave_spi_rx.c`のSPI受信ループへ**1バイト届くたびに記録する**処理を追加した
ビルドです。`WRITE0`等の3バイトフレーム単位のログ(下記`slave_sn76489_VERBOSE.uf2`と同種)に
加えて、起動後最初の900バイトぶんを**RAM上のリングバッファに記録**し、たまった時点で一括で
ダンプします(1バイトごとにその場でprintfすると、printf自体のブロッキング時間でSPIハードウェア
FIFO(8バイト分)を溢れさせてしまい、観測したい現象を観測自体が壊してしまうため、記録とダンプを
分離しています)。ダンプ形式:

```
[RX  ] seq=123 val=0x02 t=4821933us
[RX  ] seq=124 val=0x7C t=4821950us DISCARDED(resync)
```

`seq`は起動からの通しバイト番号、`t`はそのバイトが実際にSPIから読めた時刻(µs)です。
`DISCARDED(resync)`が付いているのは、オペコード候補として妥当な範囲(0x00-0x0A)に収まらず、
再同期のために捨てられたバイトです(下記の再同期ロジック参照)。バイト間の`t`の差が想定
(gap_us×3程度)より大きく開いている箇所があれば、そこでバイトが欠落している証拠になります。
ダンプは起動後最初の900バイト分の1回きりです(再度記録したい場合は電源を入れ直してください)。
他のチップのスレーブでも同じ仕組みを使いたい場合は`slave_common/src/slave_spi_rx.c`を
`VGM_SLAVE_VERBOSE_LOG=ON`で再ビルドしてください(共有コードなので全スレーブ共通で使えます)。
**音声のリアルタイム性が大きく崩れる**ため、確認が終わったら通常版に書き戻してください。

## 受信側の再同期ロジック(2026-08-27追加)

実機テストで、CS間隔を広げても・CSにプルアップを付けても、AY8910スレーブでまれに1バイトだけ
欠落する現象が完全には無くならないことが分かりました。従来の受信ロジックは「3バイト読んだら
無条件に1フレームとする」という位置カウンタ方式だったため、1バイトでも欠落すると**それ以降
ずっとフレーム境界がズレたまま**になり、後続の全レジスタ書き込みが誤ったオペコード/レジスタ/
データとして解釈され続けていました(originally reported as random-looking MUTE/RESET spam or
stuck register values)。

`slave_common/src/slave_spi_rx.c`を、**オペコード候補が妥当な範囲(0x00-0x0A)かどうかを検証し、
妥当でなければ1バイトずつ捨てて再同期する**方式に変更しました。1バイト欠落しても、被害は
その場の1フレームだけに抑えられ、次のフレームからは正しく復帰します(完全に無欠落にする
修正ではなく、欠落があっても壊れ方を軽くする頑健性向上です)。

## `master_VERBOSE.uf2` (VGM_MASTER_VERBOSE_LOG=ON)

VGMファイルから読み取った生の値(`[VGM ] 0x50 dd=0x..`等)と、実際にSPIへ送信した値
(`[SPI ] cs=GPIO.. opcode=.. reg=.. data=..`)を1コマンドごとにログへ出すマスターです。
スレーブ側の`VGM_SLAVE_VERBOSE_LOG`ログと突き合わせることで、「VGM解析」「SPI送信(マスター)」
「SPI受信(スレーブ)」のどの段階で値がおかしくなっているかを切り分けられます。確認が終わったら
`../master.uf2`(通常版)に書き戻してください。
