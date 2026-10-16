# 現状と次の作業(2026-09-27 時点)

検証済みのことと未完のことを分けて記録する。事実が変わったら更新する。
要件は `docs/requirements.md`(v5.0)の §番号で参照する。

## マイルストーン(§12)

| | 内容 | 状態 | 根拠 |
|---|---|---|---|
| M-1 | 電源管理による断続的な欠落の確認(10 Msps 30 分) | 完了 | `docs/m1/RESULTS.md`: (a) AC + performance、(b) バッテリー + 既定、ともに損失 0 |
| M0 | Stream Bus(GUI なし) | 完了 | `tools/headless`、`tests/test_stream_bus.cpp`(Lossless 不変条件、LatestOnly の drop 計数、EOS) |
| M1 | Recording / Playback | 完了 | SigMF + sidecar。`RecordingSource` はループ再生、retune 不可(録音条件が真値)。IQ RECORDER App |
| M2 | 単純な RF App | 完了 | SPECTRUM(422.2 MHz の実信号で軸と絶対値を確認)、FM/AM RX(実信号の FM 復調を実機で確認) |
| M3 | STD-T98 App | 完了 | 実録音・実機ともにフレーム抽出から AMBE 音声まで。秘話は自己スクランブルした実音声で Python と一致し、実機の秘話呼でも鍵が見つかり復号できた(`docs/apps/std_t98.md`) |
| — | JR TRAIN RADIO(JR 在来線デジタル列車無線) | 実機確認前 | 原本(Rust)の全機能を C++ に移植: 受信 DSP、同期、音声の誤り訂正と合成、データ電文、運行把握、通話の自動選択、保存。合成信号で原本とビット一致を確認(`docs/apps/jr_train.md`) |
| — | ADS-B App(§7.2) | 完了 | 実機で受信。実録音 golden(羽田近傍 30 s で 1223 フレーム、6 機)。8 Msps をフルレートで消費して drops 0(`docs/apps/adsb.md`) |
| — | SLPR 20CH RX(特定小電力 20ch 同時受信) | 完了 | 合成 NFM で 2 ch 同時・隣接・provenance をテストで固定。実機で ch12 の実信号を受信、drop 0(`docs/apps/slpr.md`) |

## コンポーネントと検証状態

| 領域 | 内容 | 検証 |
|---|---|---|
| `core/` Radio | UHD を扱う唯一のコンポーネント。stage 0–6 の自己診断、`DeviceState`(判定元: libusb 列挙 / FX3 レジスタ / センサ / UHD ログ)、timed retune、AGC | 実機。USB 抜き差しからの再接続はパッチ済み libuhd が必要(`packaging/libuhd`) |
| `core/` Stream Bus | `Stream<T>` / `Subscription`、Lossless(溢れは event + flag で報告)/ LatestOnly、`BlockPool` + opaque `BlockRef` | gtest + TSan(`scripts/ci.sh`) |
| `core/` Sources | `B210LiveSource`(再接続・watchdog・generation)、`SyntheticSource`(トーン / FM / AM)、`RecordingSource`(SigMF) | gtest、実機 |
| `core/` その他 | `EventBus`(履歴、dispatch thread)、`TAP()`、`HealthMonitor`、`EventLogFile`、`AudioSink`(ALSA / PipeWire) | gtest、実機 |
| `dsp/` 共通 | `design_lowpass`、`FirDecimator` / `FirInterpolator`(ベクトル化カーネル)、`PfbChannelizer`、`SpectrumEstimator`、`stage.hpp`(StageInfo / Provenance)、FFTW プランナの mutex | gtest(参照ベクトル) |
| `appfw/` App SDK | `GuiApp`(on_start / on_stop は GUI thread で呼ぶ、`configure(QVariantMap)`、`persist({...})`)、`SettingsStore`(運転状態の復元・保存 `~/spear/state.conf`)、ビルド時レジストリ、`ViewProcessor` / `ViewSource`(任意の複素 stream → spectrum / waterfall。nfft より短いブロックは溜めて使う。手動レンジは再起動後も残る)、`TraceSource` + `EyeDiagramItem`、`SpectrumItem` / `WaterfallItem`(自前の QRhiTexture リング) | `apps/tests/test_apps.cpp`(全 App のライフサイクル)、`test_settings.cpp`(保存の往復、全 App の宣言の解決) |
| `widgets/ input/ theme/` | `SpectrumView`(markers / compact)、`EyeDiagram`、`ConstellationView`(JR TRAIN RADIO で追加)、`Readout`、`LevelMeter`、`Spear.Input`(KeyButton / Keypad / StepKeys / ValueField / NumericEntry / ChoiceEntry / Feedback(操作音の入口)。タッチ専用)、`Theme` | スクリーンショットで目視(`docs/gui/*.png`) |
| `gui/` シェル | スプラッシュ(起動時に自己診断 → 装置待ち → FPGA ロード(進捗表示)→ tune 確認。完了まで App を始めない)、メニュー(装置共通の GAIN と RX PORT(受信端子 TRXA / RXA / RXB / TRXB)。center / rate は App が決める)、ステータスバー(端子も常に表示)、ソフトキー、DIAGNOSTICS、数値入力・選択モーダル、操作音(`TapSound`: タップ音・拒否音)、起動音(`BootSound`。鳴り終わるまでスプラッシュに留まる)、`--set key=value`、`--screenshot` 検証ハーネス | 実機・合成。FPGA 進捗バーは、FPGA 未構成の状態からの起動ではまだ確認していない |
| `apps/spectrum` | 汎用。FREQ / RATE(再起動)/ REF / AVG / HOLD | 実信号で確認済み |
| `apps/recorder` | Lossless 録音、sidecar。録音中は FREQ 無効 | gtest(drop 0)、実機 |
| `apps/demod` | NFM / WFM / AM。LO を RX から 250 kHz 離す。channel IQ と audio を Stream Bus に publish | 合成 FM トーンの gtest、実信号の FM |
| `apps/std_t98` | 30ch 受信機(C++ のみ、Qt / UHD 非依存)、プロトコル、AMBE(C++ に移植)、秘話(PN、ffnn の C++ 推論、hybrid の ONNX Runtime 推論、鍵探索ワーカー)、GUI(帯域スペクトラム / チャネル格子 / アイ / フレーム / 鍵 / 全 ch 同時音声) | 実録音 golden(`~/spear/golden/std_t98`)、Python 参照実装との一致(プロトコル・AMBE・秘話)、実機の送信で確認(平文・秘話) |
| `apps/adsb` | 1090 MHz Mode S / ADS-B 受信機(C++ のみ: 回転 → 低域 FIR → 電力 → プリアンブル / PPM → CRC-24、DF17/18/11 + AP 形式)、航空機表(CPR 偶奇 / 局所)、GUI(表 / ミニマップ(埋め込みのオフライン地図 Natural Earth + OurAirports。全機が入るよう自動で範囲を合わせる)/ 電力窓 / 統計)。radio.rx を 8 Msps のまま Lossless で消費 | 既知ベクトル(The 1090MHz Riddle)、合成 PPM で provenance をサンプル単位で確認、ライフサイクル、実録音 golden(`~/spear/golden/adsb`、羽田近傍 3 s / 136 フレーム)、実機で動作(`docs/apps/adsb.md`) |
| `apps/slpr` | 特定小電力(422 MHz 帯、単信 20 ch、ICOM 準拠の 01–20)の同時 NFM 受信機(C++ のみ: 回転 → resamp1 → PFB 32 × 12.5 kHz → quadrature demod → LPF → DC 阻止 → 床相対 squelch(attack / hold))、加算ミキサ、GUI(帯域スペクトラム / 20ch 格子 / 選択 ch の IQ) | 合成 NFM(2 ch 同時、隣接の漏れ、provenance、hold)、ライフサイクル、実機で ch12 の実信号(`docs/apps/slpr.md`) |
| `apps/jr_train` | JR 在来線デジタル列車無線の多波受信機(C++ のみ、Qt / UHD 非依存): 回転 + FIR ÷8(1.92 MS/s → 240 kS/s)→ チャネルごとの Pipeline(π/4-DQPSK 復調・64 状態 MLSE・フレーム同期・音声の誤り訂正・CELP + GRU/MLP の音声合成・データ電文・LH5・運行把握)。worker スレッドで並列。通話の自動選択、保存(WAV / JSONL)。GUI(帯域スペクトラム / タイル / コンスタレーション・電文・運行把握 / SAVE / REC IQ)。プリセット east / central / west | 原本(Rust)とのビット一致(Mac、`tests/vectors`、refgen)、差分ファジング(規模 5 ですべて一致)、原本の単体テストの移植、TSan。Mac で spear 全体をビルドしてライフサイクルテストと画面を確認。Arch(FZ-G2)でもビット一致、実機で負荷(22 波 20 分)と画面を確認。**実信号での受信は未確認**(`docs/apps/jr_train.md`) |
| `tools/` | `spear-soak`(M-1)、`spear-headless`(M0)、`spear-std-t98-decode`(録音 → フレーム表 / WAV / ペイロード。秘話呼は鍵を探索して復号)、`spear-adsb-decode`(録音 → フレーム表 / 航空機一覧 / golden)、`spear-jr-train-decode`(録音 → 音声・電文・統計。原本の decode-wave と同じ形式の出力、保存、frames、WAV) | — |

## 未完の作業・既知の問題

1. STD-T98 秘話の golden: 実機の秘話呼で動作は確認した(2026-09-17)。その録音の抜粋(鍵つき)を `~/spear/golden/std_t98/` に加え、受信 → 鍵探索 → 復号を通しで回帰テストにする。現在の E2E テストは、平文の実音声を自分でスクランブルしたもの。
2. ADS-B(2026-09-17 実装・実機確認、`docs/apps/adsb.md`): 地上位置(TC5–8)の CPR、多数機のときの表の差分更新 model、受信機 reset の SDK 規約(generation 切替時)、ミニマップの `Spear.Widgets` への抽出(2 つ目の利用者が出たら)が残っている。
3. STD-T98 の実地確認: 2026-09-20 のイベントで「遠い局が途切れる / 空き ch が開く」症状が出た。原因は AGC で揺れる床に絶対閾値のスケルチが乗っていたこと。床相対スケルチと出力ゲートに直した(2026-09-21、`docs/apps/std_t98.md`「スケルチ」)。次の実地で AGC OFF(手動ゲイン)との比較と、弱い局の聞こえ方を確認する。引き込み(`max_deviation` の単位の修正後)は録音解析で 0.1〜0.4 s と確認した。
4. IQ RECORDER: 録音一覧と再生元の選択(`Spear.Input` に ListPicker が必要)、ディスク残量による自動停止。
5. Source API: 運転中の gain / bandwidth / 受信端子の変更。現在は `retune` だけで、gain と端子はメニューで決めて App 起動時に適用し、rate の変更は App を再起動する。
6. 性能: STD-T98 は 4 Msps で DSP thread が約 25–30 %(1 コア)。B210 の FPGA ロードは USB 2.0 で約 70 s。FZ-G2 は熱スロットリングが頻繁に起きる(79 °C)。損失に直結しないことは M-1 で確認したが、DSP 負荷の表示には注意する。
7. 共通 DSP への抽出: quadrature demod が 3 つの App(DEMOD / STD-T98 / SLPR)に、一次 IIR(DC 阻止)と床相対 squelch が 2 つの App に書かれている。`docs/dsp-boundary.md` の昇格ルール(2 つ目で抽出)に従って `dsp::quadrature_demod` などへ抽出する(数値は変えず、STD-T98 の golden で確認する)。
8. SLPR の実地確認: 音声の聴感、2 局同時のミックス、会場での AGC と手動ゲインの比較(`docs/apps/slpr.md`)。
9. 要件との差分:
   - §7.1 では App がシェルの RF 草案を受け取るとしていたが、center / rate は App が決める形に変えた(2026-09-17、誤設定の防止)。草案は最後に Core が運転していた RF に追従し、汎用 App の初期値にだけ使う。
   - `RecordingSource` は `configure` / `retune` で録音条件を変えない(要求は Warning を出して無視する)。
   - M3 の順序: ADS-B より先に STD-T98 を実装した。
   - §7.1 の宣言項目 `antenna` は `RfConfig::port`(装置パネルの端子名)で表す。UHD の frontend(subdev)と antenna への分解は Radio だけが行う。

8. JR TRAIN RADIO(2026-10-15 移植、実機なし): Arch でのビルド・テスト(参照ベクトルは Mac 生成。厳密比較には refgen を Arch で再実行)、実機での同期・音声・電文・負荷・画面の確認、golden の録音(手順は `docs/apps/jr_train.md`「実機テストの手順」)。

## 決定の記録

理由を添えて記録する。覆すときも理由を書く。

- libuhd にパッチを当てる(0001: USB 消失時に deleter から throw しない、0002: b2xx probe と fx3_state の公開)。upstream に PR は出さない。共用の開発機にはフル構成のパッチ版だけを入れる。B200-only 版は Appliance 用。
- GUI はタッチ操作のみ(`docs/gui/DESIGN.md`)。OS の仮想キーボードは使わない。1920×1200 の固定キャンバス。
- 状態の所有者は 1 つ(`docs/state-ownership.md`)。周波数が 4 か所にコピーされていた不具合をきっかけに規約にした。
- 運転状態の保存は SDK の仕組み 1 つ(`SettingsStore` + `persist()` の宣言)で行い、状態の所有者は増やさない(2026-09-17。再起動のたびにスケルチ / AGC が初期化される不具合から)。優先順位は 既定 < `state.conf` < site.conf。録音の再生中は周波数 / レートを復元しない(録音条件が真値)。
- 共通 DSP の境界(`docs/dsp-boundary.md`): 変調方式名の付く部品は App 内に置き、共通化は抽出で行う。
- `dsp::Provenance` は群遅延を引く(2026-09-17、ADS-B の合成 PPM テストで発覚)。段は因果的なので、出力 index から入力 index への変換は −群遅延になる。以前は足していた。
- DSP カーネルは既定の x86-64 でビルドし、`target_clones` で AVX2 / FMA 版も持つ(2026-09-17、`dsp/src/fir.cpp`)。`-march=native` は golden の丸めを全体で変えるので使わない。`FirDecimator` の decim 1 はタップを外側にした axpy で実装する(8 Msps × 47 tap で処理時間が 1/2)。
- 操作音(2026-09-18): 入力を受け付けたときにタップ音、数値入力が値を拒否したときに拒否音を鳴らす。空白のタップ、モーダルの背景、無効なキー、ドラッグ / ピンチ / フリックでは鳴らさない。
  音はシェルが持つ core の `AudioSink` 1 つで出す(`gui/src/ui_audio.cpp`、ALSA バッファ 50 ms、App の音声とは PipeWire がミックスする)。Qt Multimedia は依存が増えるので使わない。
  QML 側の入口は `Spear.Input` の `Feedback` singleton(信号だけで C++ に依存しない)。音量や ON/OFF の設定は置かない(音量は OS 側で調整する)。`--set audio_device=null` で App の音声ごと無音にできる。
- App は音量調整を持たない(常に 100 %、MUTE のみ)(2026-09-18)。`AudioSink` から gain を外し、DEMOD / STD-T98 の VOL キーと `volume` の保存を削除した。現在は OS(PipeWire)の音量で足り、キオスク化した後も音量はシステム側の 1 か所で扱うほうがよい。App ごとに音量があると二重になる。
- メニューに EXIT(確認ダイアログつき)を置く(2026-09-17)。KDE Plasma などでキオスク化せずに全画面で起動していると、閉じる手段が無いため。キオスク化したら見直す(`gui/qml/Main.qml` のコメント)。
- 起動時に装置の準備を済ませてから App を選べるようにする(2026-09-17、`Source::warm_up`)。以前は最初の App を起動するまで状態が UNKNOWN で、FPGA の書き込み中でも App を開始できた。UHD は `multi_usrp::make` で FPGA を書くので open → close だけでよく、libuhd のパッチは不要。
- 位置・方位を前提にしない(2026-09-17、ADS-B)。可搬機で GPS も方位センサも無いため。地図の範囲は受信した機体の位置だけから決め(自動フィット)、地図データはリポジトリに同梱してバイナリに埋め込む(オフライン、パブリックドメインの Natural Earth + OurAirports)。
- 高頻度の事象はフレームごとに Event にしない(ADS-B では新規機 / 初回位置 / 消失だけ。フレーム数は統計で出す)。イベントログが溢れるのを防ぐ。
- SLPR(特定小電力)は STD-T98 と同じ骨格の App として追加し、squelch に attack(20 ms)を足した(2026-09-27、`docs/apps/slpr.md`)。4 Msps、LO = 422.1750 − 0.25 MHz、PFB 32 × 12.5 kHz(16 tap/branch。隣接の強い局が漏れて開かないようにするため)。復調も音声も 12.5 kHz のままで、リサンプルは PipeWire に任せる。全チャネルの音声が同じ歩みで出るのでミキサは加算だけでよい。attack は、1 ブロックだけの過渡(隣のキーアップ、インパルス雑音)で 150 ms の雑音を鳴らさないため(合成テストで隣接チャネルが 1 ブロックだけ開いたことから)。`ViewProcessor` が nfft より短いブロックを捨てていた不具合は、この App の 12.5 kHz の観測点で見つかり、溜める形に直した。
- STD-T98 のスケルチは床相対(全 ch 電力の中央値 + margin)にし、DSP は止めずに出力だけをゲートする(2026-09-21)。絶対 dB の閾値は AGC が効いていると床のすぐ上に乗って毎秒 30 回近く開閉し、閉で IQ を 0 にしていたのでシンボル同期が開くたびに引き込み直しになった(2026-09-20 の実地録音)。CRC が通ったフレームは閉でも出して開く(遠い局)。会場では AGC より手動ゲインの方が良いと見込んでいる(未検証)。
- 受信系の golden には実録音を使う。合成の変調器は規格に無いパラメータの調整になるので作らない(STD-T98 で試して失敗した)。
- 個体の設定は `~/spear/site.conf` に置く(例: `radio.freq_err_ppm=<ppm>`)。コードにも要件にも書かない。
- 個体の LO 誤差は Core の Radio が ppm 単位で LO 側で補正する(2026-09-19、`core/include/spear/core/lo_correction.hpp`)。
  以前は STD-T98 だけが `std_t98.freq_err_hz` で IQ を回転していたため、SPECTRUM の軸、DEMOD、録音の周波数ラベルはずれたままだった。
  B2xx は RF LO も ADC クロックも同じ TCXO から作るので、誤差は周波数に比例する(ppm)。`Radio` が要求を装置の目盛りに換算して tune し、読み値を真の周波数に戻すので、`RfConfig` / `config()` / event / `StreamMeta` / SigMF はすべて真の周波数のままで、どの App も変更なしで正しくなる。
  ソフトウェアでの回転(Source 内や共通 reader)は採らない(CPU 負荷、sc16 の再量子化、録音が生データでなくなるため)。補正前の録音は読み替えない(撮り直す)。
  サンプルレートの ppm 誤差(4 Msps で約 12 Hz)は補正できず、シンボル同期が吸収する。録音の SigMF global に `spear:lo_correction_ppm` を記録する。
- 受信端子(RX PORT)は装置共通の設定としてメニューに置き、GAIN と同じく App 起動時に適用する(2026-09-19、`core/include/spear/core/rf_port.hpp`)。
  - B210 の 4 端子(TRXA / RXA / RXB / TRXB)は、UHD では frontend(subdev spec `A:A` / `A:B`)と antenna(`TX/RX` / `RX2`)の 2 段で指定する。Spear の内部(`RfConfig::port` / 保存キー `shell.draftPort` / GUI / SigMF `spear:rx_port`)では装置パネルの名前 1 つで扱い、分解は `Radio::tune_rx` だけが表を見て行う。
  - subdev spec は tune の最初の装置呼び出しで設定する(能力の照合もセンサも channel 0 から spec 経由で解決されるため)。frontend の存在は事前に tree で確認する。libuhd の `coerce_subdev_spec` は B200 / B20xmini で `B` を黙って `A` に書き換えるので、例外を待つ方法だと RXB が RXA になってしまう。
  - 既定は UHD の既定と同じ RXA。運転中に変更する API は作らない(メニューは App 停止中にしか表示されない。GAIN と同じ)。
  - 実機確認(2026-09-19、5IWG5D5): 4 端子とも stage 4 で `port=… lo_locked`、12 s のストリームで Lossless drop 0(`spear-headless --port`)。GUI でも warm-up の tune check と App 起動が選んだ端子で通った(`--port TRXB` / `RXB` で SPECTRUM を運転し、ステータスバーに端子名が出る)。端子 LED での対応の目視確認は App の運転中に行う(起動時には点かない)。
- AMBE は mbelib-neo の AMBE 経路だけを C++ で書き直した(IMBE / SIMD / pffft なし)。pyambelib と PCM が ±1 LSB で一致する。
- ライセンスは GPL-3.0 で公開する(2026-09-17)。GPL-2.0-or-later 由来のコードと結合してよい。
- STD-T98 の LO は帯域中心 − 250 kHz にする(ゼロ IF の DC スパイクを ch16 に重ねないため)。回転量は Core の実際の LO から求めるので、録音の再生(LO 固定)と実機で同じコードが通る。
- 秘話モデル(§2.5 の道 1 と道 2、2026-09-17): ffnn は C++ で再実装(safetensors を直接読む)、hybrid は ONNX Runtime(必須の依存)で動かす。両モデルともリポジトリに置き、バイナリに埋め込む。
  秘話の解除は App の主要機能で、モデルは作者が学習したもの(ライセンスは本体と同じ)なので、ビルドオプションにも外部ファイルにもしない。
  鍵探索は専用のワーカースレッド 1 本で行う(DSP thread では行わない。ORT のスレッドプールも作らない)。tools のような別プロセス + UDS の構成は採らない。
  SACCH の CRC が不良のフレームでは秘話の判定を保持する(tools は平文として扱い、セッションを破棄していた)。

## 装置の事実(個体によらないもの)

- 開発機と対象機は同じ FZ-G2(i5-10310U, 8 GB, Arch, Plasma Wayland scale 1.5。GUI は 1920×1200 の固定キャンバスを縮小表示)。
- B210(LibreSDR 互換機でも同じ)は USB 2.0 で使う。FPGA のロードは約 70 s。
- `ref_locked` は外部 10 MHz 用 PLL のロック状態(internal では unlocked が正常)。ストリーム中に FX3 状態レジスタを読むと約 13 ms バルク転送が止まるので、状態遷移時と timeout 時にだけ読む。
- B2xx 個体の LO 誤差は数 ppm(351 MHz で数百 Hz〜1 kHz)になりうる(STD-T98 の 6.25 kHz チャネルでは無視できない)。`site.conf` の `radio.freq_err_ppm` に設定すると Radio が LO 側で補正する。
  値の求め方: 補正なし(未設定)で STD-T98 を動かし、受信中のチャネルの「FREQ ERR」[Hz] ÷ LO [MHz] を ppm とする(符号はそのまま。例: +1030 Hz / 351.04 MHz = +2.93)。設定後は FREQ ERR がほぼ 0 になる。
  適用中の値は DIAGNOSTICS の「LO CORR」と、warm-up の tune check の行に表示される。
- 受信端子と UHD の対応(`rf_port.hpp` が正): TRXA = `A:A` `TX/RX`、RXA = `A:A` `RX2`、RXB = `A:B` `RX2`、TRXB = `A:B` `TX/RX`。
  選んだ端子の LED は App の運転中(rx streamer がある間)だけ点く(ATR が streamer の有無で RX 状態を決めるため)。起動時の warm-up の tune check(open → tune → close)では点かない。B200 系(frontend A のみ)で RXB / TRXB を選ぶと stage 4 で FAULT になる。
- Qt のログは journald に出る。端末に出すには `QT_FORCE_STDERR_LOGGING=1` を付ける。
- 記録・ログ・golden・音声は `~/spear/`(`recordings / logs / golden / audio / site.conf`)に置く。個体固有の値、録音、音声由来の golden(`golden/std_t98/ch3_pich.*`, `ambe_golden.txt`, `ch3_payloads.txt`, `secret_golden.txt`)はリポジトリに入れない(golden テストは無ければ skip)。
