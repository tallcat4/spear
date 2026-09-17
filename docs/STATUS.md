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
| M3 | STD-T98 App | **概ね完了** | 実録音・実機ともにフレーム抽出 → AMBE 音声。残: 秘話(下記) |

## コンポーネントと検証状態
| 領域 | 内容 | 検証 |
|---|---|---|
| `core/` Radio | UHD の唯一の所有者。stage 0–6 の自己診断、`DeviceState`(一次ソース: libusb 列挙 / FX3 レジスタ / センサ / UHD ログ)、timed retune、AGC | 実機。USB 抜挿再接続は **パッチ済み libuhd** が前提(`packaging/libuhd`) |
| `core/` Stream Bus | `Stream<T>` / `Subscription`、Lossless(溢れは event + flag で必ず表面化)/ LatestOnly、`BlockPool` + opaque `BlockRef` | gtest + TSan(`scripts/ci.sh`) |
| `core/` Sources | `B210LiveSource`(再接続・watchdog・generation)、`SyntheticSource`(トーン/FM/AM)、`RecordingSource`(SigMF) | gtest、実機 |
| `core/` その他 | `EventBus`(履歴、dispatch thread)、`TAP()`、`HealthMonitor`、`EventLogFile`、`AudioSink`(ALSA/PipeWire) | gtest、実機 |
| `dsp/` 共通 | `design_lowpass`、`FirDecimator` / `FirInterpolator`(ベクトル化カーネル)、`PfbChannelizer`、`SpectrumEstimator`、`stage.hpp`(StageInfo / Provenance)、FFTW プランナ mutex | gtest(参照ベクトル) |
| `appfw/` App SDK | `GuiApp`(on_start/on_stop は GUI thread 保証、`configure(QVariantMap)`)、ビルド時レジストリ、`ViewProcessor/ViewSource`(任意の複素 stream → spectrum/waterfall)、`TraceSource` + `EyeDiagramItem`、`SpectrumItem` / `WaterfallItem`(自前 QRhiTexture リング) | `apps/tests/test_apps.cpp`(全 App のライフサイクル) |
| `widgets/ input/ theme/` | `SpectrumView`(markers / compact)、`EyeDiagram`、`Readout`、`LevelMeter`、`Spear.Input`(KeyButton / Keypad / StepKeys / ValueField / NumericEntry、タッチ専用)、`Theme` | スクリーンショットで目視(`docs/gui/*.png`) |
| `gui/` シェル | メニュー(GAIN のみ、center/rate は App が決める)/ ステータスバー / ソフトキー / DIAGNOSTICS / 数値入力モーダル、`--set key=value`、`--screenshot` 検証ハーネス | 実機・合成 |
| `apps/spectrum` | 汎用。FREQ / RATE(再起動)/ REF / AVG / HOLD | off-air 確認済み |
| `apps/recorder` | Lossless 録音、sidecar、録音中は FREQ 無効 | gtest(drop 0)、実機 |
| `apps/demod` | NFM/WFM/AM、LO を RX から 250 kHz 離す、channel IQ と audio を Stream Bus に publish | 合成 FM トーン gtest、off-air FM |
| `apps/std_t98` | 30ch 受信機(純 C++、Qt/UHD 非依存)、プロトコル、AMBE(C++ 化)、GUI(帯域スペクトラム / チャネル格子 / アイ / フレーム / 全 ch 同時音声) | 実録音 golden(`~/spear/golden/std_t98`)、Python 参照との等価、実機で送信確認 |
| `tools/` | `spear-soak`(M-1)、`spear-headless`(M0)、`spear-std-t98-decode`(録音 → フレーム表 / WAV / ペイロード) | — |

## 未完・既知の欠陥・次の仕事
1. **STD-T98 秘話**(§3.4): `ambe2_ffnn`(MLP、道 1 = C++ 再実装)と `ambe2_hybrid`(ONNX、道 2)。両モデルとも ONNX 変換済み。
   `docs/apps/std_t98.md` の判定を参照。着手前に「どこで鍵探索を走らせるか(DSP thread ではない)」を決める。
2. **ADS-B App**(§8.2、M3 の前に予定していたが後回し): 1090 MHz、event/provenance/golden の検証。受信系の検証は実録音を golden に。
3. **STD-T98 の実機での引き込み**: `max_deviation` の単位バグ修正後、毎回の送信で即ロックすることを継続確認。
   もし再発したら FRAME LOG と SPS(26.02〜26.06 のはず)を記録。
4. **IQ RECORDER**: 録音一覧 / 再生元選択(`Spear.Input` に ListPicker が要る)、ディスク残量による自動停止。
5. **Source API**: 運転中の gain / bandwidth / antenna 変更(現状は `retune` のみ、rate 変更は App 再起動)。
6. **性能の余裕**: STD-T98 は 4 Msps で DSP thread 約 25–30 %(1 コア)。B210 の FPGA ロードが USB 2.0 で約 70 s。
   FZ-G2 は熱スロットリングが頻発する(79 °C)— 損失には直結しないことを M-1 で確認済みだが、DSP 負荷表示を見る。
7. **要件との差分**(要件書は書き換えない):
   - §8.1 の「シェルの草案 RF を App が受け取る」は、center/rate を App が決める形に変更(2026-09-17、誤設定防止)。草案は
     「最後に Core が運転していた RF」に追従し、汎用 App の初期値にだけ使う。
   - `RecordingSource` は `configure/retune` で録音条件を変えない(要求は Warning で無視)。
   - M3 の順序: ADS-B を飛ばして STD-T98 を先行。

## 決定の記録(理由つき、覆すなら理由を書く)
- **libuhd を改造**(0001: USB 消失時に deleter から throw しない、0002: b2xx probe + fx3_state 公開)。upstream PR は出さない。
  共用開発機にはフル構成パッチ版だけを入れる。B200-only は appliance 用。
- **GUI はタッチのみ・計器の文法**(`docs/gui/DESIGN.md`)。OS 仮想キーボード不採用。1920×1200 固定キャンバス。
- **状態の単一所有**(`docs/state-ownership.md`)— 4 か所に周波数がコピーされていた不具合から規約化。
- **共通 DSP の境界**(`docs/dsp-boundary.md`)— 変調方式名の付いた部品は App 内、昇格は抽出で。
- **受信系の golden は実録音**。合成変調器は規格外のパラメータ合わせになるので作らない(STD-T98 で失敗して学んだ)。
- **個体設定は `~/spear/site.conf`**(例 `std_t98.freq_err_hz=<Hz>`)。コードにも要件にも埋めない。
- **AMBE は mbelib-neo の AMBE 経路だけを C++ で書き直し**(IMBE / SIMD / pffft なし)。pyambelib と PCM が ±1 LSB で一致。
- **ライセンスは GPL-3.0 で公開**(2026-09-17)。GPL-2.0-or-later 由来コードの結合は問題ない。
- **STD-T98 の LO は帯域中心 − 250 kHz**(ゼロ IF の DC スパイクを ch16 に重ねない)。回転量は Core の実 LO から導くので
  録音再生(LO 固定)と実機で同じコード経路。

## 装置の事実(個体に依らないもの)
- 開発機 = 対象機 FZ-G2(i5-10310U, 8 GB, Arch, Plasma Wayland scale 1.5 → GUI は 1920×1200 固定キャンバスを縮尺)。
- B210(LibreSDR 互換機でも同じ)を **USB 2.0** で使う前提。FPGA ロードは約 70 s。
- `ref_locked` は外部 10 MHz の PLL のこと(internal では unlocked が正常)。FX3 状態レジスタの読み出しはストリーム中に
  ~13 ms のバルク停止を起こす → 遷移時 / timeout 時のみ読む。
- B2xx 個体の LO 誤差は数百 Hz〜1 kHz 程度あり得る(STD-T98 の 6.25 kHz チャネルでは無視できない)。`site.conf` の
  `std_t98.freq_err_hz` に置く。値の求め方: `spear-std-t98-decode` のチャネル表「est freq err」がほぼ 0 になる値。
- Qt のログは journald に行く → `QT_FORCE_STDERR_LOGGING=1`。
- 記録・ログ・golden・音声は `~/spear/`(`recordings / logs / golden / audio / site.conf`)。個体固有の値・録音・
  音声由来の golden(`golden/std_t98/ch3_pich.*`, `ambe_golden.txt`)はリポジトリに入れない(golden テストは無ければ skip)。
