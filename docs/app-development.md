# App 開発ガイド(App SDK)

S.P.E.A.R. の成果物は **RF バックエンド + フレームワーク + 再利用部品**であり、App はそれを検証する手段である。
App は機能追加のためではなく「S.P.E.A.R. が App の実装に必要なものを提供できているか」を確かめるために作る。
各 App の完了時に `docs/apps/<id>.md` に**プラットフォーム欠陥レポート**(足りなかった API・部品、書きにくかった箇所、性能)を残す。

## 構成
```
apps/<id>/
  CMakeLists.txt     spear_add_app(...) を 1 回。これで登録される(レジストリはビルド時生成 §3.1)
  <id>_app.hpp/.cpp  spear::appfw::GuiApp の派生
  <Id>Page.qml       ページ(QML)。見えるもの: app / sys / ui
  test_*.cpp         SyntheticSource / RecordingSource で回る回帰テスト(apps/tests/ に追加)
docs/apps/<id>.md    欠陥レポート
```
新しい App は `apps/template/` をコピーして始める。`apps/CMakeLists.txt` に `add_subdirectory(<id>)` を足した順がメニュー順。
大きい App(STD-T98)は Qt 非依存の受信機ライブラリ(`receiver.*`、`protocol/`、`dsp/`)と GUI App(`<id>_app.*`)を分け、
受信機は `tools/` のヘッドレスコマンドと gtest で先に検証してから GUI に載せる。

## 2 種類の App
| | 汎用 App(SPECTRUM / RECORDER) | 目的が確定した App(FM/AM RX、STD-T98、今後の ADS-B 等) |
|---|---|---|
| center / rate | App の FREQ / RATE キーで変える(rate は `ui.restartWithRate()` = App 再起動) | App が `declare_rf_config()` で決める。**UI から LO / rate を変える手段を置かない**(誤設定で受信不能になる) |
| 初期値 | シェルの草案(最後に Core が運転していた RF) | 規格 / App の定数 + `site.conf` の個体値 |
| ゼロ IF | — | 受信したい周波数から LO を離す(demod: `loOffset`、STD-T98: 帯域中心 − 250 kHz)。回転量は **Core の実 LO** から導く(録音再生でも同じ経路) |

## 契約
| 項目 | 内容 |
|---|---|
| メタデータ | `spear_add_app(ID NAME DESCRIPTION DIRECTION CLASS HEADER PAGE SOURCES [LIBS] [UNLISTED])` |
| RF 宣言 | シェルの草案が `set_rf_config()` で渡され、`declare_rf_config()` で Core へ。適用不能なら App 起動失敗(§8.1) |
| `on_start(Core&)` | `core.rx()` に subscribe、DSP チェーンを静的に構成、DSP 用 thread を起動。**GUI thread で呼ばれる**(基底が保証) |
| `on_stop()` | consumer / thread を全て解放。stop 後に `core.rx().consumer_count()` が 0 になること(テストで確認) |
| App 固有 State | `Q_PROPERTY`(NOTIFY 付き)として App が所有。変更は App のメソッドだけ(`docs/state-ownership.md`) |
| Core の State | コピーしない。ページは `sys.*`(SystemModel)を読む。周波数変更は `core()->source().retune()` |
| Event | 事象は `core()->events().emit(kind, source, detail, range)` に **元 sample index 範囲**を付ける (§4.5) |
| TAP | 中間 stream に `TAP("<id>.<point>", ptr, n)` (§5.3) |
| ページ | `required property var app / ui`、`softKeys`(7 個まで、8 個目 BACK はシェル)、`softKey(action)`。部品は `Spear.Widgets` / `Spear.Input` |
| 入力 | `ui.askFreq / askRate / askGain / askRef / askOffset(current, callback)`、`ui.restartWithRate(v)`、`ui.showDiagnostics()`。自前の数値入力(タッチ専用) |
| 設定 | `configure(const QVariantMap&)`: `record_dir`、`audio_device`、`--set key=value`(`spear.sh` が `~/spear/site.conf` から渡す。キーは `<id>.<name>`)。保存値の復元より後に呼ばれる(site.conf が勝つ) |
| 運転状態の保存 | コンストラクタで `persist({"squelchDb", "view.dbMax", ...})`。シェルの `SettingsStore` が `~/spear/state.conf` から書き戻し、NOTIFY のたびに保存する。宣言するのは **ユーザーが操作する値**(スケルチ、MUTE、モード、REF/AVG、選択 ch)。観測値・派生値(チャネル IQ 表示のレンジ等)・個体値(site.conf)は宣言しない。名前の解決は `spear_settings_tests` が全 App について確認する |
| 観測点 | 内部 stream を `Stream<cf32>/<float>` として publish し `ViewProcessor` を繋ぐ(spectrum/waterfall)。短いトレース(アイ等)は `appfw::TraceSource` に push → `EyeDiagram` |
| 表示 | `SpectrumView`(`markers` で多チャネル、`compact` で狭い場所)、`EyeDiagram`、`Readout`、`LevelMeter`。1920×1200 固定キャンバス、タッチのみ |
| 音声 | `AudioSink(events, device, rate)`。複数 stream を混ぜるなら provenance の時刻に加算するミキサ(STD-T98 の `Mixer` を参照) |
| 検証 | 受信系は **実録音の抜粋を golden** に(`~/spear/golden/<id>/`、無ければ skip)。合成信号は部品単位(symbol sync 等)に留める。実機の新しい表示は既知周波数の実信号で確認 |
| 禁止 | UHD への直接アクセス、Core の state のコピー保持、キーボード前提の UI |

## チェックリスト(App 完了時)
* [ ] SyntheticSource で回るテストがあり、stop 後に consumer が残らない
* [ ] App の State に所有者以外からの書き込みが無い
* [ ] ユーザーが操作する State を `persist({...})` で宣言した(`spear_settings_tests` が名前を確認する)
* [ ] Event に provenance が付いている
* [ ] 中間 stream に TAP がある
* [ ] `docs/apps/<id>.md` に欠陥レポートを書いた(共通に欲しかった部品は `docs/dsp-boundary.md` の基準で判定)
* [ ] `apps/tests/test_apps.cpp` にライフサイクルのテストを足した(SyntheticSource で start → 観測点が動く → stop で consumer 0)
* [ ] 規格のパラメータは一次資料の値・**単位**のまま(移植元のパラメータの単位を確認した)
* [ ] `--screenshot` で実画面を撮って `docs/gui/` か欠陥レポートに残した
* [ ] `docs/STATUS.md` の表を更新した
