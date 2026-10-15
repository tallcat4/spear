# JR TRAIN RADIO — 実装メモ(実機確認前)

JR 在来線デジタル列車無線(352.6 MHz 帯、下り = 基地局送信)の多波同時受信 App。
移植元は jr-conventional-line-radio-receiver(Rust + Tauri、MIT、© 2026 KIRISHIKI Yudai)の commit `dfc7440`(v1.6.0、jr-conventional-dsp v1.1.0)。

**現段階(2026-10-15、ブランチ `feature/jr-train`)**: 実機も実測データも無い状態で、受信 DSP とフレーム同期を C++ に移植した。
原本の Rust と同じ入力で同じ結果になることは確認済み(下記「検証」)。B210 につないで受信を確かめ、実録音(golden)を集めるための App と CLI が揃っている。
音声・データ電文・運行把握はまだ移植していない(「次の段階」)。

## 構成
```
apps/jr_train/
  dsp/jr_dsp.hpp, demodulator.cpp   1 チャネルの受信 DSP(原本 dsp.rs / receiver.rs)
  dsp/detail.hpp, sequence.cpp      64 状態の適応系列推定(sequence.rs)、表示用の観測(signal.rs)、前段(equalizer.rs)
  protocol/frame_sync.*             フレーム同期(原本 receiver-core frame.rs)
  presets.*                         チャネル割当 east 22 波 / central 3 波 / west 19 波(原本 ui/radio-presets.json。路線は持たない)
  receiver.*                        前段(回転 + FIR ÷decim)+ 全チャネル(worker スレッドで並列)+ Observer。Qt / UHD 非依存
  jr_train_app.* / JrTrainPage.qml  GUI(帯域スペクトラム / チャネルのタイル / コンスタレーション / 読み出し / REC)
  tests/                            gtest(原本と同じ結果、原本の単体テストの移植、前段、ライフサイクル)
  tests/vectors/                    参照ベクトル(1.2 MB。tests/refgen が生成)
  tests/refgen/                     参照ベクトル生成器(Cargo。原本の Rust を path 依存で呼ぶ開発用ツール。CMake の対象外)
  LICENSE.MIT                       移植元のライセンス(移植したファイルの SPDX は MIT)
tools/jr_train_decode/              spear-jr-train-decode(録音 → チャネル統計 / フレーム / 原本へ渡す WAV・frames)
widgets/qml/ConstellationView.qml   コンスタレーション(新規の共通ウィジェット)
```

## 信号経路
| 段 | 内容 | 由来 |
|---|---|---|
| RF | 1.92 MS/s(B210 の master clock 30.72 MHz ÷ 16 で正確)。LO = 帯域中心 − 250 kHz(`jr_train.lo_offset_hz`) | spear。STD-T98 と同じく DC を帯域の外へ |
| 回転 | −(帯域中心 − 実際の LO)。double の複素 NCO、4096 sample ごとに正規化 | spear。実際の LO は Core から読む(録音再生でも同じ経路) |
| 前段 FIR | ÷8 → 240 kS/s。Hamming 129 tap、cutoff 85 kHz(`design_lowpass` + `FirDecimator<cf32>`) | spear。帯域は ±68.6 kHz(端のチャネル ±65.625 kHz + 2.9 kHz) |
| チャネル | NCO でチャネルを DC へ → 32 相 polyphase で 19.2 kS/s(4 sample/symbol、Hamming sinc、cutoff 4200 Hz)。rate ≥ 384 kHz なら先に 3 段 CIC | 原本 |
| AGC | 64 sample のピーク(max + 0.4 min)と約 1 s で減衰する基準。基準の 8% 未満は「不確か」(ループは惰性、等化器は学習しない) | 原本 |
| 整合フィルタ・タイミング | RRC α=0.2(1025 tap / 128 倍 / 32 相)と微分 RRC。半シンボル間隔 21 点の窓。誤差 0.5·Re(y·conj(dy))、PI ループ(BW 0.02) | 原本 |
| 前段の等化 | 中央タップ固定(原本で CMA の適応を 0 にしてある) | 原本 |
| 搬送波 | π/4 交番を回転で除く → 系列推定の innovation(最良生き残りの予測との位相差)で 2 次ループ(0.2 / 0.003、±497 Hz)。ロック = 誤差振幅の平滑値 < 0.4 | 原本 |
| 系列推定 | 64 状態(直前 3 シンボル)× 4 タップの複素伝送路推定(NLMS 0.08)。枝メトリック = 二乗距離(上限 16)。トレースバック 32 | 原本 |
| 差動復号 | Gray `[0,1,3,2][(cur − prev) & 3]` → dibit | 原本 |
| フレーム同期 | 384 bit = 192 シンボル = 40 ms。bit 0/128/256 にマーカー `0x3D22 / 0xE867 / 0xE867`(下位 14 bit の一致を数え 2/3 以上)。3 フレーム連続で捕捉、3 フレーム連続の不一致で外れる | 原本 |

**変調の確認**: 4800 baud の π/4-DQPSK。チャネル間隔 6.25 kHz、割当 352.53750〜352.66875 MHz。帯域中心 352.603125 MHz(東・西)/ 352.625 MHz(東海)。
定数はすべて原本のコードから転記した(一次資料は未入手)。原本の単位(rad/symbol、sample、Hz)のまま。

## 設計の決定
| 項目 | 決定 | 理由 |
|---|---|---|
| 言語 | Rust の DSP を C++ に移植(§2.5) | spear は C++ のみ。FFI は使わない。Rust は参照ベクトルの生成にだけ使う |
| 移植の粒度 | 原本の各段を 1 対 1、式の順序もそのまま。`-ffp-contract=off`。`std::complex` の演算子は使わず実部・虚部で書く | 原本と**ビット一致**させる(浮動小数のフィードバックループなので、近似一致では原因の切り分けができない) |
| チャネル化 | 原本どおりチャネルごとの NCO + polyphase(共通の `PfbChannelizer` は使わない) | 6.25 kHz の critically-sampled PFB では 4 sample/symbol が取れず、隣接チャネルの折り返しも出る。原本と同じ入力条件で比較できることを優先 |
| 前段 | 1.92 MS/s → 240 kS/s を spear の `FirDecimator` で | 原本の既定 240 kS/s に合わせる。原本の CIC は 1.92 MS/s(÷8)でも使えるが、`FirDecimator` のほうが通過域が平坦 |
| 並列化 | チャネルを worker スレッドに静的に割り当て、ブロックごとに全チャネルの完了を待つ。Observer は呼び出し元のスレッド | 22 波の MLSE は 1 コアに収まらない(下記)。決定的で、App 側の排他が要らない |
| 受信の判定 | タイルの「SYNC」= 直近 1 s にフレーム同期。搬送波ロックは補助表示 | 原本の搬送波ロックは、空きチャネルでも隣接波の漏れや雑音で立つ(参照ベクトルの空きチャネルでも LOCK、EVM 2〜3% になる例がある)。マーカーの一致のほうが確か |
| Event | チャネルごとの最初のフレーム同期だけ | 全フレームでは 22 波 × 25 /s |
| 録音 | REC は radio.rx(1.92 MS/s ci16)をそのまま SigMF に(RecorderApp と同じ経路、専用の Lossless 購読) | golden は前段を含めて再現できる生の入力にする。7.7 MB/s(1 分 460 MB) |
| 画面 | 周波数・レート・プリセットの操作は置かない。プリセットは `jr_train.preset`(east / central / west、既定 east) | 目的別 App の規約(誤設定で受信不能になる) |

## 検証(実機なし)
- **原本と同じ結果**(`tests/test_dsp.cpp` `MatchesRustReference`): `tests/refgen` が合成した 240 kS/s の帯域 IQ(5 チャネル: 帯域の両端と中央。
  周波数オフセット +120〜+300 / −250 Hz、遅延波 0.45 / 0.2、70 ms のフェード、空きチャネル 1 つ、雑音、sc16 量子化)を原本の Rust と C++ の両方に通す。
  Mac(arm64)で **dibit・フレーム(位置・マーカー誤り数・48 byte)・搬送波状態・周波数誤差・EVM・コンスタレーションの各点がすべてビット一致**。
  テストは dibit とフレームの完全一致、浮動小数の値は許容誤差(libm の違い)で判定する。
- **原本の単体テストの移植**: 系列推定(診断値が遅延波と複素利得を除く、主タップが小さいとき反転しない、ISI と 240 シンボルの欠落を越えて復号、長い無信号のあと復帰)、
  表示の観測(理想点の 4 隅と利得に依らない EVM、位相のばらつきと EVM、欠落で差動の対を切る)、フレーム同期(3 フレームで捕捉・3 フレームで外れる)、入力の区切り方に依らないこと(CIC 経路を含む)。
- **前段**(`test_receiver.cpp`): 参照ベクトルを ×8 に補間して +250 kHz にずらした 1.92 MS/s(B210 の LO を帯域中心 −250 kHz に置いた状態)を Receiver に入れ、
  各チャネルが原本と同等以上に同期し、フレームの中身が一致すること。空きチャネルは同期しないこと。
- **CLI と原本の突き合わせ**: `spear-jr-train-decode --export-wav` が書いた WAV を原本の `jr-receiver decode-wave` に通すと、22 チャネルすべてで
  フレーム数・同期数・EVM・ロック状態が C++ と一致した。`--frames-dir` の出力は原本の `jr-receiver decode-frames` でそのまま読める。
- **ライフサイクル**(`apps/tests/test_apps.cpp` `JrTrainLifecycleAndRecordingOnSynthetic`): 合成源で start → 帯域の観測点が動く → REC で SigMF ができる → stop で consumer 0。
  **Mac ではビルドできないので未実行**(Qt Quick と ALSA が無い)。Arch で `scripts/ci.sh` を回すこと。
- **負荷**(Mac で測定、Apple M5、22 波・雑音入力・1.92 MS/s): 1 スレッドで実時間の 57%、9 スレッドで 13%(壁時計)。FZ-G2(i5-10310U、4C8T)は
  1 コアあたり 2〜3 倍遅いと見て、1 スレッドでは間に合わない。worker は既定でハードウェアスレッド数 − 1(`jr_train.threads` で変更可)。

### 参照ベクトル
`tests/vectors/` は `tests/refgen` の出力そのもの(手で編集しない)。作り直すには、spear と原本を同じディレクトリに並べて:
```
cd apps/jr_train/tests/refgen
CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release -- ../vectors
```
refgen は原本の `jr_conventional_dsp`(path 依存)と `receiver-core/src/frame.rs`(`#[path]` で取り込み)だけを使い、外部 crate も SoapySDR も要らない。
Rust のコードを spear に置くのは §2.5 の例外ではなく、ビルドに含めない開発用ツールとして扱う(STD-T98 の `tools/*.py` と同じ位置づけ)。

## 未検証(実機で確かめること)
1. 実信号で同期するか。原本は RTL-SDR / SoapySDR で実績があるが、B210 + spear の前段は初めて。
2. LO オフセット 250 kHz とゲイン。帯域外の強い信号が前段 FIR(約 −50 dB)で十分落ちるか。
3. 搬送波ループの引き込み範囲(±497 Hz)に B210 の LO 誤差が収まるか(`radio.freq_err_ppm` の補正後。352 MHz で 1 ppm = 352 Hz)。
4. CPU 負荷(FZ-G2、熱スロットリング時を含む)と Lossless の drops。
5. フレームの radio.rx index は DSP の遅延を 58 シンボルと見積もった概算(`receiver.cpp` `kDspLatencySymbols`)。音声の時間合わせに使う前に実録音で較正する。
6. 画面(1920×1200 でのレイアウト、日本語の注記の表示)。`--screenshot` で撮って `docs/gui/` に残す。

## 実機テストの手順
1. Arch でビルドとテスト: `cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build`(`spear_jr_train_tests` と `spear_app_tests` を含む)。`scripts/ci.sh` で TSan も。
2. 必要なら site.conf に `jr_train.preset=east`(東海は `central`、西日本は `west`)。
3. `./spear.sh` → メニューの JR TRAIN RADIO。アンテナは 350 MHz 帯用。
4. 見るもの: 帯域スペクトラムに信号が出るか → タイルが SYNC(緑)になるか → 選択チャネルのコンスタレーションが 4 点に集まるか、FREQ ERR、DSP 負荷、DROPS。
5. REC で 1〜数分録音する(列車の通過・通話がある時間帯)。録音は `~/spear/recordings/jr_train_<preset>_<UTC>.sigmf-*`。
6. オフラインで再現と原本との突き合わせ:
   ```
   spear-jr-train-decode ~/spear/recordings/jr_train_east_XXXX --quiet --frames-dir out/frames --export-wav out/band.wav
   jr-receiver decode-wave out/band.wav 352603125 out/rust east        # 原本(--no-default-features でビルド可)
   jr-receiver decode-frames out/frames/352625000Hz-00 out/rust-frames common   # 原本で音声・データまで復号できるか
   ```
   C++ と原本のチャネル表(frames / synced / EVM)が一致すること。原本の decode-frames で音声 WAV と電文 JSONL が出れば、次の段階の golden になる。
7. 良い区間を切り出して `~/spear/golden/jr_train/` に置き、golden テスト(録音が無ければ skip)を足す。

## 次の段階
1. 音声フレームの FEC(K=9 畳み込み・パンクチャ・外符号 RS / CRC)と、音声合成(CELP 本体 + GRU / MLP の後処理、モデル 4 MB)の C++ 移植。§2.5 の道 1(C++ 再実装)。
2. データ電文(PN9、RS(42,30)、CRC-16、パケット組み立て、JIS 文字列、LH5)と運行把握。
3. WAV / JSONL の保存、通話の自動選択、複数チャネル音声のミキサ(STD-T98 の Mixer を appfw に抽出する。2 つ目の利用者)。
4. 移植の検証はこの段と同じく、refgen を拡張して原本と同じ入力で突き合わせ、実録音の frames(`--frames-dir`)を原本の decode-frames と比べる。

## ライセンス
`apps/jr_train/dsp/`、`protocol/`、`presets.*` は原本(MIT)の派生物。各ファイルに `SPDX-License-Identifier: MIT` と著作権表示を残し、
`apps/jr_train/LICENSE.MIT` に原本の許諾文を置く。MIT のコードは GPL-3.0 の spear に結合できる。
原本の LH5 とフレーム復号には「reference から移植」とあるが、その出典とライセンスが書かれていない。LH5 を移植する前に作者に確認する(フレーム同期は今回移植した)。
