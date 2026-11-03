# STD-T99 DIGICOM — デジタル小電力コミュニティ無線の受信(2026-11-03)

ARIB STD-T99 Annex 2(デジタル小電力コミュニティ無線、通称デジコミ)の **1 チャネルを受信して、呼の情報と TOKUDER 音声を出す** App。
[std-t99-receiver-sharp](https://github.com/tallcat4/std-t99-receiver-sharp)(SDR# プラグイン、`a7889c2`)の移植。
原本の受信機は [std-t99-receiver](https://github.com/tallcat4/std-t99-receiver)、音声の復号器は [tokuder-neurodec](https://github.com/tallcat4/tokuder-neurodec) の C++ 移植で、
DSP の部品はもともと SPEAR から切り出したもの(FIR / PFB / RRC / Gardner)。**並列受信(全チャネル同時)は持たない**: 選択した 1 チャネルだけを受ける。

## チャネル表(`apps/std_t99/channels.hpp` が唯一の真値)
表 Annex 2-3-1。CH1–9 は 142.934375 MHz から、CH10–18 は 146.934375 MHz から 6.25 kHz 間隔。CH18(146.984375 MHz)は呼出チャネル(4.7)で、App の既定。
2 つの群は 4 MHz 離れているので、チャネルを変えると LO を動かす(`Source::retune`)。

## 構成
```
apps/std_t99/
  channels.hpp        チャネル表
  protocol/           物理層(デホワイト、RICH / SACCH / PICH / FACCH / TCH、ビタビ、CRC)とメッセージ(4.2)。原本のまま(名前空間だけ変更)
  dsp/frame_sync.*    同期語検出と同期の状態機械。STD-T98 の SyncCorrelator に「窓の平均を引いた SSE」と候補の DC を足したもの
  dsp/center_channelizer.hpp   PFB の bin 0 だけを計算する(選択チャネルは DC に回してある)
  receiver.*          1 チャネル受信機(入力 100 kHz、PFB 8 × 6.25 kHz)。原本のまま。RRC と Gardner は STD-T98 と共有(spear_std_t98_dsp)
  front.*             App 側の前段: 回転(チャネル − 実 LO)→ ÷10(127 tap)→ 400 kHz → ÷4(63 tap)→ 100 kHz
  session.*           メッセージの集約(SACCH スーパーフレーム、発信者名の組み立てと CRC-32)、フレームごとの JSON
  call.*              送信の区切り、呼の情報、選択呼出の着信判定(表 Annex 2-4-14)、受信履歴 100 件
  voice/              TOKUDER: 軟判定の誤り訂正、tokuder48_causal_v1 の推論(tokuder.*)、音声の時間軸(timeline.*)、復号スレッド(worker.*)
  storage.*           SAVE(.burst / .llr / .jsonl / .wav、原本の FORMAT.md と同じ形式)
  monitor.*           上の全体(Qt / UHD 非依存)。App・spear-std-t99-decode・テストが同じものを使う
  std_t99_app.*       GUI App
  position_map.*      発信位置の地図(QQuickPaintedItem: ピンとタッチ。地図データ・表示範囲・下地は widgets/map で ADS-B と共有)
  StdT99Page.qml      チャネル格子 / 通話 / 位置 / 待受 / 帯域スペクトラム / アイ / 品質 / 履歴
  models/             重み(バイナリに埋め込む。models/README.md)
  tests/              規格書 7.7.2 の試験信号、選択呼出の表、実機録音の golden
tools/std_t99_decode  spear-std-t99-decode <sigmf base> [--ch N] [--save dir] [--wav f]
```

| 項目 | 決定 | 理由 |
|---|---|---|
| RF | 4 Msps、LO = チャネル − 250 kHz | STD-T98 / SLPR と同じ実績のあるレート。LO の DC スパイクをチャネルから離す |
| レート変換 | App 側の Front(整数比 ÷40)。Receiver は原本で試験した 100 kHz / PFB 8 のまま | 原本は SDR# の任意レートを sinc 補間で 100 kHz にしていた。受信機の数値を変えずに済む。原本の記録では 50 kHz(初段の間引き 1)だと試験信号でシンボル経路の CRC がほとんど通らなかった |
| 回転 | 倍精度の複素数の漸化式(ブロックごとに正規化) | 4 Msps で毎サンプル cos / sin を呼ぶより軽い |
| AFC | 行わない(原本どおり) | 受信機がフレームごとに同期語の平均(DC)を差し引く。入力を回すとフレームの途中で DC が動き、試験信号で CRC を落とした(原本の記録) |
| 音声の復号 | 専用スレッド(VoiceWorker)。DSP thread は「48 bit を 1 つ復号 / 流れを終える」の命令を順に出す | 規約(推論は DSP の外)。命令の順序は原本の Decoder の呼び出し順と同じなので、音声も同じ |
| 待受 | 再生だけを決める。表示と保存は常に全通話 | 原本と同じ |
| event | 送信の開始だけ(radio.rx の index 付き)。フレームごとには出さない | イベントログを溢れさせない |
| 保存 | SAVE = 復号結果、REC IQ = radio.rx の SigMF | REC IQ は golden 用(JR TRAIN RADIO と同じ経路) |
| 地図 | ADS-B の地図(Natural Earth 1:10m、埋め込み)のデータ・表示範囲・下地の描画を `widgets/map`(`spear_map`)に抽出して共有。最後の送信を赤いピン、履歴のほかの発信者を橙の点。測位無効と日本の範囲外は立てない。1 か所だけのときは 60 km 幅 | 原本のプラグインと同じ表示。2 つ目の利用者なので抽出(docs/dsp-boundary.md の昇格ルール)。原本の日本地図(鉄道・道路・市街地・日本語の地名)より粗いが、オフラインの全世界データを 1 つにする |

## 原本からの変更
- **送信の区切り**: 実機は 1 送信の頭に同期バーストを続けて送る(2026-11-03 の実機: 音声で 2 個、緊急通信の位置通知で 6 個)。原本は PICH1 付きの同期バーストごとに呼を分けていたので、
  1 送信が受信履歴の 2〜7 件に分かれた(1 フレームだけの呼が並ぶ)。前の呼が終話しておらず、0.25 s 以内に続き、発信者が同じなら同じ送信とする(`CallMonitor::frame`)。
  間隔は受信機の時刻で測る(壁時計だと録音の復号で狂う)。規格書の試験信号は同期バーストが 1 個なので、原本の試験では見えなかった。
- **周波数ずれの表示とアイパターン**: 原本は AFC をせず、フレームごとに同期語の平均(DC)を差し引いて判定する(これは変えていない)。ただしアイパターンは
  差し引く前の信号を描いていたので、ずれの分だけ上下にずれた(実機の録音で +1.4 レベル。作者の指摘)。アイは同期語の DC を引いて描く。
  ずれの表示も、原本の「同期中の弁別器出力の平均」(真値の 6〜7 割に出ていた)をやめ、同期語の DC ÷(post_filt_gain × post_sync_gain = 1.15)× 315 Hz にした。
  試験信号で −600〜+420 Hz のずれが ±30 Hz 以内、実機の録音で +467 Hz(B210 の +2.93 ppm = +431 Hz に送信機の +0.25 ppm)。
- Shift_JIS → UTF-8 は Windows の API の代わりに glibc の iconv(CP932)。
- 重みは `.incbin`(`voice/model_data.cpp`)、`voice/tokuder.cpp` だけ `-O3 -ffast-math`(内積のベクトル化。AVX2 / FMA は実行時に選ぶ)。

## 検証
- **規格書 7.7.2 の試験信号**(`tests/test_receiver.cpp`): 4 送信 / 29 フレームがすべて CRC 正常、発信者 127-0123456、宛先 127-0000654、発信者名「無線試験No.9」、
  N35°40.2565′ E139°45.0235′ 7 m、GPS 時刻、緊急通信 1、グループ 3、音声 = 音声フレーム × 640 sample。4 Msps(LO から +250 kHz、ずれ +180 Hz)と 100 kHz(ずれ −600〜+420 Hz)。
  80 ms 格子に対する開始位置 20 通り、雑音 60 s でフレーム 0。選択呼出の表(受信接続試験 表 Annex 2-7-6 の組み合わせ)。
  変調器は試験専用(`tests/spec_iq.hpp`)。規格の試験信号のシンボル列と規格の整形なので、「合成の変調器で golden を作らない」規約の対象外とした(実録音の golden を主にする)。
- **実機**(2026-11-03、FZ-G2 + B210 5IWG5D5、TRXA、AGC slow、CH18、送信機 1 台を近くで): 118 s で 290 フレーム、**すべて CRC 正常**、音声フレーム 258。
  6 送信(音声 4 回、緊急通信 1 の位置通知 2 回)、発信者 034-0010744、発信者名「TallCat」(CRC-32 一致)、一斉呼出。復号した音声は明瞭(作者が聴いて確認)。
  golden は `~/spear/golden/std_t99/ch18.sigmf-*`(`spear-headless` で録ったので LO 補正なし。周波数ずれの表示は +467 Hz)。
  録音の再生で App の画面も確認した。`docs/gui/std_t99.png` は試験信号(4 Msps の SigMF にしたもの)の再生で、地図に東京のピンが立つ。
- 性能: 復号器は 1 フレーム(20 ms)あたり約 1.6 ms(1 コアの 8 %)。DSP thread は合成信号で 9 %(4 Msps)。

## 既知の問題・残り
1. アイパターンの DC はシンボル経路の同期語から取るので、捕捉経路(acquire)だけで出た送信の最初のフレームでは前の値が残る(表示だけ)。
2. **地図の細かさ**: 地名は英語(Natural Earth populated_places_simple)、鉄道・道路・市街地は無い。原本のプラグインの日本地図(name_ja、鉄道、道路)が要るなら `widgets/map/build_map.py` に層を足す。
3. 実機で未確認: 個別通話・グループ通話・位置(GPS 測位あり)・遠隔制御の要求、弱い局、CH1–9 の群。待受(個別 / グループ)で再生が止まること。
4. 2 周波待受(4.7.2)・送信・応答は持たない(原本と同じ)。
5. 音声の大きさ: `dsp::Leveler`(−18 dBFS)を AudioSink の手前に通す(`docs/audio.md`)。保存と `std_t99.audio` には掛けない。
