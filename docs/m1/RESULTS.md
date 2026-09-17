# M-1: 前提の健全性確認 — 結果 (要件 §14 M-1)

## 条件 (a): AC + governor=performance + USB autosuspend 無効 — 2026-09-15

環境: FZ-G2 (i5-10310U) / B210 互換機 (LibreSDR_B220mini, fpga 16.0 fw 8.0) / UHD 4.9.0 /
10 Msps sc16, 100 MHz, gain 30, block 16384 samples, `--mlock`, rtprio **未設定**(SCHED_FIFO 無効)。
生データ: `soak_a_2026-09-15.csv`(1 秒ごと)。

| 指標 | 値 |
|---|---|
| 受信時間 / サンプル数 | 1800 s / 17,979,015,168(起動 2.3 s 込み 9.988 Msps 平均) |
| overflow(host 読み遅れ) | 0 |
| out_of_sequence(USB パケット欠落) | 0 |
| timeout / late_command | 0 / 0 |
| discontinuity / missing_samples | 0 / 0 |
| reconnects / watchdog_resets / generation | 0 / 0 / 0 |
| Lossless consumer drop / pool heap fallback | 0 / 0 |
| CPU 周波数 | 1300–4146 MHz(平均 2213) |
| package 温度 | 41–66 °C(平均 58.7)、throttle count 増加なし |

**判定: (a) で 30 分ゼロ → M0 へ進む。** Acceptance 10 もこの構成で成立。

補足(同日の予備走行、修正前バイナリ): 走行中に 8 並列のビルドを重ね package 温度 100 °C・
熱スロットリング(throttle count 533→1151)が 5 秒間発生したが、その間も overflow / out_of_sequence は 0。
40 MB/s は CPU 性能の問題ではない(§13.1)ことの傍証。

予備走行で `discontinuity` が約 130 回/秒出たのは `time_spec.to_ticks(sample_rate)` の丸めジッタによる
計測バグ(B210LiveSource 側)。master clock tick で index を計算する形に修正済み。

## 条件 (b): バッテリー + デフォルト設定 — 2026-09-15

AC を抜き、`scripts/tune-power.sh restore` でデフォルトへ戻した状態(governor=powersave, EPP=balance_performance
(power-profiles-daemon のバッテリー時設定), USB autosuspend=2 s, B2xx power/control=auto)。`--mlock` なし、rtprio なし。
生データ: `soak_b_2026-09-15.csv`。

| 指標 | 値 |
|---|---|
| 受信時間 / サンプル数 | 1800 s / 17,976,705,024(9.987 Msps 平均。(a) との差は起動時間 0.23 s 分) |
| overflow / out_of_sequence / timeout / late_command | 0 / 0 / 0 / 0 |
| discontinuity / missing_samples | 0 / 0 |
| reconnects / watchdog_resets / generation | 0 / 0 / 0 |
| Lossless consumer drop / pool heap fallback | 0 / 0 |
| CPU 周波数 | 456–3980 MHz(平均 1874) |
| package 温度 | 36–56 °C(平均 53.6)、throttle 増加なし |
| バッテリー | 99% → 85%(B210 の USB 給電込み) |

## 総合判定

(a)(b) とも 30 分ゼロ。§14 の分岐で最良のケース。§13.1 が主敵と想定した電源管理の介入は、この個体
(LibreSDR_B220mini)・この USB 構成・UHD 4.9 デフォルトの transport buffer では 10 Msps に対して顕在化しなかった。
CPU が 456 MHz まで落ちても USB 割り込み応答は間に合っている。

方針への反映:
* §13.1 の調整(performance 固定 / autosuspend 無効 / SCHED_FIFO / mlockall)は「必須」ではなく Appliance 化時の保険とする。
  Development 段階では OS 設定を触らない。
* バッテリー消費 ~14 % / 30 min → フィールドでの連続運用はおよそ 1.5〜2 時間が目安。
* 再接続パス(§17.2)は両条件とも発火しなかった。USB 抜き差しによる意図的な切断試験を別途行う。

## 未解決
* rtprio 未設定のため `set_thread_priority_safe` が失敗する(§13.1 の 3 番)。今回の結果からは不要だが、
  `/etc/security/limits.d/spear.conf` に `<user> - rtprio 99` / `memlock unlimited` を置けば有効化できる。
* B2xx の `ref_locked` は外部 10 MHz 基準 PLL のロック状態。`internal` では unlocked が正常なので
  起動シーケンス stage 3 は external/gpsdo のときのみ必須条件とした。

# USB 切断・再接続試験 (要件 §17.2) — 2026-09-15

10 Msps 受信中に B210 の USB ケーブルを抜き、約 10 秒後に挿し直す。`spear-soak --minutes 2`。

| 回 | 結果 | 原因 / 対処 |
|---|---|---|
| 1 | 切断検出は成功(`disconnected: usb rx6 LIBUSB_TRANSFER_ERROR`)。直後の後始末で **terminate** | UHD の device object 破棄中に USB 送信 → `LIBUSB_ERROR_NO_DEVICE` を noexcept な deleter から throw |
| 2 | 後始末を通過(object をリークして回避)。挿し直し後 stage 1・2 → stage 3 で **terminate** | `uhd::device::make` が発見 address のハッシュで**生きている古い object を返す**。hint のキーはハッシュに入らないので迂回不能 |
| 3 | 同上 | — |
| 4 | (実験) `libusb_submit_transfer` を interpose し NO_DEVICE を成功扱いにする → destructor が timeout で完走、cache expire、`Detected Device`/`Loading FPGA`、**`reconnected: generation advanced (1)`**、10 Msps 再開 | 「消失 device = 全操作が timeout する device」という semantics で UHD が正しく片付くことを実証 |

結論: 素の UHD 4.9 では「破棄すると terminate / 破棄しないと再利用される」の袋小路で、アプリ側では解決できない。
根本原因は `libusb1_zero_copy.cpp` の `release()` → `enqueue_buffer()` が deleter 文脈で `submit_what_we_can()` の
例外を伝播させること(upstream master でも未修正)。**`packaging/libuhd` のパッチで UHD 側を直す**(interpose は削除)。

| 5 | **パッチ済み libuhd 4.9.0.1-8.1**(interpose 削除)。`tear_down` は 0 ms で完了、挿し直し後 `reconnected: generation advanced (1)`、10 Msps 再開、drop 0 | **§17.2 の再接続が成立** |

観察: 挿し直し直後の FW 書き込みで USB が接続→切断→再接続する挙動は UHD の通常動作(FX3 が新 FW で再起動)。
その後の FPGA イメージ転送は USB 2.0 では 60 秒前後かかる。B210 は本機では **USB 2.0 で意図的に運用**している
(10 Msps sc16 = 40 MB/s は USB 2.0 実効帯域の上限付近だが、M-1 の 2 条件で 30 分ゼロ欠落)。

# 装置状態の一次ソース化 (要件 §12.1) — 2026-09-15

「rate が 0 だから書き込み中だろう」式の推定を排し、状態を一次ソースから判定する。

| 状態 | 一次ソース |
|---|---|
| `DISCONNECTED` | libusb 列挙(`uhd::usrp::b2xx::probe()`, パッチ #2)が空 |
| `NO_FIRMWARE` | USB descriptor manufacturer が Cypress(FX3 ブートローダ)/ UHD ログ `Loading firmware image` |
| `STANDBY` | FX3 状態レジスタ = unconfigured / fpga_ready / configuring_fpga / busy、または UHD ログ `Loading FPGA image` |
| `INITIALIZING` | `device::find` 成功〜`make`〜tune の途中(stage 1〜3) |
| `READY` | FX3 = running(閉じている)/ センサ `lo_locked` = true(開いている、stage 4) |
| `STREAMING` | rx streamer が time_spec 付きサンプルを配送している(stage 6) |
| `FAULT` | FX3 = error / interface 0 を他が占有 / lock timeout / stage 失敗 / recv timeout 時に FX3 ≠ running |
| `LOST` | 動作中に `usb_error` / watchdog |

パッチ #2(`packaging/libuhd/0002-*.patch`): 副作用なしの `b2xx::probe()`(`device::find()` は FW 書き込みの副作用を持つため
存在確認に使えない)と、open 中の device 用 property `/mboards/0/fx3_state[_code]` を UHD に追加。

**実測: 受信中に FX3 状態レジスタ(EP0 ベンダ要求)を読むと、バルク転送が約 13 ms 止まり毎回 overflow する**
(USB 2.0、RX thread からでも別 thread からでも同じ)。よって受信中の一次ソースはデータ経路そのものとし、
レジスタは遷移時と recv timeout 時(既に欠落している状況)にのみ読んで原因を分類する。

抜き差し試験(6 回目)で観測した遷移:
`STREAMING → LOST → DISCONNECTED → NO_FIRMWARE → (uhd: Loading firmware) → INITIALIZING → (uhd: Loading FPGA, 68 s) → INITIALIZING(fx3=running) → READY(lo_locked) → STREAMING`、generation 0→1。
