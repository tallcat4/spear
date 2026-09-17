# STD-T98 モニタ — 移植と欠陥レポート (§14 M3)

`../std-t98-tools`(Python/GNU Radio, 30ch デジタル簡易無線受信)の S.P.E.A.R. 移植。
M3 の主眼(§14)は「既存 DSP を修正せず任意の中間 stream に観測点(Scope/Waterfall/Constellation)を足せること」。
機能追加ではなくプラットフォーム検証が目的。

## 秘話モデルの規模判定 (§3.4 / §14 M3 の前提条件)
`core/secret/cracker.py` を読んだ結果:
| モデル | 構造 | 用途 | §3.4 |
|---|---|---|---|
| `ambe2_ffnn` (503 KB, input 980→128→2) | MLP 1 層 | 主力。全 32767 鍵の平文/暗号判定 | **道1(C++ 再実装)で自明** |
| `ambe2_hybrid` (3.8 MB, Conv1d×3 + BiLSTM×2 + attn) | 大 | full search 冒頭の「そもそも平文か」1 回判定のみ | **道2(ONNX)**。両モデルとも劣化なく ONNX 変換済み(2026-09-17) |
**判定: 道1で閉じる**(主力 ffnn は MLP)。単一プロセス・C++ 維持(§3.4)。段階 2 で実装。
→ 実装(2026-09-17、下記「秘話」): ffnn は道1(safetensors 直読み + C++ 推論)、hybrid は道2(ONNX Runtime、必須依存)。モデルはバイナリに埋め込む。

## 段階
- **1a(完了)**: プロトコルデコーダ純 C++ 移植。frame layout / dewhiten / RICH / SACCH・PICH(Viterbi K=5 + CRC-6/12)/ TCH 分割。
  Python 実装を真値に 40 ケースの golden test で等価性を確認(`tests/gen_golden.py` → `golden_vectors.hpp`)。SDR 不要・決定的。
- **1b(完了)**: 純 C++ 30ch 受信機 `apps/std_t98/receiver.*`(Qt/UHD 非依存)。resamp → `dsp::PfbChannelizer` → ch ごとに
  `dsp::FirInterpolator`×10 → 直交検波 → RC/sinc 受信フィルタ(firdes.py 移植)→ Gardner symbol sync(GR symbol_sync_ff 相当)
  → 同期語 SSE 検出 → 1a のデコーダ。フレームは radio.rx の sample index まで provenance を持つ。
  - 実機録音(`~/spear/recordings/rec_20260917_035942`, 4 Msps, ch1–3 送信)で 100 フレーム、SACCH CRC OK 98/100、
    PICH CSM 一致。**B2xx 個体の LO 誤差(この個体は約 +1 kHz)**は `ReceiverConfig.freq_err_hz`(site.conf)で打ち消す。
  - 検証方針: std-t98-tools 自体が RX 専用で実機で検証されているので、規格外の合成変調器を発明せず、
    実録音の 1.2 s 抜粋(`~/spear/golden/std_t98/ch3_pich.sigmf-*` + 期待値 `ch3_pich.golden.json`、リポジトリ外、無ければ skip)を golden test にした
    (13 フレーム、PICH 1 + SACCH 12、全 CRC OK、フレーム間隔 192 シンボル = 320000 サンプル)。
  - `dsp::PfbChannelizer`(共通、critically-sampled polyphase)。単一トーン分離を参照ベクトルで検証(channel c ↔ c·fs/M)。
  - FFTW プランナは dsp 全体で 1 mutex に統一(spectrum と channelizer が別 mutex だと危険だった)。
  - headless: `spear-std-t98-decode <sigmf base> --freq-err <Hz> --squelch -50 [--wav base] [--payloads f]`。
- **2(完了)**: AMBE 3600x2450 → PCM 8 kHz。`apps/std_t98/ambe/` に mbelib-neo の AMBE 経路だけを C++ で書き直した
  (IMBE・SIMD・スレッドローカル・pffft なし、GPL-2.0-or-later のまま)。実録音の TCH ペイロードを pyambelib(Python)で
  復号した golden(音声を含むのでリポジトリ外 `~/spear/golden/std_t98/ambe_golden.txt`、無ければ skip)と比較:
  fec_demod 完全一致、PCM は 98.1% 一致・最大 1 LSB 差(float 演算順)。
- **1c(完了)**: UI — 30ch 俯瞰スペクトラム + 選択chアイパターン + フレーム表示 + 音声再生。
- **3(完了、2026-09-17)**: 秘話(下記)。

## 秘話(音声スクランブル解除、§3.4)
`../std-t98-tools` の `core/crypto/*`、`core/secret/cracker.py`、`std_t98_multi_audio_service.py` の秘話部分、`std_t98_multi_secret_service.py` を
`apps/std_t98/secret/` に移植した。tools では別プロセス + UDS だったものを **単一プロセス内のワーカースレッド 1 本** にした(§3.4)。

| 部品 | 内容 | 検証 |
|---|---|---|
| `pn.*` | 15 bit LFSR の PN 196 bit(鍵 = 初期状態 1..32767)、ThumbDV 順 / mbelib d 順の keystream | Python `generate_pn_sequence_196` / `descramble_burst` と同値(`test_secret.cpp`) |
| `models.*` | `apps/std_t98/models/` の `ambe2_ffnn.safetensors` と `ambe2_hybrid.onnx` を `.incbin` でバイナリに埋め込む(実行時のパス無し、read-only rootfs 可) | `EmbeddedModelsLoad` |
| `safetensors.*` | safetensors の最小リーダ(メモリ上、F32 のみ、JSON ヘッダは自前の部分パーサ) | ffnn のロード |
| `ffnn.*` | `AMBE2Classifier`(Linear 980→128, ReLU, Linear 128→2)の C++ 推論。入力が 0/1 なので fc1 は「立っているビットの列の総和」 | torch と logits 最大差 9.5e-6、argmax 一致(golden 64 ケース) |
| `hybrid.*` | `AMBE2HybridClassifier`(Conv1d×3 + BN + GELU、BiLSTM×2、attention pooling)を ONNX Runtime(必須依存、`onnxruntime-cpu`)で。モデルはメモリから `Ort::Session` | torch と最大差 3.8e-6(export 時 512 乱数 + golden 64) |
| `cracker.*` | `SecretCracker` の移植。今の鍵の検証 → 全チャネル共通キャッシュ(16、上位 2 検証)→ 全鍵探索(hybrid 平文判定 → ffnn 全 32767 鍵 → hybrid 採点 → ブロック 2 で順位) | Python と 7 ケースすべて同じ鍵・同じ経路(下記 E2E) |
| `tracker.*` | チャネルごとの要求ポリシー(5 バーストで最初の要求、10 ごとに再確認、同時 1 要求、セッション番号で古い結果を捨てる) | 単体テスト |
| `worker.*` | 専用スレッド。モデルのロードもこのスレッド。要求は同一チャネルの未処理分を置き換え、停止時は探索を打ち切る | 単体テスト、TSan |

**データの流れ**(DSP thread): TCH フレーム → SACCH(CRC OK)の `call_stat == 1` で秘話呼と判定 → `AmbeDecoder::decode_3600(block, pcm, keystream)` が
FEC 後の 49 bit に鍵の PN を XOR してから音声合成(`secret_voice.py` と同じ位置)→ XOR 前の 49 bit × 4 を `Tracker` の窓に積む → 要求が立てば
`Worker::submit`。結果はワーカースレッドから mailbox に入り、DSP thread が次のブロックの先頭で取り込む(`secret_drain_results`)。
鍵が変われば PN を作り直す。結果は Event(`secret chN key K (full search hit, 0.58 s)`)にも残す。

**ビット順の事実**: Python の `THUMBDV_MAP`(keystream の並べ替え表)と C++ デコーダの `kThumbDv`(ThumbDV bit i ↔ mbelib `d[kThumbDv[i]]`)は
同じ表。したがって **mbelib d 順では `d[j] ^= pn[frame*49 + j]` と並べ替えなしで掛かる**。モデル入力は学習時どおり ThumbDV 順のまま。
テスト `DescrambleMatchesPythonInBothBitOrders` が両方の順で同じ結果になることを確認する。

**tools との差(意図的)**:
- SACCH CRC 不良のフレームは tools では `call_stat = 0`(平文)扱いになり秘話セッションを破棄していたが、SACCH は 2% 程度落ちる(実録音 98/100)。
  ここでは **CRC OK の SACCH だけで秘話判定を更新し、不良フレームでは前の値を保つ**。セッションは同期バースト(PICH)、平文の SACCH、squelch 閉で終える。
- 鍵はセッション(呼)をまたいで保持し、次の秘話呼ではまず「今の鍵」を検証する(tools と同じ)。鍵が無い間の秘話音声は tools と同じくスクランブルのまま鳴る(活動が分かる)。
- 音声を出していないチャネルでも窓は積む(選択を切り替えた瞬間から復号できる)。FEC だけなので DSP 負荷は無視できる。
- 秘話解読は App の主要機能なのでビルドオプションにしない。ONNX Runtime は必須依存、モデルはリポジトリ(`apps/std_t98/models/`、作者本人が学習、
  ライセンスは本体と同じ)に置いてバイナリに埋め込む。切り分け用に `std_t98.secret=0` で探索だけ止められる。

**モデル**(`apps/std_t98/models/README.md`): `ambe2_ffnn.safetensors`(C++ が直接読む)、`ambe2_hybrid.safetensors`(学習の原本)、
`ambe2_hybrid.onnx`(`apps/std_t98/tools/export_secret_onnx.py` で変換。opset 17、入力 `input` [N, 980]、出力 `logits` [N, 2]。変換時に torch と比較し、
512 乱数入力で最大差 5.7e-6、argmax 100%。しきい値を超えると非 0 で終わる)。モデルファイルが変わると `secret/models.cpp` が再コンパイルされる(`OBJECT_DEPENDS`)。

**検証(実録音 + 自己スクランブル)**: 手元の録音に秘話呼は無い(`call=0` のみ)。そこで `~/spear/golden/std_t98/ch3_payloads.txt`(実録音の平文 TCH 12 フレーム)を
既知の鍵でスクランブルし、Python(`gen_secret_golden.py`、torch)と C++ の両方で `resolve` させた。鍵 1 / 12345 / 32767 / 20000 / 777、窓 5 と 10、
今の鍵あり / 別の鍵あり / 平文 — **7 ケースすべて Python と同じ鍵・同じ経路**。全鍵探索は C++ 単一スレッドで 0.58 s(1 ブロック)/ 1.08 s(2 ブロック)、
Python(torch、マルチスレッド)は 0.37 / 0.65 s。golden は音声由来なのでリポジトリ外(無ければ skip)。
**実機の秘話呼**: 秘話設定した無線機の送信で鍵が見つかり音声が復号できることを確認(2026-09-17)。録音の抜粋(鍵つき)を golden に加えて
受信 → 鍵探索 → 復号の通し回帰テストにするのが次の仕事。

**鍵探索の実行場所**(STATUS の宿題): DSP thread ではなく `secret::Worker` の専用スレッド。全鍵探索 0.6–1.1 s の間も DSP thread は止まらない。ORT には
スレッドプールを作らせない(intra-op 1)— DSP thread から CPU を奪わないため、また ORT 内部の同期は TSan から見えず偽陽性を出すため。

## STD-T98 物理層メモ(移植で確定)
- フレーム 384 bit = 192 シンボル(4値 FSK -3/-1/+1/+3、2400 baud)。SW(20) RICH(16) SACCH(60) TCH1(144) TCH2(144)。
- ビットマップ: +1→00 +3→01 -1→10 -3→11。dewhiten は 9-bit LFSR、先頭 10 シンボル(SW)を除く 182 シンボルに乗算。
- SACCH/PICH 共通トレリス K=5: g1=x⊕x3⊕x4, g2=x⊕x1⊕x2⊕x4。SACCH depuncture 6/8、PICH 2/4。
- レート(std-t98-tools 既定): 1.2 Msps → resamp1 → 300k → PFB 48ch → 6.25k/ch → resamp2 ×10 → 62.5k → demod、sps≈26。

## 実機で見つかった移植バグ
- **symbol sync の max_deviation の単位**(2026-09-17、実機で発覚): GR `symbol_sync_ff` の `max_deviation` は「平均クロック周期の
  公称値からの絶対偏差 [samples/symbol]」(0.02 → 26.04 ± 0.02)。移植では比率と解釈して ±0.52 sample を許していたため、
  雑音区間で平均周期が誤った値に流れ着き、そこから抜けられずアイが開かない(運よく一度ロックすれば動く)挙動になった。
  録音再生では偶然ロックしていたので golden test は通っていた → **golden test は「動く」ことしか保証しない。パラメータの単位は
  一次資料(GR ソース)と突き合わせること**。修正後、平均周期の上下限テストを追加。

## ライセンス
`apps/std_t98/ambe/` は mbelib(2010)/ mbelib-neo(2025)由来で GPL-2.0-or-later(C++ に書き直しても派生物)。
S.P.E.A.R. 本体は **GPL-3.0 で公開する方針**(2026-09-17)なので、GPL-2.0-or-later のコードを静的リンクしても問題ない
(GPL-2 "or later" → GPL-3 として結合可)。ファイル頭の SPDX と著作権表示は残すこと。

## プラットフォーム検証メモ
- 純 C++ プロトコルデコーダは Qt/SDR 非依存ライブラリ(`spear_std_t98_protocol`)として App から独立にテストできた。
  golden vector を外部リポジトリの Python から生成する方式は、移植の等価性検証に有効(他の復号系 App でも使える)。
- 受信機の検証は「合成信号」より「実録音の抜粋を golden にする」ほうが正しかった。合成変調器は規格の送信整形が不明なまま
  パラメータ合わせになり(RRC/RC/NRZ どれも実信号の統計に合わない)、無駄だった。録音 → 抜粋 → golden の流れを
  `RecordingSource` + `spear-std-t98-decode --payloads` で回せる。ADS-B でも同じ方針にする。
- 30ch 受信機は 4 Msps 入力で実時間の約 1 倍(1.2 s を 1.2 s で処理、-O2 単スレッド)。GUI ではワーカースレッド 1 本で足りるが
  余裕は小さい。FirInterpolator ×10 を 30 本回す部分が支配的 → 必要なら PFB 出力 6.25 kHz のまま demod する案(sps≈2.6 は
  Gardner には厳しい)か、×4 に落とす案。
- `Provenance` の連鎖(resamp1 → PFB → 補間 → シンボル判定点)でフレーム先頭を radio.rx sample index に戻せた(§4.5)。
  PFB の provenance は「M 入力 → 1 出力」の整数比で表せるので StageInfo で十分だった。
- AMBE の float 経路は ±4447 でソフトクリップされる(mbelib の short 経路は ×7)。AudioSink に出すときは ×7 して 16 bit に。
