# 共通 DSP の境界(規約)

## 問い
「AM/FM 復調を共通部品に入れるか」。変調方式は無数にあり、同じ FM でも用途で定数が違う。
4FSK も BPSK も無いのに AM/FM だけあるのは説明がつかない。GNU Radio 並みを揃える覚悟をするか、App に任せるか。

## 判定基準(共通 DSP に入れる条件、すべて満たすこと)
1. **数学的に定義され、用途固有の定数を含まない**。FIR(係数は引数)は入る。「FM 放送の de-emphasis 75 µs」は入らない。
2. **2 つ以上の App が同じものを必要とする、または性能上の要衝である**(channelizer / decimator は §1.1 の CPU 負荷の集中点)。
3. **参照ベクトルで検証できる**。

**変調方式の名前が付いた部品は共通に置かない**。名前が付いた時点で用途定数が混入する。
`quadrature_demod`(x[n]·conj(x[n−1]) の偏角)や `magnitude`(|x|)は数学演算なので入る。
「FM 受信機」「4FSK 復調器」は App のコード(原始演算 + 用途の定数の組み合わせ)。

## 昇格ルール
共通部品は**予測して作らず、2 つ目の App が同じものを書いたときに抽出**する。抽出時に基準 1〜3 を満たすよう一般化する。
GNU Radio 的な網羅は目指さない。App 内部で GNU Radio を使う道 (§17) は残す。

## 共通側が提供するもの
| 種別 | 内容 |
|---|---|
| **段の規約** | `dsp/stage.hpp`: 入出力 rate と群遅延の宣言、`Provenance` による出力 index → 入力 index の機械計算 (§4.5、段は因果なので群遅延を引く)、TAP の置き場 |
| **原始演算**(基準を満たすもの) | 現状(2026-09-17): `design_lowpass`、`FirDecimator<T>` / `FirInterpolator<T>`(8/4 本累算のベクトル化カーネル、時間順 taps)、`PfbChannelizer`(critically-sampled polyphase)、`SpectrumEstimator`(sc16/cf32)、FFTW プランナの単一 mutex(`fftw_planner.hpp`)。App 内に留めているもの(2 つ目の App で抽出): quadrature_demod(demod / STD-T98 receiver で 2 回目 → 抽出候補)、一次 IIR、DC block、squelch、Gardner symbol sync、RC/RRC 設計 |
| **テスト用信号生成** | `SyntheticSource` の拡張(変調済みテスト信号)。これは検証基盤であり DSP 部品ではない |
| **参照ベクトルの枠組み** | 段単位・App 単位の回帰テスト (§12.3) |

## この線引きの検証
FM/AM App: channelizer + `quadrature_demod` + IIR(定数は App)+ resampler / channelizer + `magnitude` + DC block + resampler —
**共通の原始演算 + App の定数だけで書けるか**。書けなければ、足りないものが基準を満たすかを問う。
