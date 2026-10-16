# SLPR 20CH RX — 特定小電力トランシーバーの同時受信(2026-09-27)

422 MHz 帯の特定小電力トランシーバー(単信 20 ch、狭帯域 FM)を **20 チャネル同時に受信して音声にする** App。
STD-T98 MONITOR と同じ骨格(resamp1 → PFB → チャネルごと)の 2 つ目の利用者であり、目的は機能ではなく
「多チャネル受信の骨格を、デジタル固有のもの(シンボル同期・フレーム・秘話)無しで組めるか」「観測点を低レートの stream に付けられるか」の検証。

## チャネル表(`apps/slpr/channels.hpp` が唯一の真値)
各社の対応表(ICOM IC / STANDARD FTH・PK・CL・HX / MOTOROLA MS50・MS80・CL08 / ALINCO DJ / KENWOOD UBZ / UNIDEN / Wedge Talkie)から。
呼び方は **ICOM 準拠の通し番号 01–20**。中継用(440 MHz 帯)は扱わない。

| 区分 | ch | 周波数 | 帯域中心からのオフセット |
|---|---|---|---|
| 単信 11ch | 01–11 | 422.0500–422.1750 MHz、12.5 kHz 間隔 | −125 … 0 kHz(bin −10 … 0) |
| 単信 9ch | 12–20 | 422.2000–422.3000 MHz、12.5 kHz 間隔 | +25 … +125 kHz(bin +2 … +10) |

帯域中心は ch11(422.1750 MHz)で、20 ch が ±125 kHz に収まる。ch11 と ch12 の間(422.1875)だけ 1 スロット空く。

## 構成
```
apps/slpr/
  channels.hpp   チャネル表(番号・名前・周波数・PFB の bin・区分)
  receiver.*     純 C++ 受信機(Qt/UHD 非依存): 回転(帯域中心 → DC)→ resamp1 ÷10(4 Msps → 400 kHz、127 tap)→ PFB 32 × 12.5 kHz(16 tap/branch)
                 → [20ch] quadrature demod(±2.5 kHz → ±1.0)→ 音声 LPF 3 kHz(63 tap)→ 一次 DC 阻止(200 Hz)→ squelch のゲート → 音声 12.5 kHz
  slpr_app.*     GUI App: 観測点(slpr.band 400 kHz / slpr.channel 12.5 kHz / slpr.audio 12.5 kHz)、ミキサ、AudioSink、squelch の event
  SlprPage.qml   上: 20ch 俯瞰スペクトラム(マーカー 01–20)/ 中: チャネル格子 2 × 10 / 下: 選択 ch の IQ スペクトラムと読み出し
  tests/         受信機のテスト(合成 NFM)
```

| 項目 | 決定 | 理由 |
|---|---|---|
| RF | 4 Msps、LO = 帯域中心 − 250 kHz | STD-T98 と同じ構成(実機で Lossless drop 0 の実績)。ゼロ IF の DC スパイクは −250 kHz = resamp1 の阻止域(÷10 の折り返し先 +150 kHz は bin +12 で ch20 の外) |
| PFB | 32 bin × 12.5 kHz、プロトタイプ 16 tap/branch(cutoff 6.25 kHz、遷移 ~2.6 kHz、阻止 −53 dB) | 隣接チャネル(±12.5 kHz)の占有帯域(±5.5 kHz)は 7 kHz 以遠で阻止域。STD-T98 の 8 tap/branch では隣の強い局が漏れて空きチャネルを開けた(テストで確認) |
| 復調 | PFB 出力の 12.5 kHz のまま quadrature demod。補間しない | NFM(偏移 ±2.5 kHz)の位相ステップは最大 1.3 rad で atan2 の折り返しに届かない。補間は情報を増やさない |
| 音声 | 12.5 kHz のまま AudioSink へ(PipeWire がリサンプル)。全チャネルは同じブロックで同じ数だけ音声を出すので**加算するだけ**のミキサ | STD-T98 の Mixer(フレームが非同期に届くので provenance の時刻に加算)は要らない。有理数リサンプラ(共通 DSP に無い)も要らない |
| squelch | 床相対(全 20 ch の中央値 + margin 8 dB)、ヒステリシス 3 dB、**attack 20 ms**、hold 150 ms。閉でも復調は回し、音声だけ 0 に | STD-T98 で学んだ床相対(AGC で床が動いても追従)。attack はこの App で追加: 1 ブロックだけの過渡(隣の局のキーアップ、インパルス雑音)で 150 ms の雑音を鳴らさない。閉でも回すのは、開いた瞬間に DC 阻止と LPF が定常であるため |
| DC 阻止 | 一次(200 Hz) | 送信機の周波数ずれ(±4 ppm ≈ ±1.7 kHz)は検波後の直流になる(800 Hz ずれで 0.32)。個体の LO 誤差は Core が打ち消すが相手の誤差は残る |
| Event | squelch の開閉だけ(1 送信 = 2 event、provenance: 開は超え始めたブロックの先頭、閉は開いていた範囲) | ブロックごとに出すと 250/s |
| 保存 | squelchMarginDb / selectedChannel / mute / allChannelAudio / bandView.* | 個体値は site.conf(`slpr.band_center_hz`、`slpr.squelch_margin_db`) |
| UI に置かないもの | LO / rate / 帯域中心の操作、音量 | 目的が確定した App の規約(`docs/app-development.md`)、音量は OS 側 |

## 検証
- `apps/slpr/tests/test_receiver.cpp`(合成 NFM = 偏移 ±2.5 kHz の周波数変調そのもの。STD-T98 のような送信整形の推測を含まないので合成でよい):
  チャネル表の一致(周波数・区分・bin)、**2 ch 同時**(ch03 に 1 kHz、ch15 に 700 Hz → それぞれの音声にそのトーンだけ、RMS 0.71、他 18 ch は閉じて無音、全 ch の音声が同じ歩み)、
  隣接(強い局 −14 dBFS の隣 ±12.5 kHz と群の境 +25 kHz が開かない、漏れは床に埋もれる)、雑音だけで開かない、provenance(送信開始 0.3 s → 開 index 1.2M ± 1 ブロック)、
  hold 後に閉じる、LO オフセット(帯域中心 − 250 kHz)、送信機 +800 Hz ずれで直流が残らない。
- `apps/tests/test_apps.cpp` `SlprReceivesOneChannelOnSynthetic`: SyntheticSource の FM を ch15 の位置(+62.5 kHz + 250 kHz)に置いて start → 4 Msps / 421.925 MHz、
  ch15 だけ開く、`slpr.audio` に 1 kHz、`slpr.channel`(12.5 kHz)の観測点も流れる、開 event、drops 0、stop 後 consumer 0。
- 実機(2026-09-27、B210 5IWG5D5、gain 30、RXA): STREAMING、LO LOCK、Lossless drop 0、DSP thread 19 %。運転中に **ch12(422.2000 MHz)で実信号**
  を受信(下図: 8 回開いて計 15 s、チャネル IQ のウォーターフォールに NFM のバースト、帯域スペクトラムの max hold にピーク −97 dBFS)。
  音声の聴感は作者の確認待ち(スクリーンショットは `--set audio_device=null`)。

![slpr](../gui/slpr.png)

## 基盤への指摘(実装メモ)
1. **`ViewProcessor` は nfft より短いブロックを黙って捨てていた**(`appfw/src/view_processor.cpp`)。12.5 kHz のチャネル IQ は radio.rx の 16384 sample
   ブロックあたり 51 sample しか出ないので、`slpr.channel` に付けた観測点(nfft 256)は何も表示しなかった。STD-T98(62.5 kHz、512 sample)と DEMOD(240 kHz)は
   偶然 nfft を超えていた。**直した**: 短いブロックは nfft ぶん溜めてから 1 フレームにする(header は先頭ブロック、flags は OR、generation が変われば捨てる)。
   `SlprReceivesOneChannelOnSynthetic` が `channelView` の行数で固定する。
2. **quadrature demod は 3 つ目の App 内実装になった**(DEMOD / STD-T98 receiver / SLPR receiver)。`docs/dsp-boundary.md` の昇格ルール(2 つ目で抽出)を
   超えているので、`dsp::quadrature_demod`(x·conj(x[n−1]) の偏角、利得は引数)への抽出が次の仕事。一次 IIR(DC 阻止 / de-emphasis)も同じ(DEMOD / SLPR)。
   床相対 squelch(中央値 + margin、attack / hyst / hold)も STD-T98 と SLPR で 2 回目 — ただし「開く条件に CRC を混ぜる」など用途依存が混じるので、
   状態機械だけを共通化できるか要検討。
3. `--screenshot` 検証用に `--source synthetic --synth-fm <offset_hz>`(NFM トーン)を足した。合成源に変調信号を置く手段が GUI 側に無かった。
4. 有理数リサンプラは今回も要らなかった(12.5 kHz のまま PipeWire に任せた)。3 つの復調系 App がすべて回避できているので、共通 DSP に足す理由はまだ無い。

## 残課題
- 実機で音声の聴感と、2 局同時送信時のミックスの聞こえ方(クランプ ±1.0)。会場での AGC vs 手動ゲイン(STD-T98 と同じ宿題)。
- CTCSS / DCS(グループコード)による選択呼び出しは扱っていない(音声には出さず、表示だけなら PFB 出力から 67–254 Hz を検出すれば足りる)。
- 中継用チャネル(440 MHz 帯)は帯域が別なので、この App の対象外(別 App か帯域切替)。
