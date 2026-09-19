# 現状と次の仕事(2026-09-17 時点)

現状を知りたい人はここから。**「何が検証済みか」と「何が未完か」を分けて書く**。更新は事実が変わったときに。
要件書は `docs/requirements.md` v4.1、§番号で参照。

## マイルストーン(§14)
| | 内容 | 状態 | 根拠 |
|---|---|---|---|
| M-1 | 電源管理由来の断続性確認(10 Msps 30 分) | **完了** | `docs/m1/RESULTS.md`: (a) AC+performance、(b) バッテリー+既定、ともに損失 0 |
| M0 | Stream Bus(GUI なし) | **完了** | `tools/headless`、`tests/test_stream_bus.cpp`(Lossless 不変条件、LatestOnly の drop 計数、EOS) |
| M1 | Recording / Playback | **完了** | SigMF + sidecar、`RecordingSource` はループ再生・retune 不可(録音条件が真値)、IQ RECORDER App |
| M2 | 単純な RF App | **完了** | SPECTRUM(off-air 422.2 MHz で軸と絶対値を確認)、FM/AM RX(off-air FM 復調を実機で確認) |
| M3 | STD-T98 App | **完了** | 実録音・実機ともにフレーム抽出 → AMBE 音声。秘話は自己スクランブルした実音声で Python と等価、実機の秘話呼でも鍵が見つかり復号できた(`docs/apps/std_t98.md`) |
| — | ADS-B App(§8.2) | **完了** | 実機で受信、実録音 golden(羽田近傍 30 s で 1223 フレーム、6 機)。8 Msps フルレート消費で drops 0(`docs/apps/adsb.md`) |

## コンポーネントと検証状態
| 領域 | 内容 | 検証 |
|---|---|---|
| `core/` Radio | UHD の唯一の所有者。stage 0–6 の自己診断、`DeviceState`(一次ソース: libusb 列挙 / FX3 レジスタ / センサ / UHD ログ)、timed retune、AGC | 実機。USB 抜挿再接続は **パッチ済み libuhd** が前提(`packaging/libuhd`) |
| `core/` Stream Bus | `Stream<T>` / `Subscription`、Lossless(溢れは event + flag で必ず表面化)/ LatestOnly、`BlockPool` + opaque `BlockRef` | gtest + TSan(`scripts/ci.sh`) |
| `core/` Sources | `B210LiveSource`(再接続・watchdog・generation)、`SyntheticSource`(トーン/FM/AM)、`RecordingSource`(SigMF) | gtest、実機 |
| `core/` その他 | `EventBus`(履歴、dispatch thread)、`TAP()`、`HealthMonitor`、`EventLogFile`、`AudioSink`(ALSA/PipeWire) | gtest、実機 |
| `dsp/` 共通 | `design_lowpass`、`FirDecimator` / `FirInterpolator`(ベクトル化カーネル)、`PfbChannelizer`、`SpectrumEstimator`、`stage.hpp`(StageInfo / Provenance)、FFTW プランナ mutex | gtest(参照ベクトル) |
| `appfw/` App SDK | `GuiApp`(on_start/on_stop は GUI thread 保証、`configure(QVariantMap)`、`persist({...})`)、`SettingsStore`(運転状態の復元・自動保存 `~/spear/state.conf`)、ビルド時レジストリ、`ViewProcessor/ViewSource`(任意の複素 stream → spectrum/waterfall、手動レンジは再起動をまたぐ)、`TraceSource` + `EyeDiagramItem`、`SpectrumItem` / `WaterfallItem`(自前 QRhiTexture リング) | `apps/tests/test_apps.cpp`(全 App のライフサイクル)、`test_settings.cpp`(往復・全 App の宣言解決) |
| `widgets/ input/ theme/` | `SpectrumView`(markers / compact)、`EyeDiagram`、`Readout`、`LevelMeter`、`Spear.Input`(KeyButton / Keypad / StepKeys / ValueField / NumericEntry / ChoiceEntry / Feedback(操作音の入口)、タッチ専用)、`Theme` | スクリーンショットで目視(`docs/gui/*.png`) |
| `gui/` シェル | スプラッシュ(起動時の立ち上げ: 自己診断 → 装置待ち → FPGA ロード(進捗)→ tune 確認。完了まで App を始めない)/ メニュー(装置共通の GAIN と RX PORT(受信端子 TRXA / RXA / RXB / TRXB)、center/rate は App が決める)/ ステータスバー(端子も常時表示)/ ソフトキー / DIAGNOSTICS / 数値入力・選択モーダル / 操作音(`TapSound`: タップ音・拒否音)/ 起動音(`BootSound`、鳴り終わるまでスプラッシュに留まる)、`--set key=value`、`--screenshot` 検証ハーネス | 実機・合成。FPGA 進捗バーは未構成からの起動でまだ未確認 |
| `apps/spectrum` | 汎用。FREQ / RATE(再起動)/ REF / AVG / HOLD | off-air 確認済み |
| `apps/recorder` | Lossless 録音、sidecar、録音中は FREQ 無効 | gtest(drop 0)、実機 |
| `apps/demod` | NFM/WFM/AM、LO を RX から 250 kHz 離す、channel IQ と audio を Stream Bus に publish | 合成 FM トーン gtest、off-air FM |
| `apps/std_t98` | 30ch 受信機(純 C++、Qt/UHD 非依存)、プロトコル、AMBE(C++ 化)、秘話(PN、ffnn C++ 推論、hybrid ONNX Runtime、鍵探索ワーカー)、GUI(帯域スペクトラム / チャネル格子 / アイ / フレーム / 鍵 / 全 ch 同時音声) | 実録音 golden(`~/spear/golden/std_t98`)、Python 参照との等価(プロトコル・AMBE・秘話)、実機で送信確認(平文・秘話) |
| `apps/adsb` | 1090 MHz Mode S / ADS-B 受信機(純 C++: 回転 → 低域 FIR → 電力 → プリアンブル / PPM → CRC-24、DF17/18/11 + AP 形式)、航空機表(CPR 偶奇 / 局所)、GUI(表 / ミニマップ: 埋め込みのオフライン地図(Natural Earth + OurAirports)に全機が収まるよう自動フィット / 電力窓 / 統計)。radio.rx を 8 Msps フルレートで Lossless 消費 | 既知ベクトル(The 1090MHz Riddle)、合成 PPM で provenance をサンプル単位、ライフサイクル、実録音 golden(`~/spear/golden/adsb`、羽田近傍 3 s / 136 フレーム)、実機で動作(`docs/apps/adsb.md`) |
| `tools/` | `spear-soak`(M-1)、`spear-headless`(M0)、`spear-std-t98-decode`(録音 → フレーム表 / WAV / ペイロード、秘話呼は鍵探索して復号)、`spear-adsb-decode`(録音 → フレーム表 / 航空機一覧 / golden) | — |

## 未完・既知の欠陥・次の仕事
1. **STD-T98 秘話の golden**: 実機の秘話呼で動作確認済み(2026-09-17)。その録音の抜粋(鍵つき)を `~/spear/golden/std_t98/` に加え、
   受信 → 鍵探索 → 復号を通しで回帰テストにする(現状の E2E は平文の実音声を自己スクランブルしたもの)。
2. **ADS-B**(2026-09-17 実装・実機確認済み、`docs/apps/adsb.md`): 残りは地上位置(TC5–8)の CPR、表の差分更新 model(多数機のとき)、
   受信機 reset の SDK 規約(generation 切替)、ミニマップの `Spear.Widgets` への抽出(2 つ目の利用者が出たら)。
3. **STD-T98 の実機での引き込み**: `max_deviation` の単位バグ修正後、毎回の送信で即ロックすることを継続確認。
   もし再発したら FRAME LOG と SPS(26.02〜26.06 のはず)を記録。
4. **IQ RECORDER**: 録音一覧 / 再生元選択(`Spear.Input` に ListPicker が要る)、ディスク残量による自動停止。
5. **Source API**: 運転中の gain / bandwidth / 受信端子の変更(現状は `retune` のみ。gain / 端子はメニューで決めて App 起動時に適用、rate 変更は App 再起動)。
6. **性能の余裕**: STD-T98 は 4 Msps で DSP thread 約 25–30 %(1 コア)。B210 の FPGA ロードが USB 2.0 で約 70 s。
   FZ-G2 は熱スロットリングが頻発する(79 °C)— 損失には直結しないことを M-1 で確認済みだが、DSP 負荷表示を見る。
7. **要件との差分**(要件書は書き換えない):
   - §8.1 の「シェルの草案 RF を App が受け取る」は、center/rate を App が決める形に変更(2026-09-17、誤設定防止)。草案は
     「最後に Core が運転していた RF」に追従し、汎用 App の初期値にだけ使う。
   - `RecordingSource` は `configure/retune` で録音条件を変えない(要求は Warning で無視)。
   - M3 の順序: ADS-B を飛ばして STD-T98 を先行。
   - §8.1 の宣言項目 `antenna` は `RfConfig::port`(装置パネルの端子名)。UHD の frontend(subdev)と antenna への分解は Radio だけが行う。

## 決定の記録(理由つき、覆すなら理由を書く)
- **libuhd を改造**(0001: USB 消失時に deleter から throw しない、0002: b2xx probe + fx3_state 公開)。upstream PR は出さない。
  共用開発機にはフル構成パッチ版だけを入れる。B200-only は appliance 用。
- **GUI はタッチのみ・計器の文法**(`docs/gui/DESIGN.md`)。OS 仮想キーボード不採用。1920×1200 固定キャンバス。
- **状態の単一所有**(`docs/state-ownership.md`)— 4 か所に周波数がコピーされていた不具合から規約化。
- **運転状態の保存は SDK の 1 つの仕組み(`SettingsStore` + `persist()` 宣言)で、所有者は増やさない**(2026-09-17、「再起動のたびに
  スケルチ / AGC が初期化される」不具合から)。優先順位 既定 < `state.conf` < site.conf。録音再生では周波数/レートを復元しない(録音条件が真値)。
- **共通 DSP の境界**(`docs/dsp-boundary.md`)— 変調方式名の付いた部品は App 内、昇格は抽出で。
- **`dsp::Provenance` は群遅延を引く**(2026-09-17、ADS-B の合成 PPM テストで発覚)。段は因果なので出力 index → 入力 index は −群遅延。以前は足していた。
- **DSP カーネルは既定 x86-64 で組み、`target_clones` で AVX2/FMA 版を併せ持つ**(2026-09-17、`dsp/src/fir.cpp`)。`-march=native` は golden の丸めを
  全体で変えるので採らない。`FirDecimator` の decim 1 はタップ外側の axpy(8 Msps × 47 tap で 1/2)。
- **操作音は「受け付けた」事象にだけ(タップ音)、数値入力の拒否には別の音(拒否音)**(2026-09-18)。空白のタップ・モーダル背景・無効キー・
  ドラッグ/ピンチ/フリックでは鳴らさない。音源は core の `AudioSink` をシェルが 1 つ持つだけ(`gui/src/ui_audio.cpp`、ALSA バッファ 50 ms、
  PipeWire が App 音声とミックス)。Qt Multimedia は依存が増えるだけなので採らない。QML 側の入口は `Spear.Input` の `Feedback` singleton(信号だけ、C++ 非依存)。
  音量・ON/OFF の設定は置かない(音量は OS 側、という前の決定と同じ理由)。`--set audio_device=null` で App 音声ごと無音にできる。
- **App に音量調整を持たせない(常に 100 %、MUTE のみ)**(2026-09-18)。`AudioSink` から gain を外し、DEMOD / STD-T98 の VOL キーと `volume` の保存を削除。
  Arch Linux 上で動かしている現状は OS(PipeWire)の音量で足り、将来キオスク化するときも音量はシステム側の 1 か所で一貫して扱うべきで、
  App ごとの音量があると二重になる。
- **メニューに EXIT(確認ダイアログつき)を置く**(2026-09-17)。KDE Plasma 等で非キオスクのままフルスクリーン起動している現状では
  閉じるのにひと手間かかるための策。将来キオスク化するなら EXIT という UI が最適とは限らない(`gui/qml/Main.qml` のコメント)。
- **起動時に装置を立ち上げてから App を選ばせる**(2026-09-17、`Source::warm_up`)。以前は最初の App 起動まで状態が UNKNOWN で、FPGA 書き込み中でも App を始められた。
  UHD は `multi_usrp::make` で FPGA を書くので open → close だけで済み、libuhd のパッチは要らない。
- **可搬機なので位置・方位の前提を置かない**(2026-09-17、ADS-B)。GPS も方位センサも無い。地図は受信した機体の位置だけから決め(自動フィット)、
  地図データはリポジトリに同梱してバイナリに埋め込む(オフライン、パブリックドメインの Natural Earth + OurAirports)。
- **高頻度の事象はフレームごとに Event にしない**(ADS-B: 新規機 / 初回位置 / 消失だけ。フレーム数は統計)。イベントログを溢れさせない。
- **受信系の golden は実録音**。合成変調器は規格外のパラメータ合わせになるので作らない(STD-T98 で失敗して学んだ)。
- **個体設定は `~/spear/site.conf`**(例 `radio.freq_err_ppm=<ppm>`)。コードにも要件にも埋めない。
- **個体の LO 誤差は Core の Radio が ppm で LO 側で打ち消す**(2026-09-19、`core/include/spear/core/lo_correction.hpp`)。以前は STD-T98 だけが
  `std_t98.freq_err_hz` で IQ を回転していたが、装置の性質なので SPECTRUM の軸・DEMOD・録音の周波数ラベルもずれたままだった。B2xx は RF LO も
  ADC クロックも同じ TCXO から作るので誤差は周波数に比例する(ppm)。`Radio` が要求を装置の目盛りに換算して tune し、読み値を真の周波数に戻すので、
  `RfConfig` / `config()` / event / `StreamMeta` / SigMF はすべて真の周波数のまま、全 App が無変更で正しくなる。ソフト回転(Source 内 / 共通 reader)は
  採らない(CPU、sc16 の再量子化、録音が生でなくなる)。補正前に録った録音の読み替えはしない(撮り直す)。サンプルレートの ppm 誤差(4 Msps で約 12 Hz)は
  補正できずシンボル同期が吸収する。録音の SigMF global に `spear:lo_correction_ppm` を provenance として残す。
- **受信端子(RX PORT)は装置共通の設定としてメニューに置き、GAIN と同じ形で App 起動時に適用する**(2026-09-19、`core/include/spear/core/rf_port.hpp`)。
  B210 の 4 端子(TRXA / RXA / RXB / TRXB)は UHD では frontend(subdev spec `A:A` / `A:B`)× antenna(`TX/RX` / `RX2`)の 2 段で指定するが、
  Spear の外(`RfConfig::port` / 保存 `shell.draftPort` / GUI / SigMF `spear:rx_port`)は装置パネルの名前 1 つで扱い、分解は `Radio::tune_rx` だけが表を見て行う。
  subdev spec は tune の最初の装置呼び出し(能力照合もセンサも channel 0 → spec 経由で解決される)。frontend の存在は tree で先に確かめる —
  libuhd の `coerce_subdev_spec` は B200 / B20xmini で `B` を黙って `A` に書き換えるので、例外待ちだと RXB が RXA になる(黙って丸めない)。
  既定は UHD の既定と同じ RXA。運転中の変更 API は作らない(メニューは App 停止中にしか見えない。GAIN と同じ)。
  実機確認(2026-09-19、5IWG5D5): 4 端子とも stage 4 で `port=… lo_locked`、12 s ストリームで Lossless drop 0(`spear-headless --port`)。GUI でも
  warm-up の tune check と App 起動が選んだ端子で通る(`--port TRXB` / `RXB` で SPECTRUM 運転、ステータスバーに端子名)。端子 LED による対応の目視確認は
  App 運転中に行う(起動時には点かない)。
- **AMBE は mbelib-neo の AMBE 経路だけを C++ で書き直し**(IMBE / SIMD / pffft なし)。pyambelib と PCM が ±1 LSB で一致。
- **ライセンスは GPL-3.0 で公開**(2026-09-17)。GPL-2.0-or-later 由来コードの結合は問題ない。
- **STD-T98 の LO は帯域中心 − 250 kHz**(ゼロ IF の DC スパイクを ch16 に重ねない)。回転量は Core の実 LO から導くので
  録音再生(LO 固定)と実機で同じコード経路。
- **秘話モデルは ffnn = C++ 再実装(safetensors 直読み)、hybrid = ONNX Runtime(必須依存)、両モデルともリポジトリに置いてバイナリに埋め込む**
  (§3.4 道 1 + 道 2、2026-09-17)。秘話解読は App の主要機能で、モデルは作者本人が学習しライセンスも本体と同じなので、ビルドオプションにも
  外部ファイルにもしない。鍵探索は専用ワーカースレッド 1 本(DSP thread では走らせない、ORT のスレッドプールも作らせない)。tools の別プロセス + UDS は
  採らない。SACCH CRC 不良フレームでは秘話判定を保持する(tools は平文扱いでセッション破棄)。

## 装置の事実(個体に依らないもの)
- 開発機 = 対象機 FZ-G2(i5-10310U, 8 GB, Arch, Plasma Wayland scale 1.5 → GUI は 1920×1200 固定キャンバスを縮尺)。
- B210(LibreSDR 互換機でも同じ)を **USB 2.0** で使う前提。FPGA ロードは約 70 s。
- `ref_locked` は外部 10 MHz の PLL のこと(internal では unlocked が正常)。FX3 状態レジスタの読み出しはストリーム中に
  ~13 ms のバルク停止を起こす → 遷移時 / timeout 時のみ読む。
- B2xx 個体の LO 誤差は数 ppm(351 MHz で数百 Hz〜1 kHz)あり得る(STD-T98 の 6.25 kHz チャネルでは無視できない)。`site.conf` の
  `radio.freq_err_ppm` に置くと Radio が LO 側で打ち消す。値の求め方: 補正なし(未設定)で STD-T98 を動かし、開いているチャネルの
  「FREQ ERR」[Hz] ÷ LO [MHz] = ppm(符号そのまま。例 +1030 Hz / 351.04 MHz = +2.93)。設定後は FREQ ERR がほぼ 0 になる。
  DIAGNOSTICS の「LO CORR」と warm-up の tune check 行に適用中の値が出る。
- 受信端子と UHD の対応(`rf_port.hpp` が唯一の真値): TRXA = `A:A` `TX/RX`、RXA = `A:A` `RX2`、RXB = `A:B` `RX2`、TRXB = `A:B` `TX/RX`。
  選んだ端子の LED は **App 運転中(rx streamer が生きている間)だけ**点く(ATR が streamer の有無で RX 状態を決める)。起動時の warm-up の tune check
  (open → tune → close)では点かない。対応を目で確かめるには App を起動して LED を見る。B200 系(frontend A のみ)で RXB / TRXB を選ぶと stage 4 FAULT。
- Qt のログは journald に行く → `QT_FORCE_STDERR_LOGGING=1`。
- 記録・ログ・golden・音声は `~/spear/`(`recordings / logs / golden / audio / site.conf`)。個体固有の値・録音・
  音声由来の golden(`golden/std_t98/ch3_pich.*`, `ambe_golden.txt`, `ch3_payloads.txt`, `secret_golden.txt`)はリポジトリに入れない(golden テストは無ければ skip)。
