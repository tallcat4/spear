# ツールと運用手順

## 起動

```
./spear.sh                      # B210 で全画面起動。録音・ログは ~/spear/ 以下
./spear.sh --windowed           # ウィンドウ表示(開発用)
./spear.sh --source synthetic   # ハードウェアなし
./spear.sh --source file:rec1   # 録音を再生
```

- `spear-gui` を直接起動した場合も既定の入力は実機(`--source b210`)。合成信号は明示したときだけ使う。
- 操作はタッチのみ(FZ-G2 のタブレットモード専用)。画面下のソフトキーと、読み出し欄のタップで操作する。
- 終了はメニュー左端の EXIT(確認ダイアログあり)。
- RF の center / rate は各 App が決める(汎用 App は FREQ / RATE キーを持つ)。メニューで設定するのは装置共通の GAIN(AGC 可)と RX PORT(受信端子)だけ。右側の欄をタップして開く。メニューのソフトキーは EXIT だけ。
- 画面の設計は `docs/gui/DESIGN.md`。

### デスクトップへの登録

`scripts/install-desktop.sh` で次の場所に起動アイコンを登録する。`--remove` で削除する。

- KDE Plasma のスタートメニュー(`~/.local/share/applications/spear.desktop` + アイコン)
- タスクバー(パネルのタスクマネージャの launchers に追加。plasmashell は終了時に設定を書き戻すので、止めてから編集して起動し直す)
- デスクトップ(`~/Desktop/spear.desktop`。初回クリックで実行の許可を聞かれることがある)

通常の Plasma セッションで使うための一時的な手段で、キオスク化(専用セッションで自動起動)したら不要になる。

### 設定ファイル

| ファイル | 内容 |
|---|---|
| `~/spear/site.conf` | 個体・設置場所に固有の値(`key=value`)。例: `radio.freq_err_ppm=<個体の LO 誤差 ppm>`。`spear.sh` が `--set` で渡す。`radio.*` は Core、`<id>.*` は各 App が読む |
| `~/spear/state.conf` | 運転状態(メニューのゲイン / AGC、各 App のスケルチ・MUTE・モード・REF/AVG・選択チャネル、汎用 App の周波数 / レート)。変更の 0.5 s 後に原子的に書き換え、次回起動時に復元する |

site.conf の値が state.conf より優先される。運転状態を初期化するには state.conf を削除する。

### スクリーンショット

`--screenshot out.png --after 10 --start-app 0` で実際の画面を PNG に保存する(検証用)。`--invoke openExitDialog` のように Main.qml の関数も呼べる。

## STD-T98 録音の解析

```
./build/tools/spear-std-t98-decode ~/spear/recordings/<base> --freq-err <残留誤差 Hz> --squelch -50 --wav ~/spear/audio/<base>
```

- 30 チャネルのフレーム表(SACCH/PICH CRC、CSM、推定周波数誤差)と、チャネルごとの AMBE 音声 WAV を出力する。
- `--freq-err` はその録音に残っている周波数誤差。`radio.freq_err_ppm` を設定した後の録音なら 0、それより前の録音は個体の誤差(Hz)を指定する。
- 秘話呼(SACCH call=1)は GUI と同じ方法で鍵を探索し(1 ブロック約 0.6 s)、以降のフレームを復号して WAV に出す。
  フレーム行の末尾に `SECRET key=<鍵> (<経路>, <秒>)`、以降の行に `[secret K <鍵>]` が付く。`--no-secret` で探索しない(スクランブルのまま出す)。

## ADS-B 録音の解析

```
./build/tools/spear-adsb-decode ~/spear/recordings/<base> [--ref <lat>,<lon>] [--fix] [--golden ~/spear/golden/adsb/es.golden.txt]
```

- 受理したフレーム(radio.rx の index、DF、ICAO、hex、解読内容、RSSI/SNR、位置)、統計、航空機一覧を出力する。LO オフセットは録音の中心周波数から求める。
- `--golden` は "index hex" を 1 行ずつ書き出す。抜粋した録音と一緒に `~/spear/golden/adsb/es.*` に置くと、`spear_adsb_tests` の golden テストが有効になる。

## 秘話モデルの更新

再学習したときだけ行う。モデルはリポジトリに含まれ、バイナリに埋め込まれるので、通常は不要。

```
../std-t98-tools/env/bin/pip install onnx onnxruntime                       # 変換用(std-t98-tools の venv。numpy は <2 のまま)
../std-t98-tools/env/bin/python apps/std_t98/tools/export_secret_onnx.py    # → apps/std_t98/models/(safetensors をコピーし、hybrid を ONNX に変換して torch と比較)
../std-t98-tools/env/bin/python apps/std_t98/tests/gen_secret_golden.py     # → ~/spear/golden/std_t98/secret_golden.txt(テスト用。torch の出力が真値)
```

## 耐久試験(`spear-soak`、要件 §12 M-1)

```
sudo scripts/tune-power.sh apply          # (a) governor=performance, USB autosuspend off
./build/tools/spear-soak --rate 10e6 --minutes 30 --out soak_a.csv --mlock
sudo scripts/tune-power.sh restore        # (b) デフォルト + バッテリー
./build/tools/spear-soak --rate 10e6 --minutes 30 --out soak_b.csv
```

CSV には 1 秒ごとに overflow / out_of_sequence / timeout / CPU MHz / 温度 / throttle / AC 状態を記録する。
overflow(ホストの読み遅れ)と out_of_sequence(USB パケットの欠落)は別々に数える(§3.3)。

## Stream Bus のヘッドレス確認(`spear-headless`、要件 §12 M0)

```
./build/tools/spear-headless --source synthetic --seconds 20 --record rec1
./build/tools/spear-headless --source file:rec1 --seconds 10     # 同じ App に録音を流す
./build/tools/spear-headless --source b210 --rate 4e6 --freq 100e6 --record rec2
```

- 実行中に waterfall consumer を attach / detach し、recorder(Lossless)の drop が 0 のままであることを表示する。
- 実機では `--port TRXA|RXA|RXB|TRXB` で受信端子を選べる(既定 RXA)。
- `SPEAR_TAP_DIR=./tap` を指定すると、`TAP()` の中間 stream を raw ファイルに書き出す(release ビルドでは無効)。

## 診断・装置状態

詳細は `docs/diagnostics.md`。装置状態は `DeviceState`(DISCONNECTED / NO_FIRMWARE / STANDBY / INITIALIZING / READY / STREAMING / FAULT / LOST)と根拠の文字列で表し、
libusb の列挙、FX3 状態レジスタ、UHD のセンサ、stream そのもの、UHD のログから判定する。

## Core の実装メモ

- 並行キューは `std::mutex` + `std::deque`(§5.1「単純な実装で足りる」)。lock-free 化は必要になってから行う。競合が無いことは CI の TSan で確認する(§10.2)。
- `BlockRef` の内部表現は `shared_ptr<const Block>`。公開 API からは見えないので、後で差し替えられる(§5.2)。
- Lossless consumer が溢れた場合、producer は止まらずに block を捨てる。そのうえで `ConsumerOverflow` event、burst 終了時の `Discontinuity` event(捨てた sample 範囲つき)、次に配送する block の `Discontinuity` flag の 3 つで報告する。
- `sample_index` は B210 の `time_spec` の tick から求める。欠落は index の飛びとして現れ、provenance は保たれる。
