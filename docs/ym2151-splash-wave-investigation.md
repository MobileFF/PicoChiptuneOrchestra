# YM2151「03 Splash Wave.vgm」調査まとめ（2026-08〜09）

OutRun の `調査用/03 Splash Wave.vgm`（YM2151 / OPM、実クロック 4 MHz）で
「音がおかしい」という症状を追った記録。最終的に **原因は2つ**（起動順序で
クロックプリセット RESET を取りこぼす／250 MHz では 62500 Hz の描画に間に合わない）で、
両方修正して **音程は完璧**になった。YM2151 の音量が SegaPCM よりやや小さい件だけ
未対応（本ドキュメント末尾「残課題」）。

関連: `docs/design-notes.md` §2・§5、メモリ `project_ym2151_offline_ab` /
`project_opcode_collision_port_guard` / `project_ym2203_compute_ceiling`。

---

## 1. 症状と最初の見立て（〜2026-08-31）

- FM 密度の高い曲で YM2151 の音が歪む／音程がずれて聞こえる。1・2曲目より3曲目
  （Splash Wave）で顕著。
- 除外できたもの: デジタルクリップ（ホスト実測ピークがフルスケールの 14%）、
  レンダリング underrun（`RATE CHECK` で `fell_behind≈0`）、曲をまたいだ蓄積不具合。
- **オペコード衝突**を発見・対策（`project_opcode_collision_port_guard` に詳細）:
  YM2151 のキーオンレジスタは `0x08`。`{WRITE0(0x02), reg=0x08, data}` のオペコード
  バイトが SPI で落ちると、再同期が次の `0x08` を `VGMSPI_OP_SCC_KEYON` と誤認して
  誤ディスパッチ。対策:
  1. `chip_ym{2151,2203,2413,2612}.cpp` の `*_write()` が `port` 非正規値を捨てる
  2. `slave_spi_rx.c` の再同期を**チップごとの有効オペコードマスク**
     （`VGMSPI_MASK_<CHIP>`）に変更
- `gap_us` を 120→160→200 と上げたら**悪化**（master が曲全体で遅れて早送りする）。
  → 120 に戻した。`>>7`→`>>6` も歪みには無効だった。
- 基板入れ替えテスト（YM2151 ファームを YM2203 の実基板に載せ替え）で
  **スレーブ側配線・アナログは無罪**。
- 当時の結論: **1バイトごとに CS をパルスする 3バイトフレーム送出が FM 密曲に
  対して遅すぎる**（OutRun は 1サンプル=22µs の wait 内に 20〜30 レジスタ書き込み。
  `gap_us=120` だと 1フレーム約 366µs → 30書き込みで約 11ms → 枠に入らず master が
  秒単位で遅れて早送り）。

---

## 2. SPI 転送を速くする（2026-09-02〜03）

### バーストモード（CS を3バイト保持）は SPI モード0 では不可

`tools/spi_bringup_test/` に `-DBRINGUP_BURST` / `-DBRINGUP_CS` を追加して実機検証。

- **結果（`調査用/spi_bring_up_test1.log`）**: master が `10 20 30 / 11 21 31 / …` を
  送っても、スレーブは `10 11 12 / 13 14 15 / …` = **各バーストの1バイト目しか
  受信できない**。バイト2・3は無警告で欠落。
- 原因: PL022 のスレーブモードは **CPHA=0（モード0）だと各バイトの先頭ビットを
  CS アサート時にサンプル**するため、バイト間に CS エッジが必須。

### SPI モード1（CPHA=1）へバス全体を移行

`-DBRINGUP_MODE1` を足して再検証 → **モード1なら CS 保持の連続バーストを
取りこぼさない**ことを実機確認（SEND == RECV）。

本番反映（フラグデー、全ファーム再フラッシュ）:

| ファイル | 変更 |
|---|---|
| `src/slave_common/src/slave_spi_rx.c` | `spi_set_format(..., SPI_CPHA_0, ...)` → `SPI_CPHA_1` |
| `src/master/src/slave_bus.c` `slave_bus_init()` | `spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST)` を追加 |
| `src/master/src/slave_bus.c` `s_routes` | YM2151 の `gap_us` を `120` → **`0`（バースト）** |
| `firmware/vgmplay.ini` | `[ym2151] gap = 0`、`gap` の説明更新 |

1フレーム = 24bit / 4MHz ≈ **6µs**（従来 366µs）。

**結果: 「若干改善」したが歪み・音程ずれは残った** → 転送だけが原因ではなかった。

---

## 3. エミュレーション出力の切り分け（2026-09-03）

「特定音色の発音バグでは？」という疑いに対し、`tools/offline_render/` を YM2151 対応に拡張。

### 追加したツール

- `render_ym2151.cpp` + `run_ym2151.sh` — 出荷中の `vgm_player.c` ＋ 出荷中の
  `chip_ym2151.cpp`（ymfm-OPM）で、YM2151 のネイティブレート（4MHz → 62500Hz）で
  レンダリング。`_raw`（スケーリング前 L+R）／`_shipped`（`>>6`+クランプ）／
  `_pwm10`（さらに `>>2`＝10bit PWM DAC 相当）の3 WAV を出力。
  ラッパーに `ym2151_render_raw()` を追加（`ym2151_render()` はそれを呼ぶ形に整理）。
- `render_ym2151_nuked.c` + `run_ym2151_nuked.sh` — 同じパーサで、合成コアだけ
  **Nuked-OPM**（サイクル精度、事実上の基準）。
  `tools/offline_render/vendor/nuked-opm/`（`opm.c` / `opm.h` / `LICENSE`、**LGPL-2.1**）に
  vendoring。リポジトリ直下に `.gitignore` を作成し `tools/offline_render/vendor/` を除外
  （公開時に含めない。ファームウェアには一切入らない。`vendor/README.md` に取得元・
  バージョン・除外方針）。

### 結果 — エミュレーションは無罪

- ymfm raw: peak 37504 / rms 7013。Nuked raw: peak 37248 / rms 7015。ほぼ一致。
- 7サンプル整列後 **相関係数 0.995**（90秒全体）、RMS 差は信号の 10%、10秒ごとの
  相関 0.991〜0.996 で**ドリフト・破綻なし**。
- 両コアとも `>>6` クランプ 0/5.6M、スペクトル平坦度 ~0.0004〜0.0008（強い純音）、
  DC オフセット −0.037 は**両方に出る**＝曲の実内容。
- `_pwm10`（10bit 量子化版）も含めて `調査用/` の WAV は**すべてクリーンに聞こえた**。

→ 歪み／ピッチ問題は **チップエミュレーションより下流**。ユーザーの聞こえ方は
「歪み」より **音程がズレる** が正確、という重要情報も得られた。

---

## 4. 根本原因の特定（2026-09-03）

### 診断ファーム

`src/slave_ym2151/src/main.c` に `#ifdef VGM_YM2151_USB_DIAG` ブロック（`sys clock` 表示）、
`src/slave_common/src/slave_engine.c` の `RATE CHECK` 行に **`reset_rx`（RESET 受信回数）** を
追加。`firmware/debug/slave_ym2151_USBDIAG_pico2.uf2` をビルド。
（USB CDC が ChromeOS から Linux に渡らず、当初ログが見えなかった。最初の診断版に
入れていた「USB端末を最大5秒待つ」処理自体が SPI 受信開始を遅らせて RESET を確実に
取りこぼしていたため、それも削除。）

### 実機ログが示したこと

```
=== YM2151 slave USB DIAG === sys clock = 250000000 Hz
[YM2151] audio engine running at 55930 Hz
[YM2151] RATE CHECK: 55930 samples in 1000200us (... @ 55930 Hz), fell_behind=15/55930, reset_rx=0
   ← 曲頭 RESET 前。RESET 行が一切出ない
```

master(10s)・スレーブ(5s) の起動待ちを入れて**確実に聞いている状態**にすると:

```
[YM2151] RESET preset=1 (62500 Hz)   ×3   ← slave_bus_reset の3回送出、全部届いた
[YM2151] RATE CHECK: 62500 samples in 1010240us (... @ 62500 Hz), fell_behind=1655/62500, reset_rx=3
[YM2151] RATE CHECK: 62500 samples in 1055310us (... @ 62500 Hz), fell_behind=20910/62500, reset_rx=3
```

### 原因1 — クロックプリセット RESET の取りこぼし（起動順序）

- master は曲頭で `slave_bus_reset()` を**1回だけ**呼ぶ（`{OP_RESET, preset, 0}`）。
- YM2151 スレーブの**起動時デフォルトは preset 0 = 3.579545 MHz = 55930 Hz**
  （`slave_audio_engine_run` が `ops->sample_rate_hz(0)` で初期化）。
- その曲頭の 1ms ほどの間にスレーブがまだ SPI を聞いていない（起動が遅い／
  セッション中に再フラッシュ）と、**その曲の間ずっと 55930 Hz** で出力。
- `03 Splash Wave.vgm` は 4MHz = 62500Hz 前提。ymfm は KC/KF で表現された音高を固定内部
  比率で生成し、それを何 Hz として再生するかがピッチを決める → 55930/62500 = 0.895
  → **曲全体が一様に約 −1.9 半音フラット**。
- 特徴: 歪みではなくクリーンなピッチ誤り／音量非依存／`gap_us` 非依存／
  オフラインレンダには出ない（プリセットを直接計算しているため）。

### 原因2 — 250 MHz では 62500 Hz の描画に間に合わない

- RESET 後の RATE CHECK で `fell_behind` が 79〜20910 / 62500 と乱高下、
  `samples in` が 1012000〜1055000us（実効レートが 1〜5.5% 低下し揺れる）。
- FM 密度の高い箇所でコア（ymfm generate + FIFO ドレイン）が 16µs/サンプルの
  予算を超え、`slave_engine` の「resync to now」が多発 → 実効再生レートが落ちる
  → 残留するピッチ揺れ（−0〜約 −90 cents）。
- 55930 Hz（予算 17.9µs）では余裕（`fell_behind=15`）だった。

---

## 5. 修正（2026-09-03、フラグデー）

### A. `VGMSPI_OP_CLOCK`（0x0D）— チップをリセットせずレートだけ再設定

| ファイル | 変更 |
|---|---|
| `src/protocol/vgm_spi_protocol.h` | `VGMSPI_OP_CLOCK = 0x0D` 定義、`VGMSPI_OP__COUNT` → `0x0E`、`VGMSPI_MASK_BASE` に `(1u << VGMSPI_OP_CLOCK)` 追加 |
| `src/slave_common/src/slave_engine.c` | `case VGMSPI_OP_CLOCK:` 追加。`sample_rate_hz` が実際に変わった時だけ再ペーシング（毎秒来ても実質 no-op）。変化時に `[NAME] CLOCK preset=X (Y Hz) -- late correction` を出力 |
| `src/master/src/slave_bus.c` / `.h` | `slave_bus_set_clock(chip, preset)` 追加（`{OP_CLOCK, preset, 0}` を1フレーム送出） |
| `src/master/src/vgm_player.c` | 曲頭で各チップのプリセットを `s_chip_preset[]` に保存。`reassert_clocks()` が YM2413/2612/2151/2203/SegaPCM へ再送。`wait_samples()` から**曲の1秒ごと**に呼ぶ |
| `tools/offline_render/render_wav.c` / `render_ym2151.cpp` / `render_ym2151_nuked.c` / `tools/host_tests/stub_slave_bus.c` | `slave_bus_set_clock` のスタブ追加（ビルド確認済み） |

効果: 曲頭 RESET を取りこぼしても、**次の1秒で `OP_CLOCK` が来て自動的に
正しいレートへ矯正**される。チップ状態は一切触らないので毎秒送っても安全。

> フラグデー理由: `VGMSPI_MASK_BASE` に新ビットが入るため、未更新スレーブは
> `{0x0D, preset, 0}` を再同期で誤読し、`preset`（=1）を `OP_RESET` と解釈して
> 曲中に毎秒チップリセットしてしまう。master と全8スレーブを同時に書き換えること。

### B. YM2151 スレーブ → 320 MHz / 1.15 V

| ファイル | 変更 |
|---|---|
| `CMakeLists.txt` | `VGM_YM2151_SYSCLK_KHZ 320000` / `VGM_YM2151_VREG_MV 1150`（YM2203 と同値）を追加 |
| `src/slave_ym2151/CMakeLists.txt` | `VGM_RP2350_SYSCLK_KHZ` / `_VREG_MV` に上記を渡す |

`slave_overclock.h` により `clk_peri` は 48 MHz 固定のままなので SPI は仕様内。
YM2612 / SegaPCM は 250 MHz のまま。

### 結果

- **音程が完璧になった**（ユーザー確認）。
- `slave_bus_reset` も RESET を3回送出（`sleep_us(300)` 間隔、保険）。

---

## 6. 副産物・その他の変更

- `tools/spi_bringup_test/`: `-DBRINGUP_BURST` / `-DBRINGUP_MODE1` / `-DBRINGUP_CS=<n>`。
  Pico2 用スレーブビルド（`spi_bringup_test_slave_pico2.uf2` ほか）。
- `firmware/debug/`: 各種診断 uf2（`slave_ym2151_USBDIAG*_pico2.uf2`、
  `master_BOOTWAIT10.uf2`、`spi_bringup_test_*` mode1/burst 版）＋ `README.md` 追記。
- `src/master/CMakeLists.txt`: `-DVGM_MASTER_BOOT_DELAY_S=N` パススルー。
- `src/slave_ym2151/`: `-DVGM_YM2151_USB_DIAG` / `-DVGM_YM2151_BOOT_DELAY_MS=N`
  （どちらも未指定で 0＝通常動作、`#ifdef` ガード済み）。
- `.gitignore` 新規（`build-*/`、`tools/offline_render/vendor/`、
  `tools/offline_render/*.wav`）。
- `docs/design-notes.md` §2・§5 を更新。

---

## 7. 残課題

**YM2151 の出力音量が SegaPCM よりやや小さい**（今回は不問）。

- `render_ym2151` の計測: `03 Splash Wave.vgm` の `_shipped`（`>>6`+クランプ）ピークは
  約 586/2048 ＝ レンジの約 29%。クランプは一度も発生していない（ヘッドルームが余っている）。
- シフト量とヘッドルーム（この曲）:

  | shift | ピーク/2048 | クランプ |
  |---|---|---|
  | `>>6`（現行） | 586 (29%) | 0 |
  | `>>5` | 1172 (57%) | 0 |
  | `>>4` | 2048 (100%) | 167/562万 (0.003%) |
  | `>>3` | — | 2.9%（過大） |

- `>>5` は無害に +6 dB。`>>4` はこの曲には最適だが、ラウドな YM2151 曲
  （1 チャンネルで `>>6` 時 ±1020 ＝ `>>4` で ±4080）ではクランプするので
  固定シフトの既定にはできない。根本策は曲ごとの正規化／AGC かソフトリミッタ。
- 判断が必要なら `tools/offline_render/run_ym2151.sh <vgm> <out> <sec> <shift>` で
  各シフトの WAV を作って試聴できる。
