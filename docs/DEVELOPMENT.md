# 開発ガイド

リポジトリで作業を始めるときに読む文書。全体像、読む順、規約、確認方法をまとめる。現状と次の作業は `docs/STATUS.md` にある。

個体・環境に固有の情報(装置のシリアル、LO 誤差、ローカルのパス)はリポジトリに入れず、各自の `~/spear/site.conf` とリポジトリ外のメモに置く。

## 概要

FZ-G2(タブレット、タッチ操作のみ)+ USRP B210 上で動く SDR の基盤。C++20 / Qt 6 / UHD。
構成要素は RF バックエンド、フレームワーク(Stream Bus、provenance、App SDK、観測点)、再利用部品(DSP、Widgets、入力)で、
その上に App(SPECTRUM / IQ RECORDER / FM-AM RX / STD-T98 MONITOR / ADS-B)が載る。
App を作り終えたら、基盤に足りなかった点を `docs/apps/<id>.md` に記録し、基盤に反映する。
要件は `docs/requirements.md`(v5.0、コード中の §番号はこの文書の節)。

## 読む順

1. `docs/STATUS.md` — 完成していること、未完のこと、検証済みの事実、決定の記録
2. `README.md` — 構成、ビルド、起動
3. `docs/app-development.md` — App を追加・修正するときの契約
4. 規約: `docs/state-ownership.md`(状態の単一所有)、`docs/dsp-boundary.md`(共通 DSP の境界)、`docs/gui/DESIGN.md`(見た目と操作)
5. 作業する領域の `docs/apps/*.md`。装置まわりなら `docs/diagnostics.md` と `packaging/libuhd/README.md`

## ビルド・テスト・起動

```
cmake -S . -B build -G Ninja && ninja -C build        # 依存: cmake ninja gcc≥13 fftw3f gtest Qt6 boost(UHD 用) パッチ済み libuhd onnxruntime-cpu
ctest --test-dir build -j4                            # 全テスト(ハードウェア不要。実録音の golden が無いテストは skip)
./spear.sh                                            # 実機で全画面。--windowed / --source synthetic / --source file:<sigmf base>
```

ハードウェアなしの CI 一式: `scripts/ci.sh`(UHD OFF + TSan)。ヘッドレス実行: `build/tools/spear-headless`、`spear-std-t98-decode`。

## 規約

理由は各リンク先の文書に書いてある。

- 状態の所有者は 1 つ。RF の宣言は `Source::config()` だけが持ち、GUI / App はコピーせず Core から読む。要求は捨てない(`docs/state-ownership.md`)。
  再起動後も残す値は App が `persist({...})` で宣言し、シェルの `SettingsStore`(`~/spear/state.conf`)が復元・保存する。App に保存処理を書かない。
- 共通 DSP に入れるのは、数学的に定義され、用途の定数を含まず、2 つ目の App が必要とし、参照ベクトルで検証できるものだけ。
  変調方式名の付く部品は App 内に置く。共通化は 2 つ目の利用者が出てから抽出で行う(`docs/dsp-boundary.md`)。
- UI はタッチ操作のみ。黒基調・フラット。キーボード / マウス前提の要素、グラデーション、装飾は使わない。
  入力部品は自前の `Spear.Input` を使い、OS の仮想キーボードは使わない(`docs/gui/DESIGN.md`)。
- 用途が決まっている App(STD-T98 など)には LO / rate を手で変える UI を置かない(誤設定で受信できなくなるため)。FREQ / RATE キーを持つのは汎用 App(SPECTRUM / RECORDER)だけ。
  装置共通の設定は GAIN(AGC を含む)と RX PORT(受信端子)だけをメニューに置く。欄をタップして開き、同じ機能のソフトキーは置かない。
- 規格のパラメータは一次資料の値を使う(STD-T98: 偏移 315 Hz/level、2400 Bd、6.25 kHz、同期語など)。
  移植時はパラメータの単位を一次資料(GR のソースなど)で確認する(例: `max_deviation` は比率ではなく samples/symbol。実機で発覚した)。
- 受信系のテストは、合成信号ではなく実録音の抜粋を golden にする。golden は `~/spear/golden/` に置き、リポジトリには入れない(録音・音声・個体の値を含むため。無ければテストは skip)。合成の変調器は作らない。
- 学習済みモデルは C++ で再実装する(safetensors を直接読む)か ONNX Runtime で動かす(§2.5)。torch / libtorch は使わない。
  モデルはリポジトリに置いてバイナリに埋め込む(実行時にパスを持たない)。推論は DSP スレッドの外の専用スレッドで行う(STD-T98 の `secret::Worker`)。
  logits は Python 側を真値とした golden で一致を確認する。
- 個体・設置場所に固有の値はコードに書かない。`~/spear/site.conf` の `key=value` を `spear.sh` が `--set` で渡す。
  装置の値(例: `radio.freq_err_ppm` = B210 個体の LO 誤差。Core の Radio が LO 側で補正する)と、App 固有の値(`<id>.<name>`)がある。
  site.conf は自動保存の `state.conf` より優先される。
- libuhd はパッチ済みパッケージ(`packaging/libuhd`)を使う。upstream に PR は出さない。B200-only プロファイルは共用の開発機に入れない(他の UHD アプリが動かなくなる)。
- event には radio.rx の sample 範囲(provenance)を付ける。中間 stream には `TAP()` を置く。
- ライセンスは GPL-3.0。GPL-2.0-or-later 由来のコード(`apps/std_t98/ambe/`)は結合してよい。SPDX と著作権表示は残す。
- コメントは日本語、識別子は英語。既存ファイルの書き方に合わせる。根拠として要件の §番号を書く。
- 要件と実装の違いは `docs/STATUS.md` の「要件との差分」に書く。要件書の §番号を変えたら、コードとドキュメントの参照も合わせて直す。

## 確認方法

- ユニット / 回帰テスト: `ctest`。App のライフサイクル(stop 後に consumer が残らないこと)は `apps/tests/test_apps.cpp`。
- 画面: 次のコマンドでスクリーンショットを撮って確認する。
  ```
  WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 \
    ./build/gui/spear-gui --source file:<base> --screenshot out.png --after 10 --start-app <n> [--diag] [--app-action <key>] [--restart-rate R]
  ```
  App 番号はメニュー順(0 SPECTRUM, 1 RECORDER, 2 FM/AM, 3 STD-T98, 4 ADS-B)。`--state-file` を付けなければ運転状態の復元・保存はしない(再現性のため)。
- 実機: B210 の FPGA ロードは USB 2.0 で約 70 s かかる。`--after` はそれより長くする(途中で切ると次回また読み込む)。
  新しい表示部品や DSP 段は、周波数が既知の実信号で軸の向きと絶対値を確認する(合成トーンでは向きの誤りが分からない)。
- 実録音: `spear-std-t98-decode <base> --freq-err <Hz> [--wav out]` でフレーム表と音声を出す(秘話呼は鍵を探索して復号する)。App の録音(LO = 帯域中心 − 250 kHz)は `--freq-err 250000`(`docs/tools.md`)。
- 装置の既知の挙動(B210 の癖など)は `docs/STATUS.md` の「装置の事実」と `docs/diagnostics.md` にある。

## 方針

- 動けばよいという修正で終わらせない。同じ問題が再発しないよう、規約・SDK・Core のどこで直すべきかを考えて直し、規約に追記する。
- 合成データでテストが通っても、実機での確認を省かない。実機の結果(録音・スクリーンショット・数値)は docs に残す。
