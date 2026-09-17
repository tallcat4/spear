# S.P.E.A.R. 要件定義 v4.1

**Signal Processing & Emission Analysis Receiver**

Panasonic FZ-G2 / USRP B210 専用RF解析プラットフォーム

repository: `spear`

> この文書は単独で完結している。設計に至った経緯を知らなくても、これだけで実装判断ができるよう、各決定には根拠を併記した。

---

# 0. この文書の読み方

本システムは「Linux上で動くSDRアプリ」ではなく、**Linuxを内部実装として使う専用RF計測器**である。参照モデルは PortaPack H4 Mayhem(電源投入 → メニュー → 機能を1つ選択 → 実行 → 戻る)。

設計の中心にあるのは1つの問いである。**「後からウォーターフォールやアイパターンを、既存のDSPコードに一切触れずに追加できるか」**。既存のSTD-T98受信ツール群がこれをできず、波形観測のたびにDSPを別プロセスで組み直す必要があったこと ―― それが本プロジェクトの出発点である。以降の全設計はこの一点に奉仕する。

したがって最重要の設計原則は次の通り。

**中心概念は、プロセスでもGNU Radio blockでも通信方式でもなく、「型と時間情報を持ち、任意に観測・分岐・記録できる Signal Stream」である。**

---

# 1. 対象ハードウェア

| 項目 | 内容 |
|---|---|
| コンピュータ | Panasonic FZ-G2(タブレット, i5 第10世代, RAM 8 GB) |
| SDR | Ettus Research USRP B210(AD9361 × 1) |
| OS | Arch Linux |
| SDRインターフェース | UHD(`libuhd`)。B210への唯一のアクセス手段 |

他PC・他SDRへの移植性は**要件としない**。この固定性を利用し、画面サイズ・タッチ操作・B210のRF特性・固定USB構成への最適化を許容し、他SDR向けの抽象化層は持たない。

## 1.1 運用レンジ(全設計の前提となる数値)

* **sample rate は 2〜10 Msps を基本とする。B210上限の56 Mspsは使わない。**
* 想定するアプリケーションの必要帯域はいずれも概ね **1 MHz以下**。
* 10 Msps は「decimation前の入り口」としてのみ用い、各Appの内部DSPは1〜2 Msps相当で動く。
* **10 Msps sc16 = 40 MB/s** を基準データレートとする。

この数値が効く帰結:

* fan-outされる大容量streamは **Radio直後の1本のみ**。その下流はすべて軽い。
* CPU負荷は channelizer / decimator に集中する。最適化対象はそこに絞られる。
* 40 MB/s は i5第10世代・8 GB RAMに対して軽い。ボトルネックは演算性能ではなく、後述するOSの電源管理設定である。

---

# 2. アーキテクチャ全体像

```
       B210 (UHD)
            |
     [    Radio    ]   ← Coreが所有。UHDを扱う唯一の主体。RX / TX
            |
     [ Stream Bus  ]   ← fan-out / fan-in 層
            |
       Active App      ← 常に1つだけ動作する
```

Coreの責務は3つに限定される。

1. **Radio** を所有し、UHDを介してB210を駆動する
2. Appの宣言に従って B210 を tune する
3. **Stream Bus** を通じてstreamを fan-out(RX)/ fan-in(TX)する

Appはこの3つの上に載る利用者にすぎない。**AppはUHDへ直接アクセスしない。**

---

# 3. 設計制約(最初に固定する4点)

以降の全要件はこの4制約の上に構築される。各制約には「なぜそう決めたか」を付す。

## 3.1 配線はビルド時に決定する

DSPグラフの構造はソースコード上に静的に記述する。実行時のグラフ再構成・stream registry・discovery・動的attachは**持たない**。

**根拠**: 柔軟性が必要なのは「Appを設計する開発者」に対してであり、「装置を操作する利用者」に対してではない。新しいWaterfallを足したければソースを編集して再ビルドする。FZ-G2のタッチ画面上でフローグラフを組む能力は不要であり、それを支える動的機構(registry等)は複雑さに見合わない。

## 3.2 同時にactiveなAppは1つ

activeなAppがRadioを占有する。複数App同時実行・RF resourceの調停・スケジューリング・競合判定は**要件に含めない**。

**根拠**: B210はAD9361を1個しか持たず、2x2モードでもRX1/RX2はLOを共有する。加えて実用上のRFアプリは周波数帯が大きく離れており、単一LOで複数が同時成立する組み合わせは稀。したがって「1つずつ排他実行」は妥協ではなく素直な設計であり、これによりResource Manager(調停ロジック)という最も複雑な部品が丸ごと不要になる。

## 3.3 単一プロセス

Core・App・GUIは**同一プロセス内のスレッド**として動作する。プロセス分離・IPC・plane分割は非目標。

**根拠(本プロジェクトで最も重要な判断)**: 単一プロセスの価値は性能ではなく、**可観測性の自由度**にある。

* 40 MB/s という負荷では、IPCやインタプリタ言語のオーバーヘッドは実測できないほど小さい。性能は単一プロセスを選ぶ根拠ではない。
* 本当の理由はこうである。マルチプロセスにすると、観測点はプロセス境界の位置に縛られる。上流のIQを別プロセスから見るには「socketでIQを流す」しかなく、それはコピー多発・fan-out破綻・provenance分断を招く不健全な選択になる。既存STD-T98ツールがまさにこの問題に突き当たり、波形を見るためだけにDSPを別プロセスで複製していた。
* **単一アドレス空間なら、任意のDSP中間点に、コピーもsocketもなしにconsumerを1本足せる。** これが §0 で述べた「既存DSPに触れず観測点を追加する」を実現する唯一自然な形である。
* 実在の大規模計測器・アビオニクスも、大容量データを汎用socketでプロセス間送信することは避けている(オシロは共有メモリ+DMAで参照、F-35等は上流処理で抽象化してから分散)。単一の汎用コンピュータ上で「データを動かさず参照する」を実現する形が単一プロセスである。

**交換条件**: アドレス空間を共有する以上、データ競合は実行時バグになる(「30分に1回overflow」のような形で現れ、切り分けが困難)。これを人手の注意力で防ぐことはできないため、機械で担保する(§12.2)。

## 3.4 実装言語は C++20

Core・App・GUIすべてを C++20 で記述する。多言語App・plugin ABI・バイナリ互換は非目標。

**根拠**: UHDもQtもC++であり、FFI境界のコストを避けられる。Core規模が小さい(§6)ため、他言語(例: Rust)の安全性がもたらす利得より、Qt連携等の摩擦回避を優先する。ただし §3.3 の交換条件(並行性の担保)は言語機能では自動的に得られないため、§12.2 の手段で補う。

**唯一の留保 —— 機械学習モデル(秘話解除)**: STD-T98 の秘話解除機能は学習済みモデル(PyTorch由来, safetensors形式)を用いる。これは受信・復調・復号のDSP経路の**外**にある独立機能で、無くてもクリア音声は復号できる。単一プロセス・C++を維持するため、モデルは以下の優先順で扱う。

1. **モデルをC++で再実装**(safetensorsから重みを読み、Eigen等で推論)。モデルがMLPや小規模CNN程度なら最良。ランタイム追加なし。
2. **ONNX + onnxruntime(C++)**。中規模でcustom opが無ければ。torch依存が消え、libtorchより軽量。
3. **libtorch**。大規模・特殊構造の場合のみ。read-only rootfsには重いので最後の手段。

実装着手前にモデル定義(層構成)と重みサイズを確認し、1か2で閉じることを確定させる(§14 M3の前提条件)。1・2で閉じる限り、単一プロセス・C++は維持される。

---

# 4. Signal Stream の概念

## 4.1 情報の3分類

Coreが扱う情報を、性質の異なる3種に分け、混同しない。

| 種別 | 性質 | 例 |
|---|---|---|
| **Stream** | 連続・大容量 | complex IQ, real samples, discriminator output, audio, symbol stream, FFT result |
| **Event** | 離散的に発生する事象 | frame detected, packet decoded, signal detected, trigger, overflow, underflow, retune |
| **State** | 現在の状態 | center frequency, gain, sample rate, app state, B210 connection state, CPU load |

## 4.2 Stream metadata

stream生成時に以下を確定させる。

`stream ID`(静的な文字列定数) / `data type` / `sample format` / `sample rate` / `center frequency` / `bandwidth` / `physical unit` / `direction`(RX/TX)

## 4.3 Sample Block metadata

streamを構成する各データ単位(Sample Block)は以下を保持する。

`stream generation` / `sequence number` / `sample index` / `hardware timestamp` / `sample count` / `flags`

flags = { discontinuity, overflow, underflow, timeout, out_of_sequence, retune, clock reset }

**flagsを潰さないこと。** UHDは異常を細かく区別して報告する(§17.1)。特に `overflow`(ホストが読み遅れた)と `out_of_sequence`(トランスポートでパケットが落ちた)は原因が全く異なり、USB接続の不調は後者に現れる。これらを1つの「エラー」にまとめると、フィールドでの原因切り分けができなくなる。

> 注: ここでの "Block" はデータのかたまりの意。GNU Radioでは "block" が処理ノードを指すため、GRコードと並べて読む際は混同に注意。参照は `BlockRef`(§6.2)で行う。

## 4.4 時間軸

* **一次基準**: USRP sample counter(連続受信中は単調増加)。実装上は UHD の `rx_metadata_t::time_spec` を用いる
* **UTC mapping**: streaming開始時にhost monotonic clockとの対応を1点記録し、sample rateから外挿
* **精度の明示**: B210標準TCXOは数ppm級。GPSDO非搭載では長時間記録・他機器相関で誤差が累積する。記録metadataに基準種別と推定精度を残す。将来のGPSDO追加を妨げない設計とするが、初期要件には含めない。

## 4.4.1 stream generation(時間軸の不連続を型で表す)

B210の切断・再接続やクロックリセットが起きると、`time_spec` の原点がリセットされ、**時間軸が切れる**。前後の timestamp を比較することは意味を持たない。

これを暗黙にせず、`stream generation`(単調増加する世代番号)で明示する。

* 再接続・clock reset のたびに generation を進める
* **generation が異なる block 間では、timestamp と sample index を比較してはならない**
* §4.5 の provenance も generation 内でのみ成立する

**根拠**: USB接続のSDRでは再接続は例外ではなく日常的に起こりうる。時間軸の切断を型で表現しておかないと、「なぜか時刻が巻き戻ったIQ」が静かに記録に混入する。§10 の「無言の欠落は存在しない」と同じ思想を、時間軸にも適用する。

## 4.5 Provenance(来歴追跡・最小版)

全DSP段を跨ぐ自動追跡frameworkは**実装しない**。代わりに最小版を要件とする。

* 全 Sample Block が `sample index` を持つ
* 全 Event が「自分の元となった sample index 範囲」を持つ
* rate変換・group delayの補正は各App内で手計算し、根拠をコメントで残す

**根拠**: App数が一桁である限りこれで十分であり、以下を達成できる ―― decoded frameから元IQ範囲を逆算 / trigger前後のIQ切り出し / Waterfall上の位置とpacketの対応付け。自動追跡framework化はApp数が数十規模になってから再検討する。

---

# 5. Stream Bus

**静的配線された fan-out / fan-in 層。** システムの中心コンポーネント。

## 5.1 提供する機能(これだけ)

* typed stream
* consumerごとの独立キュー
* consumerごとの delivery policy(§10)
* consumerごとの drop統計
* immutable sample block の参照配布(§11)
* stream lifecycle(start / stop / EOS)

fan-in(TX)側は consumer が1つ(Radio)なので fan-out より単純。

## 5.2 絶対に守る不変原則

**Producer は consumer の具体的な存在を知らない。**

配線が静的であっても、producerからconsumerへは必ずStream Busを経由する。以下は禁止。

```cpp
// 禁止 —— これを書くと、後から観測点を足せなくなり、
//        「decoderがoverflowする原因はどのconsumerか」を切り分けられなくなる
if (waterfall_enabled) waterfall->push(buf);
```

**根拠**: これが計測器としての性質の全て。Stream Busを挟むことで (a) 既存コードに触れず consumer を追加でき、(b) consumerごとのdrop数・遅延が個別計測でき、原因切り分けが構造的に可能になる。§0 の動機はこの一点に集約される。簡略化の圧力下で最も破られやすい原則なので、最優先で守る。

## 5.3 中間streamの可観測性(TAP)

DSP中間点を観測するための軽量機構。registryもdynamic tapも使わない。

```cpp
TAP("std_t98.discriminator", buf, n);
```

* Development build: ローカルファイルへ出力
* Release build: **完全に消滅**(コード生成なし)
* 解析・表示は本体GUIとは独立したツールで行う

`TAP()` 行の追加はコード変更だが、DSPの**構造**を変えないため「観測のためにアルゴリズムを歪めない」という原則は保たれる。想定実装100行程度。

## 5.4 実装規模の目安

Core全体(Radio + Stream Bus + buffer管理 + source/sink抽象)で **2000行程度**。これを大きく超える場合、スコープが膨張している兆候とみなす。

---

# 6. Buffer 設計

## 6.1 方針

* immutable sample block
* 参照カウントによる fan-out(1本のIQ blockを複数consumerがコピーせず参照)
* buffer pool / ring buffer

lock-freeの具体実装は初期要件で固定しない。40 MB/s では素朴な実装でも足りる。

## 6.2 初期から必ず守る1点 —— Block参照は opaque handle

```cpp
// 不可: 将来の shared memory / zero-copy 化が原理的に不可能になる
std::shared_ptr<std::vector<std::complex<float>>> get_block();

// 可: 内部表現から切り離す
BlockRef get_block();   // 内部的に {pool_id, offset, length} 等へ解決
```

**根拠**: 初期実装がheap上のpoolでも、APIがhandle型でさえあれば将来の最適化余地が残る。この抽象化コストは今なら小さく、後では作り直しになる。immutability を保証するため、blockは `const` 参照で配布する(§12.2)。

---

# 7. Source / Sink 抽象

Appから見て、入出力先が区別されない構造とする。

**RX(Source)**

* `B210LiveSource` —— 実機。**障害時の再接続責務を持つ**(§17.2)
* `RecordingSource` —— 録音IQの再生
* `SyntheticSource` —— 合成信号

**TX(Sink)**

* `B210LiveSink`
* `FileSink`
* `NullSink`

**根拠**: この抽象により、ハードウェアなしでの開発・offline再解析・regression test・playbackがすべて同じAppコードで成立する。実機で取得した録音IQを、実機と同一のDSPへ流し込めることが、テスト戦略(§13)の土台になる。

---

# 8. App

## 8.1 ライフサイクル

```
create → declare_rf_config → start → (running) → stop → destroy
```

Appは起動時にCoreへ必要なRF条件を**宣言**する。activeなAppは常に1つなので競合は起きず、Coreは宣言をそのまま適用する(適用不能ならApp起動を失敗させる)。

宣言項目: `direction`(RX/TX/both) / `center frequency` / `sample rate` / `bandwidth` / `gain` / `antenna`

Appは内部でDSPチェーンを静的に構成し、GUIへ渡す view frame(§9)を生成する。

## 8.2 App一覧(想定)

| App | direction | 備考 |
|---|---|---|
| Spectrum Analyzer | RX | |
| Waterfall | RX | |
| IQ Recorder | RX | 録音は特別機能ではなく、ただIQをディスクに書くApp |
| IQ Playback | — | RecordingSource を使う |
| Replay Transmitter | TX | 録音IQを送信 |
| FM Receiver | RX | |
| STD-T98 Monitor | RX | §14 M3。既存ツールの移植先 |
| AIS Monitor | RX | |
| GNSS Analyzer | RX | |
| LoRa Analyzer | RX | |
| Unknown Signal Analyzer | RX | pre-trigger保存が要るならApp内部にring bufferを持つ |
| Correlator | RX | |

**IQ Recorder も Replay Transmitter も、他と同格の一Appである。** Core機能として特別扱いしない。「今の信号を録りたい」がどのApp実行中でも効く必要はない ―― 録りたければ Recorder Appを選べばよい。

## 8.3 App間で共有するもの(Core機能ではない)

コードとして共有するが、インフラではないもの:

* ファイル形式ライブラリ(SigMF互換 + 独自sidecar, §11参照先なし・下記)
* Widget library(§9.3)
* 汎用DSPブロック(channelizer, resampler, filter 等)

## 8.4 記録フォーマット(IQ Recorder App が用いる)

保存対象: IQ / RF metadata / timestamps / tuning history / events / protocol results / annotations

形式は **SigMF互換 + 独自sidecar** の二層。SigMF単体では tuning history・event・provenance index の表現力が不足するため、interop可能な部分をSigMFに、拡張情報をsidecarに持つ。録音後に別Appで再解析できること。

---

# 9. GUI

## 9.1 シェル

PortaPack型のメニューシェル。

```
起動 → App一覧 → 選択 → 実行 → 戻る
```

## 9.2 技術構成と原則

* Qt 6 / Qt Quick / QML

GUIは raw data 処理を担当しない。

```
IQ → FFT / decimation / view processor → lightweight view frame → QML
```

**GUIが停止・低速化してもDSP処理に影響しないこと。** GUIはStream Busの consumer の一種にすぎず、遅ければ自分のstreamがdropするだけで、他consumerに波及しない(§10)。

## 9.3 Widget library

再利用可能な描画部品として実装する:

Spectrum / Waterfall / Oscilloscope / Constellation / Eye Diagram / Histogram / Numeric monitor / Event log / Hex viewer / Audio monitor

各Appはこれらを静的に組み合わせて固定画面を構成する。「実行時に任意streamへattachする機構」は持たないが、描画部品の共通化は行う。

## 9.4 実装上の注意

Waterfall描画は、QSGTexture を worker thread から更新する自前 scene graph node(`QSGNode` 継承)として実装する。標準のQML Image更新ではタブレット級GPUで早期に破綻する。

## 9.5 ライセンス

Qt 6 を read-only rootfs の Appliance構成で**第三者に配布する**場合、LGPLの再リンク要件が問題になる。自己使用に留まる限り影響しない。配布可能性が出たら早期に確認する。

---

# 10. Delivery policy と不変条件

consumerごとに2種のみ定義する。

| policy | 挙動 | 用途 |
|---|---|---|
| **Lossless** | 欠落を許容しない。キュー溢れ時は overflow event を発行し discontinuity flag を立てる | decoder, IQ Recorder, TX stream |
| **Latest Only** | 古いbufferを破棄し最新を優先。破棄数をカウントし公開 | waterfall, spectrum, constellation, GUI表示全般 |

## 不変条件(計測器の定義。性能上の理由でも緩和しない)

> **RX Lossless stream は、全sampleを配送するか discontinuity event を発行するかの、いずれかである。無言の欠落は存在しない。**

> **TX stream は、全sampleを送出するか underflow event を発行するかの、いずれかである。無言の欠落は存在しない。**

**根拠**: 計測器とアプリケーションの差は「取りこぼしたら必ずそう言う」ことにある。遅いGUI consumer がRF/DSP処理を止めてはならない。

---

# 11. Headless動作

GUIが存在しない状態でRF/DSP Coreが完全に動作すること。CI・regression test・長時間記録の前提であり、要件から外さない。

---

# 12. 品質保証

## 12.1 装置上の可観測性

**フィールド運用時、異常の診断は装置の画面上で完結しなければならない**(現地でログを吸い出して机で解析することはできない)。以下を画面表示可能とする。

* consumerごとの drop数 / overflow / underflow 回数
* sample discontinuity 発生履歴
* CPU負荷・温度・thermal throttling 状態
* B210 connection state
* ディスク残量

起動時の最小自己診断: §17.2 の起動シーケンス(serial確認 → FPGA/FW確認 → ref_locked → lo_locked → settling)を各段で個別に報告する。

実行中は次を Core health event として扱う: USRP disconnect / 再接続と generation 更新 / overflow / out_of_sequence / timeout / sample discontinuity / thermal throttling / disk-full。

## 12.2 並行性の担保(§3.3 の交換条件)

単一プロセス・スレッド共有の正しさを、人手ではなく機械で保証する。

* **ThreadSanitizer を CI に常設**(これが実質的な安全網)
* sample block は `shared_ptr<const T>` 等で **const を強制**し immutability を守る
* 並行キューは自作せず、枯れた実装(例: moodycamel::ConcurrentQueue)を使う
* 生ポインタでsampleを渡すAPIを書かない(§6.2 の handle 規約がこれを助ける)

## 12.3 テスト戦略

**ハードウェアなしでCIが回ることを要件とする。**

* `SyntheticSource` による既知信号での検証
* 実機で取得した録音IQによる golden set(開発が実機上で行えるため入手は容易)
* 各Appについて「入力IQ → 期待される decoded output」の固定ペア

Stream Bus 自体のテスト:

* consumer 追加・削除時に producer 側コードが変わらないこと
* Lossless consumer の drop数がゼロであること
* Latest Only consumer の遅延が Lossless 側に影響しないこと

---

# 13. OS / Appliance化

初期は通常のArch Linux上のアプリケーションとして動けばよい。architectureはAppliance化を妨げないが、**「妨げない」以上の投資を初期段階では行わない。**

段階:

1. **Development**: 通常Linux Desktop上で起動(当面はこれ。実機にキーボード・マウスを繋いで直接開発する)
2. **Kiosk**: boot → systemd → kiosk compositor → RF application
3. **Appliance**: 不要サービス削除
4. **Final Appliance**: dedicated OS image / read-only root / A/B update / recovery

mkosi等のimage builderを候補とするが、着手はM3完了後。

## 13.1 Linux調整方針(性能でなく電源管理が主敵)

パッケージ数削減は優先しない。「小さいLinux」より「実行状態を完全に制御できるLinux」を優先する。

40 MB/s は i5第10世代に対して軽く、**overflowの主因は性能不足ではなく電源管理の介入**になる可能性が高い(負荷が軽いほどCPUが省電力状態へ深く入り、USB割り込み応答が遅れる)。優先順:

1. **CPU governor を performance に固定** —— 最も効く
2. **USB autosuspend 無効化** —— B210の切断・再接続の温床
3. **RXスレッドの realtime scheduling(SCHED_FIFO)** —— UHD推奨でもある
4. **memory locking(mlockall)** —— 8 GBならswapは起きないはずだが確実にする
5. active daemon整理 / I/O scheduling / UHD buffering
6. IRQ affinity(おそらく不要だが余地として)

## 13.2 メモリ予算(RAM 8 GB)

| 用途 | 概算 |
|---|---|
| Arch + systemd + compositor | 1.0〜1.5 GB |
| Qt 6 / QML + GPUバッファ | 0.5〜1.0 GB |
| Stream Bus pool + キュー | 10 MB未満 |
| App内部(FFT, filter state) | 数十 MB |
| 合計 | 2〜3 GB |

10 Msps基準では容量は制約にならない。App内部に10秒のring buffer(約400 MB)を持っても成立する。

---

# 14. マイルストーン

## M-1: 前提の健全性確認(最初にやる。約1時間)

実機は既に組み上がっているため即実行できる。目的は sample rate の上限探しではなく(10 Mspsで確定済み)、**電源管理由来の断続性の確認**。

```
10 Msps, 30分連続受信、2条件で:
  (a) AC接続 + governor=performance + USB autosuspend無効
  (b) バッテリー駆動 + デフォルト設定
記録: overflow回数 / 発生時刻分布 / CPU周波数推移 / 温度
```

判定: (a)で30分ゼロ→M0へ。(b)のみ発生→電源管理が原因確定、§13.1で対処。(a)でも発生→UHD buffer設定かUSB構成の問題、M0前に解消。

## M0: Stream Bus の成立(基盤の核)

```
B210 → Radio → Stream Bus → { Spectrum, Waterfall, IQ Recorder }
```

* 同一IQ streamを3 consumerが同時利用
* Waterfallの起動・停止が他consumerへ影響しない
* consumerごとの drop統計が取得できる
* golden vector suite の骨格を同時に整備

## M1: Recording / Playback

`B210LiveSource` と `RecordingSource` が同一APIで扱えること。保存IQを実機と同じDSPへ入力できること。SigMF + sidecar 形式を確定する。

## M2: 単純なRF App

FM receiver を実装。App lifecycle・audio出力・GUI結合・メニューシェルを検証する。

## M3: STD-T98 App

既存 STD-T98 tools の処理をAppとして再実装する。**着手前に秘話モデルの規模を確認し、§3.4 の道1か道2で閉じることを確定させる。**

以下を TAP 経由で同時観測可能であることを確認:

wideband IQ / channel IQ / discriminator output / filtered signal / symbol stream / decoded frames

既存DSPを修正せず、任意の中間streamへ Scope/Waterfall/Constellation を追加できることを目標とする(= §0 の動機の達成確認)。

---

# 15. Acceptance Criteria

## 構造

1. B210のIQを Signal Stream として取得できる。
2. 同じIQを3つ以上のconsumerが同時利用できる。
3. consumer追加・削除時に producer コードを変更する必要がない。
4. Latest Only consumer の遅延が Lossless consumer に影響しない。
5. `RecordingSource` と `SyntheticSource` を実機と同じAppへ入力できる。
6. DSP中間streamを TAP 経由で観測できる。
7. AppがUHDへ直接アクセスしない。
8. GUIが存在しなくても RF/DSP Core が動作する。
9. ハードウェアなしで regression suite が CI 上で完走する。

## 性能

10. 全 Lossless consumer の drop数が、10 Msps において30分間ゼロである。
11. Latest Only consumer の attach / detach が、Lossless側の jitter に測定可能な影響を与えない。

## Provenance

12. 記録された decoded frame から sample index 範囲を逆算してIQを切り出し、再入力して同一のframeが得られる。

**この12番が通ればprovenance設計は実用水準にある。** 通らなければ、可観測性は「眺められる」止まりで「追跡できる」に達していない。

---

# 16. 非目標

初期段階では以下を目標としない。

* 実行時のグラフ再構成 / stream discovery / 動的attach
* 複数Appの同時実行
* RF resource の調停・スケジューリング
* マルチプロセス構成 / IPC
* 多言語App / plugin ABI
* Core機能としての Recording / Rolling Cache(いずれもApp側の機能)
* 任意のLinux PC / 任意のSDRへの対応
* Windows / macOS への移植
* GNU Radio Companion 互換環境
* SDRangel 互換 plugin API
* 一般ユーザー向けデスクトップSDRソフト
* 最小Linuxディストリビューション作成そのもの
* B210 FPGA の初期段階からのカスタム化

---

# 17. UHD / GNU Radio の位置づけ

* **UHD**: B210への基本かつ唯一のインターフェースとして使う。
* **GNU Radio**: 有力なDSP実装手段として利用可**能**とするが、システム全体のarchitectureにはしない。GRのschedulerは自前でgraphとbufferを所有したがるため、Stream Bus↔GR境界では必ずコピーが入る。使うなら「App全体の実装として」に留め、App内部に複数の小さなGR graphを作らない。GR内部のTAPは明示的なsink blockからStream Busへ出す。
* Appは将来的に native C++ / GNU Radio / GPU処理 / FPGA処理 等を選べる設計を妨げないが、初期はnative C++を基本とする。

## 17.1 UHDを最大限使う(信頼性の要)

**SoapySDRのような抽象化層を挟まない。** Soapyは複数SDRへの移植性のために異常報告を単純化してしまい、本システムが必要とする診断情報が失われる。移植性は §16 で非目標と定めている以上、UHDを直接叩いて情報量を最大化する。

使用する機能:

| 機能 | 用途 |
|---|---|
| `rx_metadata_t::error_code` | overflow / timeout / late_command / broken_chain / alignment を**区別して** §4.3 の flags へ |
| `rx_metadata_t::out_of_sequence` | トランスポート層のパケット欠落検出。USB不調はここに出る |
| `rx_metadata_t::time_spec` | §4.4 の一次基準。provenance の土台 |
| `recv_async_msg()` | TX側の underflow / time_error / seq_error / burst_ack。**§10 のTX不変条件はこれなしに実装できない** |
| timed command (`set_command_time`) | 指定時刻での正確なretune。「どのsampleから新周波数か」がsample単位で確定する |
| `get_sensor("lo_locked")` / `("ref_locked")` | tune後のロック確認。確認せずに streaming するとロック前の汚いデータを取り込む |
| transport args (`num_recv_frames` 等) | USBバッファ増量。M-1 で overflow が出たら最初に触る |
| `set_thread_priority_safe()` | RXスレッドの優先度設定(§13.1 の3番) |

## 17.2 障害時の挙動

**現実的な障害の最大要因はUSB接続である。** B210は開発用ボードであり、基地局に使われるX310/N3xx系(GPSDO・10GbE・大容量FPGA)とは信頼性の系譜が異なる。UHDで到達できるのは「USB SDRとしての上限」であり、以下はAPIでは解決しない: USBの物理的不安定性 / TCXOの数ppm精度 / B210のFPGA容量。

その前提で、Radio層に以下を実装する。

**再接続**: デバイス消失時、`B210LiveSource` は例外でAppを落とさず、再接続を試みる。成功したら §4.4.1 の stream generation を進め、Stream Bus へ discontinuity を打って継続する。計測器として、記録を止めるより「切れたと明示して続ける」ほうが正しい。

**watchdog**: `recv()` が timeout を返し続ける状態(USBは生きているがデータが来ない)を検出し、能動的に再初期化する。ハングと正常の中間状態を放置しない。

**起動シーケンス**: 各段の失敗を区別して報告する。

```
1. device列挙 → serial確認(想定の個体か)
2. FPGA image / FW version 確認 → 不一致なら警告
3. clock source 設定 → ref_locked 確認
4. tune → lo_locked 確認(タイムアウト付き)
5. stream開始 → 最初のN blockを破棄(settling)
6. 定常監視へ
```

この粒度で自己診断を書くと、フィールドで画面を見ただけで原因が分かる装置になる(§12.1)。

## 17.3 B210 FPGA

初期要件ではない。将来 decimation / trigger / channelization 等をFPGAへ移す可能性を、architecture上は閉ざさない。

---

# 18. 運用方針(技術外だが判断基準として文書化)

STD-T98等の第三者間通信を受信・記録・解析し、かつTX機能を持つため、以下を方針として定め、機能追加時の判断を一貫させる。

* 電波法第59条(秘密の保護)の取り扱い
* 記録したIQ・復号結果の保存範囲と期間
* Replay Transmitter の送信可否 —— 周波数・出力・免許の条件、および実際に電波を輻射しない運用(ダミーロード・減衰器)の位置づけ

---

# 付録A. 用語

| 用語 | 定義 |
|---|---|
| **Radio** | B210を所有しUHDを扱う唯一の主体。RX/TX両方向 |
| **Stream Bus** | 静的配線された fan-out / fan-in 層。registryもdiscoveryも持たない |
| **App** | 装置の1機能。排他実行され、固定画面を持つ。Recorder も Transmitter も一App |
| **Stream / Event / State** | 連続データ / 離散事象 / 現在状態(§4.1) |
| **Sample Block** | streamを構成する不変のデータ単位。`BlockRef` で参照 |
| **BlockRef** | Sample Block への opaque handle。内部表現を隠蔽し将来のzero-copy化を許す |
| **Source / Sink** | streamの生成端 / 終端。Live/Recording/Synthetic 等で差し替え可能 |
| **TAP** | DSP中間点の開発用出力。Release buildでは消滅 |
| **Delivery policy** | consumerごとの配送方針。Lossless または Latest Only |
| **view frame** | GUIへ渡す軽量な表示用データ。raw IQではない |
| **stream generation** | 時間軸の世代番号。再接続・clock resetで進む。異なるgeneration間で時刻比較は不可(§4.4.1) |

## 用語の注意

* **Sample "Block"** は GNU Radio の処理ノード "block" と紛らわしい。GRコードと並置する際は文脈で判別する。混乱が実害化したら "Sample Chunk" への改名を検討する。
* 過去案にあった "Signal Fabric" は使わない("fabric" は多対多の動的相互接続を含意し、本システムの一方向静的fan-outと逆の印象を与えるため)。正式名称は **Stream Bus**。

---

# 付録B. 設計判断の要約(なぜこうなっているか一覧)

| 判断 | 理由 |
|---|---|
| streamを中心概念に置く | 既存DSPに触れず観測点を後付けするため(§0) |
| 単一プロセス | 性能ではなく、IQをプロセス境界に乗せずに任意の中間点へconsumerを足せるから(§3.3) |
| App 1つだけ排他実行 | B210のLO共有 + アプリの帯域が離れている → 調停ロジックが丸ごと不要(§3.2) |
| 静的配線 | 装置上でグラフを組む必要がない。動的機構は複雑さに見合わない(§3.1) |
| C++単一 | UHD/Qtとの摩擦回避。Coreが小規模なので他言語の安全性利得が相対的に小さい(§3.4) |
| Recorder/Transmitterを普通のApp化 | 特別なインフラにする理由がない。fan-outのconsumerの一種にすぎない(§8.2) |
| provenance最小版 | App一桁なら手計算で足り、自動追跡frameworkは過剰(§4.5) |
| Rolling Cache を Core から除外 | 反復・既知信号が大半で、pre-trigger保存が要るのは Unknown Signal Analyzer 程度。必要なApp内部に持てばよい(§8) |
| 56 Mspsを使わない | 全アプリが1 MHz以下。10 Mspsで足り、性能問題が実質消える(§1.1) |
| SoapySDRを使わない | 移植性は非目標。抽象化層は異常報告を単純化し、診断情報を失わせる(§17.1) |
| stream generation を持つ | USB SDRでは再接続が日常的に起きる。時間軸の切断を型で明示しないと、時刻の巻き戻ったIQが静かに混入する(§4.4.1) |
| 再接続してもAppを落とさない | 計測器としては、記録を止めるより「切れたと明示して続ける」が正しい(§17.2) |
