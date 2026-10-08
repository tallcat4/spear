# S.P.E.A.R. 要件定義 v5.0

Signal Processing & Emission Analysis Receiver — Panasonic FZ-G2 / USRP B210 用 RF 解析プラットフォーム。

v4.1 から章立てと § 番号を変更した(内容は同じ)。旧 §10 配送方針は §4.3 に、旧 §11 Headless 動作は §10.4 に移した。

---

# 1. 概要

## 1.1 目的

FZ-G2 と B210 を組み合わせた、タッチ操作の RF 計測器を作る。操作の流れは PortaPack H4 Mayhem と同じ(電源投入 → メニュー → App を 1 つ選ぶ → 実行 → 戻る)。Linux は内部実装として使い、一般的なデスクトップ SDR アプリにはしない。

主要な要求は、動いている DSP のコードを変えずに、任意の中間信号へ後から観測点(ウォーターフォール、アイパターンなど)を追加できることである。既存の STD-T98 受信ツールでは、波形を見るたびに DSP を別プロセスで組み直す必要があった。

このため、システムの中心には「型と時間情報を持ち、観測・分岐・記録できる Signal Stream」を置く。

## 1.2 対象ハードウェア

| 項目 | 内容 |
|---|---|
| コンピュータ | Panasonic FZ-G2(タブレット、第 10 世代 i5、RAM 8 GB) |
| SDR | Ettus Research USRP B210(AD9361 × 1) |
| OS | Arch Linux |
| SDR インターフェース | UHD(`libuhd`)。B210 へのアクセスは UHD だけを使う |

他の PC・他の SDR への移植性は要件にしない。画面サイズ、タッチ操作、B210 の RF 特性、固定の USB 構成に合わせた最適化を許容し、他 SDR 向けの抽象化層は持たない。

## 1.3 運用レンジ

* sample rate は 2〜10 Msps を基本とする。B210 上限の 56 Msps は使わない。
* 想定するアプリケーションの必要帯域は概ね 1 MHz 以下。
* 10 Msps は decimation 前の入口としてだけ使い、各 App の内部 DSP は 1〜2 Msps 相当で動く。
* 基準データレートは 10 Msps sc16 = 40 MB/s。

この前提から次が言える。

* 大容量の stream が fan-out されるのは Radio 直後の 1 本だけで、下流は軽い。
* CPU 負荷は channelizer / decimator に集中するので、最適化はそこに絞る。
* 40 MB/s は i5 第 10 世代・8 GB RAM には軽い。ボトルネックになりうるのは演算性能より OS の電源管理(§11.1)。

---

# 2. アーキテクチャ

## 2.1 構成

```
       B210 (UHD)
            |
     [    Radio    ]   ← Core が所有。UHD を扱う唯一のコンポーネント。RX / TX
            |
     [ Stream Bus  ]   ← fan-out / fan-in
            |
       Active App      ← 常に 1 つだけ動作する
```

Core の責務は次の 3 つに限る。

1. Radio を所有し、UHD を介して B210 を駆動する
2. App の宣言に従って B210 を tune する
3. Stream Bus で stream を fan-out(RX)/ fan-in(TX)する

App はこの上で動く。App は UHD に直接アクセスしない。

## 2.2 配線はビルド時に決定する

DSP グラフの構造はソースコードに静的に書く。実行時のグラフ再構成、stream registry、discovery、動的 attach は持たない。

根拠: 柔軟性が要るのは App を書く開発者であり、装置の利用者ではない。Waterfall を足すならソースを編集して再ビルドすればよく、タッチ画面上でフローグラフを組む機能とそれを支える動的機構は不要である。

## 2.3 同時に動く App は 1 つ

動作中の App が Radio を占有する。複数 App の同時実行、RF リソースの調停、スケジューリングは要件に含めない。

根拠: B210 は AD9361 を 1 個しか持たず、2x2 モードでも RX1 / RX2 は LO を共有する。実用上の RF アプリは周波数帯が離れていて、単一 LO で同時に成り立つ組み合わせは少ない。排他実行にすることで調停ロジックが不要になる。

## 2.4 単一プロセス

Core・App・GUI は同一プロセス内のスレッドとして動かす。プロセス分離、IPC は目標にしない。

根拠:

* 40 MB/s では IPC やインタプリタのオーバーヘッドは問題にならない。単一プロセスにする理由は性能ではない。
* マルチプロセスにすると、観測点を置ける位置がプロセス境界に限られる。上流の IQ を別プロセスから見るには socket で IQ を流すことになり、コピーの増加、fan-out の困難、provenance の分断が起きる。既存の STD-T98 ツールはこのために DSP を別プロセスで複製していた。
* 単一アドレス空間なら、任意の DSP 中間点にコピーも socket もなしに consumer を追加できる。§1.1 の要求はこれで満たす。

代償: アドレス空間を共有するので、データ競合が実行時のバグになる(「30 分に 1 回 overflow」のような切り分けにくい形で現れる)。これは機械的に検査する(§10.2)。

## 2.5 実装言語

Core・App・GUI はすべて C++20 で書く。多言語 App、plugin ABI、バイナリ互換は目標にしない。

根拠: UHD も Qt も C++ なので FFI 境界のコストがない。Core は小さい(§4.5)ので、他言語(Rust など)の安全性の利点より Qt との連携のしやすさを優先する。§2.4 の並行性の問題は言語機能では解決しないので、§10.2 の手段で補う。

例外 — 機械学習モデル(秘話解除): STD-T98 の秘話解除は学習済みモデル(PyTorch 由来、safetensors 形式)を使う。受信・復調・復号の DSP 経路の外にある独立機能で、無くても平文の音声は復号できる。単一プロセス・C++ を保つため、モデルは次の優先順で扱う。

1. 道 1: モデルを C++ で再実装する(safetensors から重みを読んで推論)。MLP や小規模 CNN ならこれが最良で、ランタイムが増えない。
2. 道 2: ONNX + onnxruntime(C++)。中規模で custom op が無い場合。torch 依存が無く、libtorch より軽い。
3. 道 3: libtorch。大規模・特殊な構造の場合だけ。read-only rootfs には重い。

実装前にモデル定義(層構成)と重みサイズを確認し、道 1 か道 2 で済むことを確定させる(§12 M3 の前提条件)。

---

# 3. データモデル

## 3.1 Stream / Event / State

Core が扱う情報を性質で 3 種に分け、混同しない。

| 種別 | 性質 | 例 |
|---|---|---|
| Stream | 連続・大容量 | complex IQ, real samples, discriminator output, audio, symbol stream, FFT result |
| Event | 離散的な事象 | frame detected, packet decoded, signal detected, trigger, overflow, underflow, retune |
| State | 現在の状態 | center frequency, gain, sample rate, app state, B210 connection state, CPU load |

## 3.2 Stream metadata

stream の生成時に次を確定させる。

`stream ID`(静的な文字列定数) / `data type` / `sample format` / `sample rate` / `center frequency` / `bandwidth` / `physical unit` / `direction`(RX/TX)

## 3.3 Sample Block metadata

stream を構成する各データ単位(Sample Block)は次を持つ。

`stream generation` / `sequence number` / `sample index` / `hardware timestamp` / `sample count` / `flags`

flags = { discontinuity, overflow, underflow, timeout, out_of_sequence, retune, clock reset }

flags は 1 つにまとめない。UHD は異常を区別して報告する(§9.1)。特に `overflow`(ホストの読み遅れ)と `out_of_sequence`(トランスポートでのパケット欠落)は原因が異なり、USB 接続の不調は後者に出る。まとめるとフィールドで原因を切り分けられない。

注: ここでの "Block" はデータのかたまりを指す。GNU Radio の "block"(処理ノード)とは別物。参照は `BlockRef`(§5.2)で行う。

## 3.4 時間軸

* 一次基準: USRP の sample counter(連続受信中は単調増加)。実装では UHD の `rx_metadata_t::time_spec` を使う。
* UTC との対応: streaming 開始時に host の monotonic clock との対応を 1 点記録し、sample rate から外挿する。
* 精度: B210 標準の TCXO は数 ppm 級で、GPSDO が無いと長時間の記録や他機器との相関で誤差が累積する。記録の metadata に基準の種別と推定精度を残す。GPSDO の追加を妨げない設計にするが、初期要件には含めない。

## 3.5 stream generation

B210 の切断・再接続やクロックリセットが起きると `time_spec` の原点がリセットされ、前後の timestamp は比較できなくなる。これを `stream generation`(単調増加する世代番号)で明示する。

* 再接続・clock reset のたびに generation を進める。
* generation が異なる block 間では timestamp と sample index を比較してはならない。
* §3.6 の provenance も同じ generation 内でだけ成り立つ。

根拠: USB 接続の SDR では再接続は普通に起こる。時間軸の切断を型で表さないと、時刻が巻き戻った IQ が気付かれずに記録に混ざる。

## 3.6 Provenance(来歴・最小版)

DSP の全段をまたぐ自動追跡の仕組みは実装しない。次の最小版を要件とする。

* すべての Sample Block が `sample index` を持つ。
* すべての Event が、元になった sample index の範囲を持つ。
* rate 変換・群遅延の補正は各 App が計算し、根拠をコメントに残す。

これで、復号したフレームから元の IQ 範囲を求める、trigger 前後の IQ を切り出す、Waterfall 上の位置とパケットを対応付ける、ができる。自動追跡の仕組みは App が数十になったら再検討する。

---

# 4. Stream Bus

静的に配線された fan-out / fan-in 層。システムの中心コンポーネント。

## 4.1 機能

* typed stream
* consumer ごとの独立キュー
* consumer ごとの delivery policy(§4.3)
* consumer ごとの drop 統計
* immutable な sample block の参照配布(§5)
* stream lifecycle(start / stop / EOS)

fan-in(TX)側は consumer が 1 つ(Radio)なので fan-out より単純。

## 4.2 producer と consumer の分離

producer は consumer の存在を知らない。配線が静的でも、producer から consumer へは必ず Stream Bus を経由する。次のような書き方は禁止する。

```cpp
// 禁止: 観測点を後から足せなくなり、
//       decoder の overflow の原因がどの consumer かを切り分けられなくなる
if (waterfall_enabled) waterfall->push(buf);
```

根拠: Stream Bus を挟むことで、(a) 既存コードを変えずに consumer を追加でき、(b) consumer ごとの drop 数・遅延を個別に計測できる。§1.1 の要求はこの規則で満たす。簡略化の際に破られやすいので、最優先で守る。

## 4.3 配送方針と不変条件

consumer ごとに次の 2 種のどちらかを指定する。

| policy | 挙動 | 用途 |
|---|---|---|
| Lossless | 欠落を許さない。キューが溢れたら overflow event を発行し、discontinuity flag を立てる | decoder, IQ Recorder, TX stream |
| Latest Only | 古い buffer を捨てて最新を優先する。捨てた数を数えて公開する | waterfall, spectrum, constellation, GUI 表示全般 |

不変条件(性能上の理由でも緩和しない):

* RX の Lossless stream は、全 sample を配送するか、discontinuity event を発行する。黙って欠落させない。
* TX stream は、全 sample を送出するか、underflow event を発行する。黙って欠落させない。

根拠: 計測器として、取りこぼしは必ず報告する必要がある。遅い GUI の consumer が RF / DSP の処理を止めてはならない。

## 4.4 中間 stream の観測(TAP)

DSP の中間点を観測するための軽量な仕組み。registry も動的 tap も使わない。

```cpp
TAP("std_t98.discriminator", buf, n);
```

* Development build: ローカルファイルへ出力する。
* Release build: コードを生成しない。
* 解析・表示は本体 GUI とは別のツールで行う。

`TAP()` の追加はコード変更だが、DSP の構造は変えない。想定規模は 100 行程度。

## 4.5 実装規模の目安

Core 全体(Radio + Stream Bus + buffer 管理 + source / sink 抽象)で 2000 行程度。大きく超えたらスコープが膨らんでいると判断する。

---

# 5. Buffer

## 5.1 方針

* immutable な sample block
* 参照カウントによる fan-out(1 本の IQ block を複数の consumer がコピーせずに参照する)
* buffer pool / ring buffer

lock-free の実装は初期要件で固定しない。40 MB/s なら単純な実装で足りる。

## 5.2 Block の参照は opaque handle にする

```cpp
// 不可: 将来の shared memory / zero-copy 化ができなくなる
std::shared_ptr<std::vector<std::complex<float>>> get_block();

// 可: 内部表現から切り離す
BlockRef get_block();   // 内部で {pool_id, offset, length} などに解決する
```

根拠: 初期実装が heap 上の pool でも、API が handle 型なら後で最適化できる。この抽象化のコストは今なら小さく、後からだと作り直しになる。immutability を保つため、block は `const` 参照で配る(§10.2)。

---

# 6. Source / Sink

App からは入出力先を区別しない。

RX(Source):

* `B210LiveSource` — 実機。障害時の再接続を担う(§9.2)
* `RecordingSource` — 録音 IQ の再生
* `SyntheticSource` — 合成信号

TX(Sink):

* `B210LiveSink`
* `FileSink`
* `NullSink`

根拠: これにより、ハードウェアなしの開発、オフラインの再解析、回帰テスト、再生が同じ App コードで動く。実機で録った IQ を実機と同じ DSP に流せることが、テスト(§10.3)の前提になる。

---

# 7. App

## 7.1 ライフサイクル

```
create → declare_rf_config → start → (running) → stop → destroy
```

App は起動時に必要な RF 条件を Core に宣言する。動作中の App は常に 1 つなので競合は起きず、Core は宣言をそのまま適用する。適用できなければ App の起動を失敗させる。

宣言項目: `direction`(RX/TX/both) / `center frequency` / `sample rate` / `bandwidth` / `gain` / `antenna`

App は内部で DSP チェーンを静的に構成し、GUI に渡す view frame(§8)を生成する。

## 7.2 App 一覧(想定)

| App | direction | 備考 |
|---|---|---|
| Spectrum Analyzer | RX | |
| Waterfall | RX | |
| IQ Recorder | RX | IQ をディスクに書く普通の App |
| IQ Playback | — | RecordingSource を使う |
| Replay Transmitter | TX | 録音 IQ を送信する |
| FM Receiver | RX | |
| STD-T98 Monitor | RX | §12 M3。既存ツールの移植先 |
| AIS Monitor | RX | |
| GNSS Analyzer | RX | |
| LoRa Analyzer | RX | |
| Unknown Signal Analyzer | RX | pre-trigger 保存が要るなら App 内部に ring buffer を持つ |
| Correlator | RX | |

IQ Recorder と Replay Transmitter も他と同じ 1 つの App として扱い、Core の機能にはしない。どの App の実行中でも録音できる必要はなく、録音したいときは Recorder App を選ぶ。

## 7.3 App 間で共有するコード

コードとして共有するが、Core のインフラではないもの:

* ファイル形式ライブラリ(SigMF 互換 + 独自 sidecar、§7.4)
* Widget library(§8.3)
* 汎用 DSP ブロック(channelizer, resampler, filter など)

## 7.4 記録フォーマット

IQ Recorder App が使う。保存対象: IQ / RF metadata / timestamps / tuning history / events / protocol results / annotations

形式は SigMF 互換 + 独自 sidecar の二層とする。SigMF だけでは tuning history・event・provenance index を表せないため、互換にできる部分を SigMF に、拡張情報を sidecar に置く。録音後に別の App で再解析できること。

---

# 8. GUI

## 8.1 シェル

PortaPack 型のメニューシェル。

```
起動 → App 一覧 → 選択 → 実行 → 戻る
```

## 8.2 技術構成

* Qt 6 / Qt Quick / QML

GUI は raw data を処理しない。

```
IQ → FFT / decimation / view processor → 軽量な view frame → QML
```

GUI が止まっても遅くなっても DSP 処理に影響しないこと。GUI は Stream Bus の consumer の 1 つで、遅ければ自分の stream が drop するだけで、他の consumer には影響しない(§4.3)。

## 8.3 Widget library

再利用できる描画部品として実装する。

Spectrum / Waterfall / Oscilloscope / Constellation / Eye Diagram / Histogram / Numeric monitor / Event log / Hex viewer / Audio monitor

各 App はこれらを静的に組み合わせて固定画面を作る。実行時に任意の stream に attach する仕組みは持たない。

## 8.4 実装上の注意

Waterfall は、worker thread から QSGTexture を更新する自前の scene graph node(`QSGNode` 派生)として実装する。標準の QML Image の更新ではタブレット級の GPU で性能が足りない。

## 8.5 ライセンス

Qt 6 を read-only rootfs の Appliance 構成で第三者に配布する場合、LGPL の再リンク要件が問題になる。自己使用なら影響しない。配布の可能性が出たら早めに確認する。

---

# 9. UHD / GNU Radio

* UHD: B210 への唯一のインターフェースとして使う。
* GNU Radio: DSP の実装手段として使ってよいが、システム全体のアーキテクチャにはしない。GR の scheduler は graph と buffer を自分で所有するので、Stream Bus と GR の境界では必ずコピーが入る。使う場合は App 全体の実装として使い、App 内部に小さな GR graph を複数作らない。GR 内部の TAP は明示的な sink block から Stream Bus へ出す。
* App が将来 native C++ / GNU Radio / GPU / FPGA を選べる設計は妨げないが、初期は native C++ を基本にする。

## 9.1 UHD の利用

SoapySDR のような抽象化層は挟まない。Soapy は複数 SDR への移植性のために異常報告を単純化しており、このシステムに必要な診断情報が失われる。移植性は目標にしない(§14)ので、UHD を直接使う。

使用する機能:

| 機能 | 用途 |
|---|---|
| `rx_metadata_t::error_code` | overflow / timeout / late_command / broken_chain / alignment を区別して §3.3 の flags へ |
| `rx_metadata_t::out_of_sequence` | トランスポート層のパケット欠落を検出する。USB の不調はここに出る |
| `rx_metadata_t::time_spec` | §3.4 の一次基準。provenance の基礎 |
| `recv_async_msg()` | TX 側の underflow / time_error / seq_error / burst_ack。§4.3 の TX 不変条件に必要 |
| timed command (`set_command_time`) | 指定時刻での retune。新しい周波数がどの sample から有効かが sample 単位で決まる |
| `get_sensor("lo_locked")` / `("ref_locked")` | tune 後のロック確認。確認せずに streaming するとロック前のデータが入る |
| transport args (`num_recv_frames` など) | USB バッファの増量。M-1 で overflow が出たら最初に調整する |
| `set_thread_priority_safe()` | RX スレッドの優先度設定(§11.1 の 3) |

## 9.2 障害時の挙動

主な障害要因は USB 接続である。B210 は開発用ボードで、X310 / N3xx 系(GPSDO・10GbE・大容量 FPGA)ほどの信頼性は無い。USB の物理的な不安定さ、TCXO の数 ppm の精度、FPGA の容量は API では解決できない。この前提で Radio 層に次を実装する。

再接続: デバイスが消えたとき、`B210LiveSource` は例外で App を落とさず、再接続を試みる。成功したら §3.5 の stream generation を進め、Stream Bus に discontinuity を出して続行する。記録を止めるより、切れたことを明示して続けるほうを選ぶ。

watchdog: `recv()` が timeout を返し続ける状態(USB は生きているがデータが来ない)を検出し、再初期化する。

起動シーケンス: 各段の失敗を区別して報告する(§10.1)。

```
1. device 列挙 → serial 確認(想定の個体か)
2. FPGA image / FW version 確認 → 不一致なら警告
3. clock source 設定 → ref_locked 確認
4. tune → lo_locked 確認(タイムアウト付き)
5. stream 開始 → 最初の N block を破棄(settling)
6. 定常監視へ
```

## 9.3 B210 FPGA

初期要件ではない。decimation / trigger / channelization などを将来 FPGA へ移す可能性は、アーキテクチャ上残しておく。

---

# 10. 品質保証

## 10.1 装置上での診断

フィールドでは、異常の診断が装置の画面だけでできること(現地でログを取り出して解析することはできない)。次を画面に表示できるようにする。

* consumer ごとの drop 数 / overflow / underflow 回数
* sample discontinuity の発生履歴
* CPU 負荷・温度・thermal throttling の状態
* B210 の接続状態
* ディスク残量

起動時の自己診断: §9.2 の起動シーケンス(serial 確認 → FPGA / FW 確認 → ref_locked → lo_locked → settling)を段ごとに報告する。

実行中は次を Core の health event として扱う: USRP の切断 / 再接続と generation 更新 / overflow / out_of_sequence / timeout / sample discontinuity / thermal throttling / disk-full。

## 10.2 並行性の検査

§2.4 の代償として、単一プロセス・スレッド共有の正しさを機械で検査する。

* ThreadSanitizer を CI で常に走らせる。
* sample block は `shared_ptr<const T>` などで const を強制し、immutability を守る。
* 並行キューは自作せず、実績のある実装(moodycamel::ConcurrentQueue など)を使う。
* 生ポインタで sample を渡す API を作らない(§5.2 の handle 規約)。

## 10.3 テスト

ハードウェアなしで CI が回ること。

* `SyntheticSource` による既知信号での検証
* 実機で録った IQ による golden set
* 各 App について「入力 IQ → 期待される復号結果」の固定ペア

Stream Bus のテスト:

* consumer の追加・削除で producer 側のコードが変わらないこと
* Lossless consumer の drop 数がゼロであること
* Latest Only consumer の遅延が Lossless 側に影響しないこと

## 10.4 Headless 動作

GUI が無い状態で RF / DSP Core が完全に動作すること。CI、回帰テスト、長時間記録に必要。

---

# 11. OS / Appliance 化

初期は通常の Arch Linux 上のアプリケーションとして動けばよい。アーキテクチャは Appliance 化を妨げないようにするが、初期段階ではそれ以上の作業はしない。

段階:

1. Development: 通常の Linux デスクトップ上で起動する(当面はこれ。実機にキーボード・マウスを繋いで開発する)
2. Kiosk: boot → systemd → kiosk compositor → RF application
3. Appliance: 不要なサービスを削除
4. Final Appliance: 専用 OS イメージ / read-only root / A/B update / recovery

イメージビルダーは mkosi などを候補とし、M3 完了後に着手する。

## 11.1 Linux の調整方針

パッケージ数の削減は優先しない。実行状態を制御できることを優先する。

40 MB/s は第 10 世代 i5 には軽く、overflow の主因は性能不足より電源管理の介入になる可能性が高い(負荷が軽いほど CPU が深い省電力状態に入り、USB 割り込みの応答が遅れる)。調整の優先順:

1. CPU governor を performance に固定する(効果が最も大きい)
2. USB autosuspend を無効にする(B210 の切断・再接続の原因になる)
3. RX スレッドを realtime scheduling(SCHED_FIFO)にする(UHD も推奨)
4. memory locking(mlockall)。8 GB なら swap は起きないはずだが確実にする
5. 常駐 daemon の整理 / I/O scheduling / UHD のバッファ設定
6. IRQ affinity(おそらく不要)

## 11.2 メモリ予算(RAM 8 GB)

| 用途 | 概算 |
|---|---|
| Arch + systemd + compositor | 1.0〜1.5 GB |
| Qt 6 / QML + GPU バッファ | 0.5〜1.0 GB |
| Stream Bus の pool + キュー | 10 MB 未満 |
| App 内部(FFT, filter state) | 数十 MB |
| 合計 | 2〜3 GB |

10 Msps では容量は制約にならない。App 内部に 10 秒の ring buffer(約 400 MB)を持っても収まる。

---

# 12. マイルストーン

## M-1: 前提の確認

sample rate の上限は 10 Msps で確定しているので、ここでは電源管理による断続的な欠落が起きないかを確認する。

```
10 Msps, 30 分連続受信、2 条件:
  (a) AC 接続 + governor=performance + USB autosuspend 無効
  (b) バッテリー駆動 + デフォルト設定
記録: overflow 回数 / 発生時刻の分布 / CPU 周波数の推移 / 温度
```

判定: (a) で 30 分ゼロなら M0 へ。(b) だけで発生するなら電源管理が原因で、§11.1 で対処する。(a) でも発生するなら UHD のバッファ設定か USB 構成の問題で、M0 の前に解消する。

## M0: Stream Bus

```
B210 → Radio → Stream Bus → { Spectrum, Waterfall, IQ Recorder }
```

* 同じ IQ stream を 3 つの consumer が同時に使える
* Waterfall の起動・停止が他の consumer に影響しない
* consumer ごとの drop 統計が取れる
* golden vector のテスト一式の骨格を用意する

## M1: Recording / Playback

`B210LiveSource` と `RecordingSource` を同じ API で扱えること。保存した IQ を実機と同じ DSP に入力できること。SigMF + sidecar の形式を確定する。

## M2: 単純な RF App

FM 受信機を実装し、App のライフサイクル・音声出力・GUI との結合・メニューシェルを確認する。

## M3: STD-T98 App

既存の STD-T98 ツールの処理を App として再実装する。着手前に秘話モデルの規模を確認し、§2.5 の道 1 か道 2 で済むことを確定させる。

次を TAP 経由で同時に観測できることを確認する:

wideband IQ / channel IQ / discriminator output / filtered signal / symbol stream / decoded frames

既存の DSP を変えずに、任意の中間 stream に Scope / Waterfall / Constellation を追加できることを目標とする(§1.1 の要求の確認)。

---

# 13. 受け入れ基準

構造:

1. B210 の IQ を Signal Stream として取得できる。
2. 同じ IQ を 3 つ以上の consumer が同時に使える。
3. consumer の追加・削除で producer のコードを変更する必要がない。
4. Latest Only consumer の遅延が Lossless consumer に影響しない。
5. `RecordingSource` と `SyntheticSource` を実機と同じ App に入力できる。
6. DSP の中間 stream を TAP 経由で観測できる。
7. App が UHD に直接アクセスしない。
8. GUI が無くても RF / DSP Core が動作する。
9. ハードウェアなしで回帰テストが CI で完走する。

性能:

10. 10 Msps で 30 分間、すべての Lossless consumer の drop 数がゼロである。
11. Latest Only consumer の attach / detach が、Lossless 側の jitter に測定可能な影響を与えない。

Provenance:

12. 記録した復号フレームから sample index の範囲を求めて IQ を切り出し、再入力すると同じフレームが得られる。

---

# 14. 非目標

初期段階では次を目標にしない。

* 実行時のグラフ再構成 / stream discovery / 動的 attach
* 複数 App の同時実行
* RF リソースの調停・スケジューリング
* マルチプロセス構成 / IPC
* 多言語 App / plugin ABI
* Core 機能としての Recording / Rolling Cache(どちらも App 側の機能)
* 任意の Linux PC / 任意の SDR への対応
* Windows / macOS への移植
* GNU Radio Companion 互換の環境
* SDRangel 互換の plugin API
* 一般ユーザー向けのデスクトップ SDR ソフト
* 最小 Linux ディストリビューションの作成
* 初期段階からの B210 FPGA のカスタマイズ

---

# 15. 運用方針

STD-T98 などの第三者間通信を受信・記録・解析し、TX 機能も持つため、次について方針を決め、機能追加時の判断基準にする。

* 電波法第 59 条(秘密の保護)の扱い
* 記録した IQ・復号結果の保存範囲と期間
* Replay Transmitter の送信可否 — 周波数・出力・免許の条件、電波を輻射しない運用(ダミーロード・減衰器)の扱い

---

# 付録 A. 用語

| 用語 | 定義 |
|---|---|
| Radio | B210 を所有し UHD を扱う唯一のコンポーネント。RX / TX 両方向 |
| Stream Bus | 静的に配線された fan-out / fan-in 層。registry も discovery も持たない |
| App | 装置の 1 機能。排他実行され、固定画面を持つ。Recorder も Transmitter も App の 1 つ |
| Stream / Event / State | 連続データ / 離散事象 / 現在の状態(§3.1) |
| Sample Block | stream を構成する不変のデータ単位。`BlockRef` で参照する |
| BlockRef | Sample Block への opaque handle。内部表現を隠し、将来の zero-copy 化を可能にする |
| Source / Sink | stream の生成端 / 終端。Live / Recording / Synthetic などで差し替えられる |
| TAP | DSP 中間点の開発用出力。Release build では消える |
| Delivery policy | consumer ごとの配送方針。Lossless または Latest Only |
| view frame | GUI に渡す軽量な表示用データ。raw IQ ではない |
| stream generation | 時間軸の世代番号。再接続・clock reset で進む。異なる generation 間で時刻は比較できない(§3.5) |

用語の注意:

* Sample "Block" は GNU Radio の処理ノード "block" と紛らわしい。GR のコードと並べるときは文脈で区別する。混乱が問題になったら "Sample Chunk" への改名を検討する。
* 過去案の "Signal Fabric" は使わない("fabric" は多対多の動的接続を連想させ、一方向・静的な fan-out と合わない)。正式名称は Stream Bus。

---

# 付録 B. 設計判断の一覧

| 判断 | 理由 |
|---|---|
| stream を中心概念に置く | 既存の DSP を変えずに観測点を後から足すため(§1.1) |
| 単一プロセス | IQ をプロセス境界に通さずに、任意の中間点へ consumer を足せるため(§2.4) |
| App は 1 つだけ排他実行 | B210 は LO を共有し、アプリの帯域は離れているので、調停ロジックが不要になる(§2.3) |
| 静的配線 | 装置上でグラフを組む必要がなく、動的な仕組みは複雑さに見合わない(§2.2) |
| C++ のみ | UHD / Qt との連携が容易。Core が小さいので他言語の安全性の利点が小さい(§2.5) |
| Recorder / Transmitter を普通の App にする | 特別なインフラにする理由がない。fan-out の consumer の 1 つである(§7.2) |
| provenance は最小版 | App が一桁なら各 App の計算で足りる(§3.6) |
| Rolling Cache を Core に入れない | 対象は反復する既知の信号が大半で、pre-trigger 保存が要るのは Unknown Signal Analyzer くらい。必要な App が内部に持てばよい(§7) |
| 56 Msps を使わない | 全アプリが 1 MHz 以下で、10 Msps で足りる(§1.3) |
| SoapySDR を使わない | 移植性は目標でなく、抽象化層は異常報告を単純化して診断情報を失う(§9.1) |
| stream generation を持つ | USB SDR では再接続が普通に起き、時間軸の切断を型で表さないと時刻の巻き戻った IQ が混入する(§3.5) |
| 再接続しても App を落とさない | 記録を止めるより、切れたことを明示して続けるほうがよい(§9.2) |
