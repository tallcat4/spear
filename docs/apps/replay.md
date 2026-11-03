# IQ REPLAY TX — 実装メモ(2026-11-04、最初の送信 App)

録音した IQ を、録ったときの center / sample rate のまま送信する(要件 §7.2 Replay Transmitter、IQ RECORDER の逆)。
符号化はしない。TOKUDER などのエンコーダーは無いので、送れるのは録音を送り返すことだけ。

**検証対象**: 送信の fan-in(§4.1)、TX 不変条件(§4.3: 全 sample を送出するか Underflow event)、TX MASTER(マスターアーム)。

## 使い方
1. 送信端子(既定 TRXA)に減衰器 / ダミーロードを付ける。
2. メニュー右上の **TX MASTER** をタップし、確認で ARM。ARMED の間はステータスバーに赤い `ARMED` が出る(タップで即 SAFE)。
3. IQ REPLAY TX を起動 → FILE で録音を選ぶ(App は止めずに、Core が録音の center / rate で装置を構成し直す。`Core::reconfigure`)。
4. TRANSMIT で録音の先頭から 1 つの burst として送る。終わり(LOOP OFF)か STOP で burst を閉じる。
   TX GAIN は運転中に変えられる。保存しないので、プロセスを起動するたびに 0 dB から(FILE / TX PORT による App の再起動では前の値を引き継ぐ)。

録音一覧は `record_dir`(既定 `~/spear/recordings`)と、site.conf の `replay.dirs=<dir>:<dir>`(golden 等)を再帰的に探す。新しい順。

## 安全の仕組み(どの層で止めるか)
| 層 | 止め方 |
|---|---|
| Core: TX MASTER | `Core::set_tx_armed`。起動時は必ず SAFE(保存しない)。SAFE なら `TxStream::write` が拒否し(Warning event を 1 回)、送信 thread は burst を始めない。送信中に SAFE にすると、キューの残りを捨て、EOB を送って burst を閉じる。どの App の送信もこの入口を通る |
| 装置: ATR | burst の外では B210 の ATR が送信経路を切る(UHD `b200_impl.cpp` `update_atrs`: IDLE = `STATE_RX1_OFF`、TX_ONLY は FPGA が送っている間だけ)。App の起動中でも TRANSMIT を押すまで端子には何も出ない(LED も消えている。2026-11-04 実機で確認) |
| Radio | `tune_tx` は送信ゲインを最小にしてから周波数を合わせ、最後に宣言のゲインにする。終了時も最小に戻してから閉じる。範囲外・coercion は失敗(黙って丸めない)。送信は TRXA / TRXB だけ(`rf_port_can_tx`) |
| App | 送信ゲインの既定は 0 dB。周波数・レートは録音の値で、UI から変える手段を置かない。録音・端子を変えたら再起動(宣言し直し)まで TRANSMIT できない |

AD9361 の TX chain 自体は streamer が無くても有効のまま(`update_enables` の "enable something")。burst ごとに streamer を作り直しても止まらないので、そうはしていない。
burst の外の LO の漏れが端子に出ていないことは、受信機での測定がまだ(未検証 1)。

## 基盤に足したもの
* **`TxStream`**(`core/tx_stream.hpp`): 送信の fan-in。キューが一杯なら producer を待たせる(捨てない)。`false` は閉じられたときと SAFE のときだけ。
  App は Source より先に始まる(`Core::run_app`)ので、装置の open(数秒)の間も write は待ち続けなければならない
  (最初は 1 s で諦めていて、実機で 32 block だけ送って underflow になった。`TxSource.ProducerStartedBeforeSourceWaitsForIt`)。
  block は producer の BlockPool を指すので、App は pool を App と同じ寿命にする(`close()` は残りを捨てる)。
* `Source::tx_input()` / `set_tx_gain()`、`Core::tx()`、`RfConfig::tx_gain / tx_port`(受信の gain / port とは別。草案の受信ゲインを送信に流用しない)。
* `B210LiveSource` の送信 session(`run_tx`): probe → open → `tune_tx` → streamer → 定常(`tx_loop`)。`recv_async_msg` を別 thread で読み、
  UNDERFLOW / UNDERFLOW_IN_PACKET / SEQ_ERROR / TIME_ERROR / BURST_ACK を区別したまま 100 ms ごとにまとめて event にする。TX の lo_locked も監視。
  受信は流さない(全二重は別 session として後から足せる形)。
* 装置なしの Source(`ThreadedSource::run_tx`): sample rate で消費して Sink(既定 `NullSink`)へ。burst の途中で空になれば Underflow。
* `GuiApp::set_rf_config` で方向を App の定義(`DIRECTION`)に固定。草案は直前の Core の RF の写しなので、送信 App の後の受信 App が TX を宣言しないように。
* `Core::reconfigure`(2026-11-04): 運転中の App の RF を、App を止めずに構成し直す(録音・送信端子の選択。汎用 App の RATE も同じ経路に載せ替えた)。
  それまでは App を止めて再起動していたので、一度メニューに戻る表示が出ていた。適用できない構成は元に戻して App は動き続ける。
  `ViewSource::generation` で、spectrum / waterfall の部品は processor が替わったら前の行を捨てて描き直す。
  configure が拒否した構成(送信できない端子など)は元の構成で 1 回だけ流し直す。装置が適用できなかった構成(stage 2〜4 の Fault)は元の構成で開き直す。
  却下の理由はシェルがどの App のページでも上部に赤で出す。
* `SigmfReader`(`RecordingSource` から抽出。録音を読む 2 つ目の利用者)。
* `Spear.Input` の `ListEntry`(8 件を超える選択。ページ送り)と `ui.askList`。
* `SystemModel`: `txMode / txGain / txPort / transmitting / txUnderflows / txArmed`。ステータスバーは送信 App の間 `TX <gain>`(送信中は赤で塗る)と送信端子、UNDF を出す。
* `EventKind::TxBurst / TxGain`。
* `tools/spear-replay`: ヘッドレス送信(`--arm` が無ければ Core が拒否する)。

## 実機(2026-11-04、FZ-G2 + B210 5IWG5D5、TRXA → 減衰器 → STD-T99 機)
`spear-replay ~/spear/golden/std_t99/ch18 --tx-gain 0 --port TRXA --seconds 30 --freq-err-ppm <個体の LO 誤差>`:
146.734375 MHz(CH18 は +250 kHz)、4 Msps、120,000,000 sample を 1 burst、underflow 0、seq_error 0、burst ack。
**STD-T99 機で受信できた**(自分の送信機の録音を送り返したもの)。送信キューは常に 32/32(読み込みが装置より速い)。

## 未検証・残り
1. burst の外(App 起動中・停止後)の LO 漏れを受信機で測る。
2. GUI の App からの実機送信(ヘッドレスでは確認済み)。STOP の遅れはキュー分(32 block = 4 Msps で約 130 ms)。即時に止めるのは TX MASTER の SAFE。
3. 録音の欠落・retune は表示だけで、連続した IQ として最初の周波数で送る(送信の途中で周波数は変えない)。
4. 送信ゲインの上限は装置の範囲(B210 で 89.75 dB)。現場の上限を site.conf で決める仕組みは無い。
5. 送信中の spectrum は送っている IQ(App 内の `replay.tx`)を表示するもので、空中の信号ではない。
6. `--source file:<録音>` で起動すると、`RecordingSource` は再生する録音の center / rate を変えないので、送信する録音が同じでない限り
   `configCurrent` が false のままで TRANSMIT できない(開発時だけの制約)。
