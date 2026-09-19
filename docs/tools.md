# ツールと運用手順

## 起動(実機)
```
./spear.sh                      # B210 で全画面起動。録音・ログは ~/spear/ 配下
./spear.sh --windowed           # 開発用にウィンドウ表示
./spear.sh --source synthetic   # ハードウェアなし
./spear.sh --source file:rec1   # 録音再生
```
タップで起動するには `scripts/install-desktop.sh`: KDE Plasma のスタートメニュー(`~/.local/share/applications/spear.desktop` + アイコン)、
タスクバー(パネルのタスクマネージャの launchers に追加。plasmashell は終了時に設定を書き戻すので、止めてから編集して起動し直す)、
デスクトップ(`~/Desktop/spear.desktop`、初回クリックで実行の許可を聞かれることがある)に登録する。`--remove` で外す。
非キオスク(通常の Plasma セッション)の一時策。キオスク化(専用セッションで自動起動)したら要らなくなる。
個体・現場固有の設定は `~/spear/site.conf`(`key=value`、例 `radio.freq_err_ppm=<個体の LO 誤差 ppm>`)→ `spear.sh` が `--set` で渡す
(`radio.*` は Core、`<id>.*` は各 App)。
運転状態(メニューのゲイン/AGC、各 App のスケルチ・MUTE・モード・REF/AVG・選択チャネル、汎用 App の周波数/レート)は `~/spear/state.conf` に
自動保存され(変更の 0.5 s 後、原子的に書き換え)、次回起動で復元される。site.conf の値が優先。初期化したければファイルを消す。
RF の center / rate は各 App が決める(汎用 App は FREQ / RATE キーを持つ)。メニューで決めるのは装置共通の GAIN(AGC 可)と RX PORT(受信端子)だけ(右の欄をタップ。ソフトキーは EXIT のみ)。
`spear-gui` 単体の既定も実機(`--source b210`)。合成データは明示指定のときだけ。
操作はタッチのみ(FZ-G2 タブレットモード専用。キーボード・マウスは考慮しない)。画面下のソフトキーと読み出し欄のタップ。
`--screenshot out.png --after 10 --start-app 0` で実画面をキャプチャ(検証用。`--invoke openExitDialog` 等で Main.qml の関数も呼べる)。設計は `docs/gui/DESIGN.md`。
終了はメニュー左端の EXIT(確認ダイアログ)。非キオスクのデスクトップでフルスクリーン起動している間の導線。

## STD-T98 録音の解析
```
./build/tools/spear-std-t98-decode ~/spear/recordings/<base> --freq-err <そのファイルの残留誤差 Hz> --squelch -50 --wav ~/spear/audio/<base>
```
30 チャネルのフレーム表(SACCH/PICH CRC、CSM、推定周波数誤差)と、チャネルごとの AMBE 音声 WAV を出す。
`--freq-err` はそのファイル固有の残留誤差(`radio.freq_err_ppm` 設定後の録音なら 0。それ以前の録音は個体の誤差 Hz)。
秘話呼(SACCH call=1)は GUI と同じポリシーで鍵を探索し(1 ブロック約 0.6 s)、以後のフレームを復号して WAV に出す。フレーム行末に
`SECRET key=<鍵> (<経路>, <秒>)`、以後 `[secret K <鍵>]` が付く。`--no-secret` で探索を止める(スクランブルのまま)。

## ADS-B 録音の解析
```
./build/tools/spear-adsb-decode ~/spear/recordings/<base> [--ref <lat>,<lon>] [--fix] [--golden ~/spear/golden/adsb/es.golden.txt]
```
受理フレームの行(radio.rx の index、DF、ICAO、hex、解読内容、RSSI/SNR、位置)と統計、航空機一覧を出す。LO オフセットは録音の中心周波数から求める。
`--golden` は "index hex" を 1 行ずつ書き、抜粋録音と並べて `~/spear/golden/adsb/es.*` に置くと `spear_adsb_tests` の golden が有効になる。

## 秘話モデルの更新(再学習したときだけ。通常は不要 — モデルはリポジトリに同梱しバイナリに埋め込む)
```
../std-t98-tools/env/bin/pip install onnx onnxruntime              # 変換用(std-t98-tools の venv、numpy は <2 のまま)
../std-t98-tools/env/bin/python apps/std_t98/tools/export_secret_onnx.py    # → apps/std_t98/models/(safetensors をコピー、hybrid を ONNX に変換して torch と比較)
../std-t98-tools/env/bin/python apps/std_t98/tests/gen_secret_golden.py     # → ~/spear/golden/std_t98/secret_golden.txt(テスト用、torch が真値)
```

## 耐久試験(`spear-soak`、要件 §14 M-1)
```
sudo scripts/tune-power.sh apply          # (a) governor=performance, USB autosuspend off
./build/tools/spear-soak --rate 10e6 --minutes 30 --out soak_a.csv --mlock
sudo scripts/tune-power.sh restore        # (b) デフォルト + バッテリー
./build/tools/spear-soak --rate 10e6 --minutes 30 --out soak_b.csv
```

CSV に 1 秒ごとの overflow / out_of_sequence / timeout / CPU MHz / 温度 / throttle / AC 状態が残る。
overflow(host 読み遅れ)と out_of_sequence(USB パケット欠落)は区別して数える(§4.3)。

## Stream Bus のヘッドレス確認(`spear-headless`、要件 §14 M0)
```
./build/tools/spear-headless --source synthetic --seconds 20 --record rec1      # 実機は --source b210 [--port TRXA|RXA|RXB|TRXB](受信端子。既定 RXA)
./build/tools/spear-headless --source file:rec1 --seconds 10     # 同じ App に録音を流す
./build/tools/spear-headless --source b210 --rate 4e6 --freq 100e6 --record rec2
```

途中で waterfall consumer を attach/detach し、recorder(Lossless)の drop が 0 のままであることを表示する。
`SPEAR_TAP_DIR=./tap` で `TAP()` の中間 stream が raw ファイルへ落ちる(release では消える)。

## 診断・装置状態
`docs/diagnostics.md` を参照。装置状態は `DeviceState`(DISCONNECTED / NO_FIRMWARE / STANDBY / INITIALIZING /
READY / STREAMING / FAULT / LOST)+ 根拠文字列で、一次ソース(libusb 列挙、FX3 状態レジスタ、UHD センサ、
stream 自体、UHD 自身のログ)から判定する。

## 設計上の注記(Core)
* 並行キューは `std::mutex` + `std::deque` (§6.1「素朴な実装で足りる」)。lock-free 化は必要になってから。
  TSan で競合が無いことを CI で担保する (§12.2)。
* `BlockRef` の内部表現は `shared_ptr<const Block>`。公開 API からは見えないので将来差し替え可能 (§6.2)。
* Lossless consumer の溢れ: producer は止まらず block を捨てるが、`ConsumerOverflow` event → burst 終了時に
  `Discontinuity` event(捨てた sample 範囲つき)→ 次に配送する block に `Discontinuity` flag、の 3 点で必ず表面化する。
* `sample_index` は B210 の `time_spec` tick から導く。欠落は index の飛びとして現れ、provenance が保たれる。
