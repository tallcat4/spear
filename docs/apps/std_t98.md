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
  秘話(ffnn 道1 / hybrid ONNX 道2)は保留。
- **1c(次)**: UI — 30ch 俯瞰スペクトラム + 選択chアイパターン + フレーム表示 + 音声再生。

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
