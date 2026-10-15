# JR TRAIN RADIO — 実装メモ(実機確認前)

JR 在来線デジタル列車無線(352.6 MHz 帯、下り = 基地局送信)の多波同時受信 App。
移植元は jr-conventional-line-radio-receiver(Rust + Tauri、MIT、© 2026 KIRISHIKI Yudai)の commit `dfc7440`
(v1.6.0、jr-conventional-dsp v1.1.0、jr_conventional_voice v1.3.0)。

**現段階(2026-10-15、ブランチ `feature/jr-train`)**: 実機も実測データも無い状態で、原本の受信機能をすべて C++ に移植した。
受信 DSP、フレーム同期、音声フレームの誤り訂正、音声合成(CELP + 学習済みモデル)、データ電文、運行把握、通話の自動選択、保存(WAV / JSONL)を含む。
原本の Rust と同じ入力で同じ結果(Mac でビット一致)になることは確認済み(下記「検証」)。Mac で spear 全体をビルドして画面とライフサイクルも確認した。
B210 での受信は未確認。Arch でのビルド・テストと FZ-G2 での負荷の測定・最適化は 2026-10-16 に済ませた(下記「負荷と最適化」)。

## 実機での引き継ぎ(最初にやること)
1. 取得: `git fetch && git checkout feature/jr-train`。参照ベクトルの再生成やファジングをするなら、原本を spear と同じ階層に置く
   (`git clone https://github.com/soltia48/jr-conventional-line-radio-receiver ../jr-conventional-line-radio-receiver && git -C ../jr-conventional-line-radio-receiver checkout dfc7440`)。
2. ビルドとテスト: `cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build --output-on-failure`、続けて `scripts/ci.sh`(TSan)。
   - 参照ベクトルは Mac で作ったので、Arch では浮動小数に依存するテストが許容差で比べられる(落ちれば libm の差が許容を超えたということ)。
   - Arch でも厳密に比べるなら「参照ベクトル」の 3 コマンドを走らせ直して再テストし、差が出たベクトルをコミットするかを判断する(Rust stable と crates.io が要る)。
   - 「差分ファジング」も同じ手順で走らせる(Arch の libm・GCC での一致の確認)。
3. 実機: 下の「実機テストの手順」。見つかったことは「未検証」の番号に沿ってこのファイルに書く。
4. 録音(REC IQ)と保存(SAVE)を集め、golden テストを足す(「次の段階」)。

## 構成
```
apps/jr_train/
  dsp/jr_dsp.hpp, demodulator.cpp   1 チャネルの受信 DSP(原本 dsp.rs / receiver.rs)
  dsp/detail.hpp, sequence.cpp      64 状態の適応系列推定(sequence.rs)、表示用の観測(signal.rs)、前段(equalizer.rs)
  protocol/frame_sync.*             フレーム同期(receiver-core frame.rs)
  protocol/coding.*                 PN9 デスクランブル、CRC-16、GF(256) と RS 復号(coding.rs)
  protocol/voice_frame.*            音声フレームの誤り訂正(voice_frame.rs: K=9 畳み込み・Viterbi・外符号・合同訂正)
  protocol/data.*, text.cpp, lha.cpp  データ電文(data/mod.rs・packet.rs・operation.rs)、JIS 文字列(text.rs)、LH5 展開(lha.rs)、電文の JSON
  protocol/jis0208_table.cpp        JIS X 0208 → Unicode の表(refgen が encoding_rs から生成。WHATWG index-jis0208、CC0)
  voice/                            音声合成(jr-voice の移植。celp/ は mcelp-codec 由来、models/ は原本の係数 4 MB を埋め込む)
  pipeline.*                        1 チャネルの受信処理(pipeline.rs: DSP → 同期 → 音声 / データ、epoch、履歴、運行把握)
  call_monitor.*                    通話の自動選択(monitor.rs)
  storage.*                         保存(recording.rs / wave.rs: チャネル別 WAV、raw-frames.jsonl、data-messages.jsonl)
  presets.*, presets_json.cpp       チャネル割当と路線(presets.rs。原本の ui/radio-presets.json を埋め込み、同じ検査で読む。外部ファイルも同じ形式)
  receiver.*                        前段(回転 + FIR ÷decim)+ 全チャネルの Pipeline(worker スレッドで並列)+ Observer。Qt / UHD 非依存
  jr_train_app.* / JrTrainPage.qml  GUI
  tests/                            gtest(原本と同じ結果、原本の単体テストの移植、前段、保存)。voice/tests も
  tests/vectors/                    参照ベクトル(DSP 1.2 MB、protocol 2.6 MB、voice 0.3 MB。tests/refgen が生成)
  tests/refgen/                     参照ベクトル生成器(Cargo。原本の Rust を path 依存で呼ぶ開発用ツール。CMake の対象外)
  LICENSE.MIT                       移植元のライセンス(移植したファイルの SPDX は MIT)
tools/jr_train_decode/              spear-jr-train-decode(SigMF / 原本と同じ IQ WAV / 生 sc16 → 復号。原本の decode-wave と同じ形式の出力、保存、frames、WAV)
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
| 搬送波 | π/4 交番を回転で除く → 系列推定の innovation で 2 次ループ(0.2 / 0.003、±497 Hz)。ロック = 誤差振幅の平滑値 < 0.4 | 原本 |
| 系列推定 | 64 状態(直前 3 シンボル)× 4 タップの複素伝送路推定(NLMS 0.08)。枝メトリック = 二乗距離(上限 16)。トレースバック 32 | 原本 |
| 差動復号 | Gray `[0,1,3,2][(cur − prev) & 3]` → dibit | 原本 |
| フレーム同期 | 384 bit = 192 シンボル = 40 ms。bit 0/128/256 にマーカー `0x3D22 / 0xE867 / 0xE867`(下位 14 bit の一致を数え 2/3 以上)。3 フレーム連続で捕捉、3 フレーム連続の不一致で外れる | 原本 |
| デスクランブル | マーカーを除く 336 bit に PN9(全 1 から)を XOR → 42 byte | 原本 |
| 音声フレーム | byte 5–15 / 21–31 の 176 bit = K=9 畳み込み(0x1AF / 0x11D、パンクチャ)、78 bit + CRC-8(0x19B)。外側: CRC-16 + 短縮 RS(パリティ 6)。逆変換 → 外符号の RS 訂正 → Viterbi → 合同訂正(上限つき)の順。出力は 158 bit(20 byte) | 原本 |
| 音声合成 | CELP(整数)+ GRU / MLP の後処理(予測・補正・プランナ・時間方向)。先読み 7 フレーム(280 ms)、8 kHz・16 bit。音声が 100 ms 途切れたら先読みを吐き出す | 原本 |
| データフレーム | RS(42,30)(12 根、t = 6)+ CRC-16 → 28 byte。Slotted(データ波・音声波): 16 スロット周期、トラフィック 3–7 / 11–15、チャネル状況・呼設定は単独フレーム。Common(共用波): 全フレームがパケット | 原本 |
| 電文 | 在線 / 登録 / 転送 / 通告(Slotted)、一覧 / 旅客案内(0x21、LH5 圧縮あり)/ 運行把握(0x22/C9、LH5 の線区表)(Common)。解釈できないものは Unsupported として生のまま | 原本 |
| 通話の自動選択 | 受理した音声フレームの時刻(IQ の共通時間軸)で通話の始まりを決め、早い順。1 s 途切れたら終わり。同期確立の 120 ms 待ってから決める。手動で選ぶと固定 | 原本 |

**変調**: 4800 baud の π/4-DQPSK。チャネル間隔 6.25 kHz、割当 352.53750〜352.66875 MHz。帯域中心 352.603125 MHz(東・西)/ 352.625 MHz(東海)。
定数はすべて原本のコードから転記した(一次資料は未入手)。原本の単位(rad/symbol、sample、Hz)のまま。

## 設計の決定
| 項目 | 決定 | 理由 |
|---|---|---|
| 言語 | Rust の全機能を C++ に移植(§2.5)。音声合成の学習済みモデルも C++ で推論(§2.5 道 1) | spear は C++ のみ。FFI は使わない。Rust は参照ベクトルの生成にだけ使う |
| 移植の粒度 | 原本の各段を 1 対 1、式の順序もそのまま。`-ffp-contract=off`。`std::complex` の演算子は使わない。Rust の浮動小数・整数の意味(NaN の max/min、飽和する `as`、wrapping)を再現 | 原本と**ビット一致**させる(浮動小数のフィードバックループなので、近似一致では原因の切り分けができない) |
| チャネル化 | 原本どおりチャネルごとの NCO + polyphase(共通の `PfbChannelizer` は使わない) | 6.25 kHz の critically-sampled PFB では 4 sample/symbol が取れず、隣接チャネルの折り返しも出る。原本と同じ入力条件で比較できることを優先 |
| 前段 | 1.92 MS/s → 240 kS/s を spear の `FirDecimator` で | 原本の既定 240 kS/s に合わせる。`FirDecimator` のほうが CIC より通過域が平坦 |
| 並列化 | チャネルを worker スレッドに静的に割り当て、ブロックごとに全チャネルの完了を待つ(音声合成も worker で)。Observer は呼び出し元のスレッドで、チャネルの順に | 移植直後の 22 波は 1 コアに収まらなかった(下記「負荷と最適化」)。決定的で、App 側の排他が要らない |
| 最適化 | 演算順を変えずに速くする方法だけを使う: 剰余の除去、帰還の無い段で出力を複数まとめて計算、状態・出力・窓を**またいで**の SIMD(レーンごとの演算は原本の 1 要素分と同じ)。AVX2 版は `target_clones` で既定の x86-64 版と併せ持つ | 原本とのビット一致を保つ(SIMD の幅や命令セットで値が変わらない)。`-march=native` は使わない(spear 全体の方針) |
| 内部エラー | 原本はチャネルの worker が失敗すると受信全体を止める。spear ではそのチャネルだけ作り直して続ける(`resets` に数える) | 可搬機で受信を止めない(§9.2) |
| 受信の判定 | タイルの「SYNC」= 直近 1 s にフレーム同期。「VOICE」= 通話中。搬送波ロックは補助表示 | 原本の搬送波ロックは、空きチャネルでも隣接波の漏れや雑音で立つ(参照ベクトルの空きチャネルでも LOCK、EVM 2〜3% になる例がある) |
| 音声の再生 | 1 チャネルだけ(原本と同じ)。AudioSink 8 kHz。再生を始めるとき 200 ms の無音を前置。新しく選んだチャネルでは 600 ms より古い音声を捨てる | 原本の UI と同じ規則。到着の揺らぎ(ブロック 8.5 ms、合成の先読み)を吸収する |
| 保存 | SAVE は原本と同じ形式(`<record_dir>/jr_train/recording-…/`)。既定で音声・生フレーム・電文のすべて(`jr_train.save_audio / save_raw / save_messages`) | 実機での収集が目的。原本の decode-frames や解析ツールにそのまま渡せる |
| JSON | 電文の JSON は serde の出力と同じ(data.jsonl はキーを構造体の順、保存の JSONL はキーを辞書順 = serde_json::Value) | 原本の出力と diff で比べられる |
| JIS の表 | refgen が encoding_rs(原本と同じ)から生成した表を置く | iconv の EUC-JP は WHATWG と数文字違う。原本と同じ文字にする |
| Event | チャネルごとの最初のフレーム同期だけ | 全フレームでは 22 波 × 25 /s |
| 録音 | REC IQ は radio.rx(1.92 MS/s ci16)をそのまま SigMF に(RecorderApp と同じ経路、専用の Lossless 購読) | golden は前段を含めて再現できる生の入力にする。7.7 MB/s(1 分 460 MB) |
| 画面 | 周波数・レート・プリセットの操作は置かない。プリセットは `jr_train.preset`(east / central / west、既定 east) | 目的別 App の規約(誤設定で受信不能になる) |

## 設定(site.conf)
| キー | 既定 | 内容 |
|---|---|---|
| `jr_train.preset` | `east` | プロファイル(east / central / west、外部ファイルならその ID) |
| `jr_train.presets_file` | なし | 原本と同じ形式(schema_version 1)のプリセット JSON。全体を検査し、誤りがあれば組み込みのまま画面にエラーを出す |
| `jr_train.lo_offset_hz` | 250000 | LO を帯域中心からずらす量 |
| `jr_train.threads` | 0(自動) | worker スレッド数 |
| `jr_train.save_audio` / `save_raw` / `save_messages` | 1 / 1 / 1 | SAVE で保存する内容 |

路線の選択(ROUTE)と AUTO / MUTE / VIEW は運転状態として `state.conf` に残る。

## 画面とソフトキー
- 上: 帯域スペクトラム(チャネルのマーカー。緑 = 同期、琥珀 = 選択・通話中)
- 中: チャネルのタイル(VOICE / SYNC / LOCK、EVM、周波数誤差、電文の数。♪ = 再生中)。タップで選択して固定(AUTO が外れる)
- 下左(VIEW で切り替え): CONST = コンスタレーション / DATA = 選択チャネルの電文(新しい順)/ OPS = 運行把握の表
- 下右: 選択チャネルの統計、再生中のチャネルと通話の一覧、負荷・欠落、保存と録音の状態
- ソフトキー: AUTO(通話の自動選択)/ MUTE / VIEW / SAVE(復号結果の保存)/ REC IQ / ROUTE / DIAG
- ROUTE(原本の「路線から選ぶ」): 区間の一覧を D 番号で絞り込み、タップで選ぶ(文字の検索はキーボードが無いので D 番号の絞り込みで代える)。
  受信する周波数(音声 2 波・データ 1 波、重複は 1 波)と共用波の有無を確かめて APPLY。受信をやり直す(保存中なら区切る)。ALL CHANNELS で全チャネルに戻す。
  区間の表は原本の収録(出典: odaQshirei 氏の Posfie まとめ、確認日 2026-10-12)。現在の運用を保証しない

## 検証(実機なし、Mac で実行)
`tests/refgen` が原本の Rust に通した結果を参照ベクトルにし、C++ が同じ入力で同じ結果になることを確かめる。
- **DSP**(`test_dsp.cpp`): 合成した 240 kS/s の帯域 IQ(5 チャネル: 帯域の両端と中央。周波数オフセット、遅延波、フェード、空きチャネル、雑音、sc16 量子化)。
  dibit・フレーム・搬送波状態・周波数誤差・EVM・コンスタレーションの各点が**ビット一致**。原本の単体テスト(系列推定・表示の観測・フレーム同期・入力の区切り方に依らないこと)も移植。
- **フレーム復号・データ**(`test_protocol.cpp`): refgen で符号化した有効な音声フレーム 400 個とデータフレーム 300 個に 0〜30 bit の誤りを入れたもの、乱数の包絡。
  受理 / 棄却・ペイロード・訂正ビット数が一致。電文は Slotted(チャネル状況・呼設定・在線・登録・転送・通告・未対応)と Common(一覧・旅客案内の非圧縮と LH5・運行把握の
  定数ブロック LH5 とハフマン LH5)を、誤りあり・なしで組立から通し、**電文の JSON 文字列と統計が一致**。LH5(切り詰め・破損)、通告文、JIS の区点、旅客案内のテキストも個別に一致。
- **音声合成**(`voice/tests`): 8 つのシナリオ(連続・欠落・index の飛び・区切り・同一ペイロード等、446 フレーム)で PCM・出力の位置とタイミングが**ビット一致**。原本の契約テストも移植。
- **パイプライン**(`test_protocol.cpp`): 音声フレーム → 欠落 → 誤りつき音声 → データ → 再同期 の列を Pipeline に通し、フレームの事象・電文・**合成した PCM**・状態・履歴・運行把握が一致。
- **通し**(`EndToEndIqMatchesRust`): 3 チャネル(音声・データ・共用波)を変調した 2.4 s の帯域 IQ を Receiver(並列)に通し、原本の `Pipeline::process` と
  フレーム・電文・**PCM**・状態が一致。CLI(`--decode-out`)の `audio.wav` と `data.jsonl` は原本の `jr-receiver decode-wave` の出力と**バイト単位で同一**。
- **前段**(`test_receiver.cpp`): 参照ベクトルを ×8 に補間して +250 kHz にずらした 1.92 MS/s を Receiver に入れ、各チャネルが原本と同等以上に同期すること。
- **通話の自動選択・保存**(`test_storage.cpp`): 原本 monitor.rs / recording.rs のテストを移植(優先順位、欠落・再同期での分割、WAV の中身、失敗時の INCOMPLETE、JSONL の行)。
- **プリセット・路線**(`test_storage.cpp`): 組み込みが原本と同じ(東 22 波・112 区間、東海 3 波・2 区間、西 19 波・19 区間、西の共用波 352.54375 MHz)、
  路線からのチャネル(変則割当の例: 中央本線 甲府〜小淵沢 D6)、外部ファイルの検査(壊れたものは全体を拒否)。
- **原本 data/mod.rs・operation.rs の単体テスト**(`test_protocol.cpp` `JrTrainDataOriginal`): 一覧の ID 数、運行把握の表、欠落・順序違い・混在・破損した部分、
  書庫の破損と表の形、再送の数え方、前置部の保持、非圧縮の旅客案内。
- **画面と App 全体(Mac で spear 全体をビルドして確認、2026-10-15)**: spear 本体を Mac でビルドするため、作業用のコピーで Linux 専用の部分だけを差し替えた
  (ALSA の AudioSink を何もしない代用品に、ELF の `.incbin` を Mach-O の書式に、`target_clones` を無効に、`RLIMIT_RTPRIO` を外す、libc++ で通らない
  `std::vector<fftwf_complex>` をテストで置き換え)。リポジトリは変えていない。
  - `ctest`: 35 件中 34 件が通過。`spear_app_tests`(JR TRAIN RADIO の起動 → 帯域の観測点 → REC IQ → SAVE → 停止)と `spear_settings_tests`(運転状態の宣言)を含む。
    落ちた 1 件は STD-T98 の `StdT98SymbolSync.RecoversSymbolsFromFractionalSpsStaircase`(JR とは無関係。arm64 での浮動小数の違いと思われる。Arch では通る前提)。
  - 画面: 参照ベクトルの通しの IQ(音声・データ・共用波)を SigMF にして再生し、スクリーンショットで確認。AUTO が音声の通話を選んで再生し、コンスタレーションが 4 点、
    データ波・共用波が SYNC、DATA に電文、OPS に運行把握の表、ROUTE の一覧(D 番号の絞り込み・選択・周波数の確認)、手で選んだときの FIXED。
    帯域スペクトラムとウォーターフォールは画面のある描画(cocoa)で表示を確認(offscreen では既存の SPECTRUM App も描かれない)。
  - QML: `qmllint` で構文の誤りなし。
- **CLI**: 原本と同じ IQ WAV を入力にしても、原本の decode-wave と `audio.wav`・`data.jsonl` がバイト単位で同一。`--routes` で路線のチャネルだけを受信。
- **並行性**: 上記を ThreadSanitizer つきで実行して警告なし。
- **ライフサイクル**(`apps/tests/test_apps.cpp` `JrTrainLifecycleAndRecordingOnSynthetic`): 合成源で start → 帯域の観測点 → REC IQ → SAVE → stop で consumer 0。
  **Mac ではビルドできないので未実行**(Qt Quick と ALSA が無い)。Arch で `scripts/ci.sh` を回すこと。
- **負荷**(Apple M5): 22 波・雑音入力・1.92 MS/s で 1 スレッド 57%、9 スレッド 13%(壁時計)。音声合成は通話 1 本あたり 1 フレーム 2.8 ms(1 コアの 7%)。
  (最適化前の値。FZ-G2 での測定と最適化は「負荷と最適化」。)worker は既定でハードウェアスレッド数 − 1(`jr_train.threads`)。

### 負荷と最適化(FZ-G2、2026-10-16)
移植直後は 22 波・雑音入力・1.92 MS/s で **1 スレッド 270 %**(実時間の 2.7 倍)で、`ctest -j4` の負荷下では
`JrTrainLifecycleAndRecordingOnSynthetic` が Lossless を取りこぼした。perf で調べ、演算順を変えずに次のように直した。

| 段 | 原因 | 対策 |
|---|---|---|
| リサンプラ(polyphase、240 kS/s → 19.2 kS/s、1 出力 360 タップ) | タップごとの `% ring_len`(64 bit 除算)が全体の 43 %。残りは 1 本の累算器の加算レイテンシ | リングを 2 回書いて窓を連続領域として読む。出力 4 つをまとめて同時に計算(帰還が無いので出力の順序以外は変わらない) |
| 適応系列推定(64 状態 × 4 枝) | スカラーで 1 状態ずつ | 64 状態をまたいで 8 レーンで計算。状態 `c + 4s` の候補は `s + 16j` なので、レーンを s にとると候補が連続し、勝者の伝送路も同じマスクで選べる(gather 不要)。伝送路は内部で SoA |
| AGC | 19.2 kS/s の 1 sample ごとに 64 点の包絡を libm の `fmaxf` / `fminf` で計算し直す | 包絡は入ったときに 1 回だけ計算し、窓の最大は SIMD で取る(最大は順序に依らない。NaN を無視するのも同じ) |
| 整合フィルタ(21 窓 × 33 タップ) | 窓ごとに 1 本の依存の鎖 | 3 窓を同時に進める。リングを 2 回書いて剰余をなくす |
| 音声合成のネットワーク層 / 相関 | 出力ごとのスカラー内積 / int64 の内積 | 重みを転置して出力をまたいで 8 レーン / 整数なので順序を変えてよい(16 bit × 16 bit を int32 で、和を int64 で) |

結果(FZ-G2、i5-10310U、AC 電源):
- 受信 DSP(22 波、雑音入力): **1 スレッド 66 %、既定のスレッド数(7)で壁時計 23 %**(`spear-jr-train-decode --threads 1|0`)。
- 音声合成: 1 フレーム(40 ms)6.6 ms → 5.2 ms、**通話 1 本あたり 1 コアの約 13 %**。残りは `envelope_impulse` の漸化式(各出力が直前の出力を最初の項に使うので、
  演算順を保つ限り逐次)と FFT。
- 一致の確認: 最適化の前後で、Demodulator の dibit・snapshot・コンスタレーションを、参照ベクトル(5 ch × profile 0/1 × ブロック長 3 通り)、
  合成(雑音 + トーン + 振幅の段差 + 無音、22 ch、CIC を通る 1.92 MS/s)で畳んだハッシュが GCC 16 と clang 23 の両方で一致。系列推定は
  ∞ のコスト・NaN と ±0 の入力・不確かな区間を含む 120,000 ステップを旧実装と突き合わせて一致。音声は 1,576 フレームの PCM が一致。
  arm64(NEON)では未確認だが、レーンをまたぐ算術をしないので命令セットに依らない(Mac の参照ベクトルのテストがビット一致を検査する)。

### 差分ファジング
`tests/refgen` の bin `fuzz` が乱数の入力(誤りを入れた音声・データフレーム、モードや外符号だけを壊したもの、乱数の包絡、壊した LH5、乱数の文字列、
壊したパケットを混ぜた電文の列、欠落と index の飛びを含む音声ペイロードの列、無信号・飽和・DC・強い隣接波・引き込み範囲外の周波数ずれ・振幅の段差・途切れの IQ)を
原本に通して結果を書き出し、`test_fuzz.cpp`(`JR_TRAIN_FUZZ_DIR` があるときだけ走る)が C++ と比べる。入力は約 50 MB(規模 1)なのでリポジトリには入れない。
```
cd apps/jr_train/tests/refgen
CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release --bin fuzz -- /tmp/jr-fuzz 5      # 規模 5
JR_TRAIN_FUZZ_DIR=/tmp/jr-fuzz ./build/apps/jr_train/tests/spear_jr_train_tests --gtest_filter='JrTrainFuzz*'
```
Mac(2026-10-15、規模 5): フレーム 300,000、LH5・文字列 325,000、電文の列 7,500(電文 14,630)、音声 150 列(PCM 19,982 フレーム)、DSP 120 ケース — **すべて一致**。

### 参照ベクトル
`tests/vectors/` は `tests/refgen` の出力そのもの(手で編集しない)。作り直すには、spear と原本を同じディレクトリに並べて(crates.io から serde_json / encoding_rs を取る):
```
cd apps/jr_train/tests/refgen
CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release -- ../vectors                                              # DSP
CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release --bin protocol -- ../vectors/protocol ../../protocol/jis0208_table.cpp
CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release --bin voice -- ../vectors/voice
```
浮動小数の結果は libm に依存するので、各ベクトルは生成した環境を `platform` に書く(現在 `macos-aarch64`)。テストは同じ環境でだけビット一致を要求し、
別の環境(Arch = `linux-x86_64`)では許容差で比べる(DSP: dibit の 1% まで、同期数 ±2 / PCM: 位置だけ / 通し: 電文は一致、音声フレーム数 ±3)。
Arch でも厳密に比べるには、Arch で上の 3 つを走らせ直す(差分が出たらそれ自体が libm の差の記録になる)。
Rust のコードを spear に置くのは §2.5 の例外ではなく、ビルドに含めない開発用ツールとして扱う(STD-T98 の `tools/*.py` と同じ位置づけ)。

## 未検証(実機で確かめること)
1. 実信号で同期・音声・電文が出るか。原本は RTL-SDR / SoapySDR で実績があるが、B210 + spear の前段は初めて。
2. LO オフセット 250 kHz とゲイン。帯域外の強い信号が前段 FIR(約 −50 dB)で十分落ちるか。
3. 搬送波ループの引き込み範囲(±497 Hz)に B210 の LO 誤差が収まるか(`radio.freq_err_ppm` の補正後。352 MHz で 1 ppm = 352 Hz)。
4. CPU 負荷: 合成入力では上記のとおり余裕がある。実機の GUI と同時、熱スロットリング時、複数の通話が同時のときの DSP 負荷と Lossless の drops。
5. 音声の再生(途切れ・遅延・AUTO の切り替え)。AudioSink の underrun。
6. フレームの radio.rx index は DSP の遅延を 58 シンボルと見積もった概算(`receiver.cpp` `kDspLatencySymbols`)。通話の優先順位は原本と同じ時刻の定義(区間の開始 + bit_offset)を使うので影響しない。
7. 画面(1920×1200 でのレイアウト、日本語の表示: 線区名・注記・電文)。`--screenshot` で撮って `docs/gui/` に残す。
8. Arch(glibc、GCC)での参照ベクトルの一致(上記「参照ベクトル」)。

## 実機テストの手順
1. Arch でビルドとテスト: `cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build`
   (`spear_jr_train_tests`、`spear_jr_train_voice_tests`、`spear_app_tests` を含む)。`scripts/ci.sh` で TSan も。必要なら refgen を Arch で走らせ直す。
2. 必要なら site.conf に `jr_train.preset=east`(東海は `central`、西日本は `west`)。
3. `./spear.sh` → メニューの JR TRAIN RADIO。アンテナは 350 MHz 帯用。
4. 見るもの: 帯域スペクトラムに信号が出るか → タイルが SYNC(緑)になるか → コンスタレーションが 4 点に集まるか → VOICE になって音が出るか(AUTO)→
   VIEW の DATA に電文、OPS に運行把握(共用波)が出るか。FREQ ERR、DSP 負荷、DROPS、AUDIO UNDERRUN。
5. SAVE で復号結果を、REC IQ で生の IQ を同時に保存する(列車の通過・通話がある時間帯に数分)。
   復号結果は `~/spear/recordings/jr_train/recording-…/`、IQ は `~/spear/recordings/jr_train_<preset>_<UTC>.sigmf-*`。
6. オフラインで再現と原本との突き合わせ:
   ```
   spear-jr-train-decode ~/spear/recordings/jr_train_east_XXXX --quiet --decode-out out/cpp --export-wav out/band.wav
   jr-receiver decode-wave out/band.wav 352603125 out/rust east        # 原本(--no-default-features でビルド可)
   cmp out/cpp/<ID>/audio.wav out/rust/<ID>/audio.wav                  # チャネルごとに audio.wav / data.jsonl が一致するか(report.json・messages.json は書式が違う)
   ```
7. 良い区間を切り出して `~/spear/golden/jr_train/` に置き、golden テスト(録音が無ければ skip)を足す。

## 移植しなかったもの
- RTL-TCP / SoapySDR 入力(spear は B210。録音は SigMF、原本の IQ WAV は CLI で読む)。GUI からの IQ WAV 再生(spear の再生は SigMF のみ)。
- 原本 session.rs(取得スレッド・チャネルごとのキュー・欠落の扱い)とそのテスト: spear の Stream Bus と Receiver が同じ役割を持つ(欠落 = discontinuity)。
- 原本の C ABI(jr_dsp.h / jr_voice.h)とそのテスト、Tauri の UI と設定の保存。
- 原本 UI の文字検索・一覧の表示の一時停止(タッチのみの画面に合わせて D 番号の絞り込みにした)。
- CLI の report.json / messages.json は原本と同じ内容だが書式(整形・浮動小数の表記)が違う。比べるのは audio.wav と data.jsonl。

## 次の段階
1. 実機で見つかった問題の修正、golden テスト。
2. 複数チャネル音声の同時再生(STD-T98 の Mixer を appfw に抽出する。2 つ目の利用者)。原本は 1 チャネルのみ。

## ライセンス
`apps/jr_train/` の移植したファイル(dsp / protocol / voice / pipeline / call_monitor / storage / presets)は原本(MIT)の派生物。
各ファイルに `SPDX-License-Identifier: MIT` と著作権表示を残し、`apps/jr_train/LICENSE.MIT` に原本の許諾文を置く。MIT のコードは GPL-3.0 の spear に結合できる。
`voice/celp/` は原本が mcelp-codec(github.com/soltia48/mcelp-codec、MIT)から取り込んだもので、`voice/celp/LICENSE` を残す。
`voice/models/*.bin` は原本の係数そのまま(原本のライセンスに従う)。`protocol/jis0208_table.cpp` は WHATWG Encoding Standard の index(CC0)に由来する。
原本の LH5 とフレーム復号には「reference から移植」とあるが、出典の記載は無い。リポジトリ全体が MIT で公開されているので、これらも MIT として扱う(2026-10-15 決定)。
