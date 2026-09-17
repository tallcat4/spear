# 開発の入口

リポジトリを初めて開いた人が **最初に読む**もの。
「何であるか」「どこを読むか」「何を守るか」「どう確かめるか」だけを書く。現状と次の仕事は `docs/STATUS.md`。
個体・環境固有の事実(装置のシリアル、LO 誤差、ローカルのパス)はリポジトリに入れず、各自の `~/spear/site.conf` と
リポジトリ外のメモに置く。

## 何であるか
**どんな信号処理アプリでも追加でき、どんな信号でも観測できるようにする GUI / RF フロントエンド基盤**。
対象は FZ-G2(タブレット、タッチのみ)+ USRP B210。C++20 / Qt 6 / UHD。
成果物は **RF バックエンド + フレームワーク(Stream Bus, provenance, App SDK, 観測点)+ 再利用部品(DSP, Widgets, 入力)**。
App(SPECTRUM / IQ RECORDER / FM-AM RX / STD-T98 MONITOR)は機能ではなく **基盤を検証する道具**で、STD-T98 も 1 App にすぎない。
各 App の完了時に `docs/apps/<id>.md` に欠陥レポートを残し、基盤に反映する。要件書は `docs/requirements.md`(v4.1、§番号で参照)。

## 最初に読む順
1. `docs/STATUS.md` — 何ができていて何が未完か、検証済みの事実、次の仕事、決定の記録
2. `README.md` — 設計の考え方、構成、ビルド、起動
3. `docs/app-development.md` — App を足す・直すときの契約
4. 規約: `docs/state-ownership.md`(状態の単一所有)/ `docs/dsp-boundary.md`(共通 DSP の境界)/ `docs/gui/DESIGN.md`(見た目と操作)
5. 触る領域の欠陥レポート `docs/apps/*.md`、装置の話なら `docs/diagnostics.md` と `packaging/libuhd/README.md`

## ビルド・テスト・起動(この 3 行で全部)
```
cmake -S . -B build -G Ninja && ninja -C build        # 依存: cmake ninja gcc≥13 fftw3f gtest Qt6 boost(UHD 用) パッチ済み libuhd onnxruntime-cpu
ctest --test-dir build -j4                            # 全テスト(ハードウェア不要。実録音 golden は無ければ skip)
./spear.sh                                            # 実機で全画面。--windowed / --source synthetic / --source file:<sigmf base>
```
ハードウェアなしの完全 CI: `scripts/ci.sh`(UHD OFF + TSan)。ヘッドレス: `build/tools/spear-headless`, `spear-std-t98-decode`。

## 守ること(理由は各ドキュメントに)
- **状態は所有者 1 つ**。宣言は `Source::config()` だけ、GUI/App はコピーを持たず Core を読む。要求は捨てない(`docs/state-ownership.md`)。
  再起動をまたいで残す値は App が `persist({...})` で宣言し、シェルの `SettingsStore`(`~/spear/state.conf`)が復元・自動保存する。App に保存コードを書かない。
- **共通 DSP には「数学的に定義され・定数を含まず・2 つ目の App が要り・参照ベクトルで検証できる」ものだけ**。
  変調方式の名前が付いた部品は App 内(`docs/dsp-boundary.md`)。昇格は予測せず抽出で。
- **UI はタッチのみ・黒基調フラット・計器の文法**。キーボード/マウス前提の要素、グラデーション、SF 風装飾は不可。
  入力部品は自前(`Spear.Input`)。OS の仮想キーボードは使わない(`docs/gui/DESIGN.md`)。
- **目的が確定した App(STD-T98 等)には LO / rate を手で変える UI を置かない**(誤設定で受信不能になる)。汎用 App
  (SPECTRUM / RECORDER)だけが FREQ / RATE キーを持つ。装置共通の設定は GAIN(AGC 含む)のみメニューに置く。
- **規格のパラメータは一次資料の値を変えない**(STD-T98: 偏移 315 Hz/level、2400 Bd、6.25 kHz、同期語…)。
  移植時は **単位** を一次資料(GR ソース等)と突き合わせる(`max_deviation` は samples/symbol、比率ではない — 実機で発覚)。
- **受信系の検証は合成信号より実録音の抜粋を golden にする**(`~/spear/golden/`、リポジトリ外。録音・音声・個体値を
  リポジトリに入れない。無ければテストは skip)。合成変調器を発明しない。
- **学習済みモデルは C++ 再実装(safetensors 直読み)か ONNX Runtime**(§3.4)。torch / libtorch を持ち込まない。モデルはリポジトリに置いて
  バイナリに埋め込む(実行時のパスを持たない)。推論は DSP thread の外の専用スレッドで(STD-T98 の `secret::Worker`)。Python 側を真値にした
  golden で logits の一致を確かめる。
- **個体・現場固有の値はコードに埋めない**。`~/spear/site.conf` の `key=value` → `spear.sh` が `--set` で App に渡す
  (例: `std_t98.freq_err_hz=<Hz>` = その B210 個体の LO 誤差)。site.conf は自動保存された運転状態(`state.conf`)より優先。
- **libuhd はパッチ済みパッケージ(`packaging/libuhd`)を使う。upstream に PR は出さない。B200-only プロファイルは
  共用開発機に入れない**(他の UHD アプリを壊す)。
- **事象(event)には radio.rx の sample 範囲(provenance)を付ける**。中間 stream には `TAP()` を置く。
- ライセンス: 本体は **GPL-3.0 で公開する方針**。GPL-2.0-or-later 由来(`apps/std_t98/ambe/`)は結合可。SPDX と著作権表示は残す。
- コメントは日本語、識別子は英語。既存ファイルの密度・語彙に合わせる。要件の §番号を根拠として書く。

## 確かめ方(「動いた」と言う前に)
- ユニット/回帰: `ctest`。App のライフサイクル(stop 後に consumer が残らない)は `apps/tests/test_apps.cpp`。
- 画面: `WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 \
  ./build/gui/spear-gui --source file:<base> --screenshot out.png --after 10 --start-app <n> [--diag] [--app-action <key>] [--restart-rate R]`
  → PNG を見る(App 番号はメニュー順: 0 SPECTRUM, 1 RECORDER, 2 FM/AM, 3 STD-T98, 4 ADS-B)。`--state-file` を付けなければ運転状態は復元も保存もされない(再現性のため)。
- 実機: B210 の FPGA ロードは USB 2.0 で約 70 s。`--after` はそれより長く(短く切ると次回また読み込む)。
  実機の新しい表示部品・DSP 段は **既知周波数の実信号**で軸の向きと絶対値を確認する(合成トーンでは向きの誤りが出ない)。
- 実録音: `spear-std-t98-decode <base> --freq-err <unit Hz> --squelch -50 [--wav out]` でフレーム表と音声(秘話呼は鍵探索して復号)。
- 実機の事実(B210 の癖など)は `docs/STATUS.md` の「装置の事実」と `docs/diagnostics.md`。

## やらないこと
- 要件書(`docs/requirements.md`)を書き換えない(乖離は `docs/STATUS.md` の「要件との差分」に書く)。
- 「動く」だけの修正で終わらせない。同種の問題が二度と起きない層(規約・SDK・Core)で直し、規約に追記する。
- 合成データで通ったからと実機確認を省かない。実機の結果(録音・スクリーンショット・数値)を docs に残す。
