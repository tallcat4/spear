# ADS-B — 実装と欠陥レポート(要件 §8.2、STATUS 次の仕事 #2)

1090 MHz Mode S / Extended Squitter の受信 App。目的は機能ではなく基盤の検証:
**radio.rx を 8 Msps のままフルレートで Lossless 消費する初めての App**、高頻度事象での Event/provenance の運用、表と極座標という新しい表示。
一次資料は ICAO Annex 10 Vol. IV / DO-260B、ビット位置の記法と既知ベクトルは Junzi Sun「The 1090MHz Riddle」、参照実装は dump1090 / readsb(GPL)。

## 構成
```
apps/adsb/
  protocol/modes.*   CRC-24(0xFFF409)、DF 判別、AP 形式(剰余 = ICAO)、高度(Q ビット / Gillham: dump1090 の decodeID13Field / ModeAToModeC 移植)、
                     スコーク、便名、CPR(大域 / 局所、NL 表)、速度(TC19 subtype 1–4)、距離・方位
  dsp/ppm.*          プリアンブル検出(パルスチップ 0/2/7/9 vs 無音チップ、電力比)+ PPM ビット判定。dump1090 の detectModeS を任意の整数 spc に一般化
  receiver.*         回転(−lo_offset)→ 低域 FIR(±1.5 MHz、47 tap)→ |x|² → PpmDecoder → CRC →(任意)1 bit 訂正 → Message。既知 ICAO キャッシュ(300 s)
  aircraft.*         AircraftTable: ICAO ごとの集約、CPR 偶奇ペア(10 s)→ 前回位置からの局所解、基準位置からの距離・方位、期限切れ(60 s)
  map/               ミニマップ: world.spearmap(Natural Earth 1:10m + OurAirports、build_map.py で生成、バイナリに埋め込み 3.9 MB)、map_data(展開)、
                     view.hpp(局所等距円筒、自動フィット)、map_item(QQuickPaintedItem: 陸 / 湖 / 境界 / 都市 / 空港 / 滑走路 / 航跡 / 機体)
  adsb_app.* / AdsbPage.qml   GUI(表 / ミニマップ / 最後のフレームの電力窓 / 統計)
tools/adsb_decode  spear-adsb-decode <base> [--fix] [--ref lat,lon] [--golden out.txt]
```

## 設計の決定
| 項目 | 決定 | 理由 |
|---|---|---|
| RF | 8 Msps(4 サンプル/チップ)、帯域 8 MHz、LO = 1090 MHz − 2 MHz(`adsb.lo_offset_hz`) | 2.4 Msps(dump1090)より弱信号の復号率が上がる。ゼロ IF の DC スパイクを信号帯域(±1.5 MHz)の外へ。振幅復調なので LO 誤差は無関係 |
| 帯域制限 | 回転 → 実係数低域 FIR → 電力 | 全帯域の電力を取ると雑音帯域 8 MHz で 6 dB 損する。回転量は Core の実 LO から導く(録音再生でも同じ経路) |
| 検出 | 電力比 **6 dB**、各パルス −8 dB 以上、spc 以内で最大に揃える | 平方根を取らない(8 Msps で sqrt は無視できない)。閾値は実録音で決めた(下記) |
| 対応 DF | 17/18(ES: 識別・空中位置・速度・TC28 スコーク)、11、AP 形式 0/4/5/16/20/21(既知 ICAO に一致するときだけ) | dump1090 の実績ある範囲。地上位置(TC5–8)はフラグのみ |
| 1 bit 訂正 | シンドローム表(112 本)で O(1)。**既定 on**、訂正したフレームは ICAO が既知のときだけ受理(FIX 1BIT キーで切れる) | 実録音で +5 %。偶然シンドロームに一致する確率は候補あたり 112/2^24 なので、既知 ICAO の条件で幻の機体を作らない |
| Event | 新規機 / 初回位置確定 / 消失だけ。provenance は契機フレームの sample 範囲 | フレームごとでは都市部で 1000/s を超える。フレーム数は統計 |
| 現場固有値 | `adsb.lo_offset_hz` は site.conf。`adsb.ref_lat` / `adsb.ref_lon` は**任意**(あれば局所 CPR の初期解と 600 km 超の位置の棄却に使う。表示は依存しない) | 可搬機なので基準位置を前提にしない |

## 検証
- `apps/adsb/tests/test_protocol.cpp`: 既知ベクトル(KLM1023、偶奇ペア → 52.2572 N 3.91937 E / 38000 ft、速度 159 kt / 182.88° / −832 fpm、
  対気速度 375 kt / 243.98°、DF5 スコーク 0356)、CRC・1 bit 訂正、DF11 / DF4 をパリティから組み立てて往復、Gillham の並び、距離・方位。
- `test_receiver.cpp`: 合成 PPM(規格のパルス配置に既知フレームを置き、IF +2.03 MHz と雑音を加えたもの。変調器の発明ではなく PPM は規格の定義そのもの)で
  回転 → FIR → 電力 → 検出 → CRC → **provenance をサンプル単位**で確認(ブロック境界をまたぐフレームを含む)。AP 形式の照合、1 bit 訂正、航空機表の CPR。
- golden(実録音): `~/spear/golden/adsb/es.sigmf-*`(`rec_20260917_130714` の 1.0–4.0 s、24M サンプル)+ `es.golden.txt`(136 フレーム: DF17 45 / DF18 6 / DF11 16 /
  AP 69。`spear-adsb-decode --golden` の出力)。全フレームが出ること。結果はブロック長(4096 / 16384 / 65536)によらない。

## 実機(2026-09-17、羽田近傍、B210 gain 30、1090 MHz ホイップ)
IQ RECORDER で 1090 MHz − 2 MHz / 8 Msps を 30.6 s 録音(`rec_20260917_130714`、978 MB)。GUI の ADS-B も実機で動作した(作者確認)。
`spear-adsb-decode` の結果と検出パラメータの掃引(fix なし):
| ratio | 候補 | 受理フレーム | 位置 | 備考 |
|---|---|---|---|---|
| 12 dB | 233 | 219 | 14 | |
| 9.5 dB | 987 | 847 | 79 | 最初の既定 |
| **6 dB** | 40k | **1223** | 117 | **既定に採用**(候補 1.3k/s、CPU に影響なし) |
| 4.5 dB | 540k | 1237 | 117 | 飽和 |
| 3 dB | 3.9M | 1239 | 117 | 候補 130k/s で CPU 過多 |
cutoff 1.2 MHz は 801、2.5 MHz は 630(1.5 MHz が最良)。taps 23 は 47 とほぼ同じ(864 vs 847)。1 bit 訂正で 1223 → 1289(+5 %)。
6 機(JAL378 / ANA98 は 675 ft で最終進入、N756HA は 3825 ft 上昇中、XAX522、ANA78、AP のみ 2 機)。DF 別: DF0 268 / DF4 114 / DF5 99 / DF11 106 / DF16 15 / DF17 204 / DF18 36 / DF20 5。
録音再生の GUI(`docs/gui/adsb.png`): 41 フレーム/s、7 機 / 位置 5、DSP 60 %(CPU 1.7 GHz)、drops 1(起動直後のみ)。

## 基盤への指摘(欠陥レポート)— 直したもの
1. **`dsp::Provenance::then()` の群遅延の符号が逆だった**(`dsp/include/spear/dsp/stage.hpp`)。段は因果(出力 k は入力 [k−(N−1), k] から)なので
   出力 index → 入力 index は群遅延を **引く**。足していたため 2 × 群遅延ぶん遅い index を返していた。STD-T98 では 1 フレーム 320k サンプルに対して
   数十サンプルなので気付かなかった。ADS-B の合成 PPM テストがサンプル単位で固定した。
2. **`FirDecimator` は出力ごとの内積呼び出しがタップ数によらず ~11 ns/出力かかっていた**(自動ベクトル化した 8 累算器ループの前後処理と呼び出し)。
   8 Msps では 47 tap で 22 ns/sample = 1 コアの 18 %。`decim == 1` のときはタップ外側の axpy(`__restrict`、L1 に収まる 1024 float の塊ごと)にして
   1/2 以下(47 tap: 179 → ~100 ms / 8M sample)。カーネル(`dot` / `axpy`)は `target_clones("avx2,fma","default")` で AVX2/FMA 版も持つ
   (バイナリは既定の x86-64 のまま、実行時に選ぶ。FMA で丸めが 1 ulp 変わり得るが golden は全部通る)。`decim > 1` の経路(STD-T98 の resamp1)は
   従来どおり内積で、複素 × 実の係数は 2 倍展開して実数版と同じ形にした。
3. **`RecordingSource` のループ再生が、録音長がブロック長の倍数でないと 1 周で止まっていた**(`core/src/recording.cpp`: 半端な末尾を読んで eof/fail が立つと
   `!data_` で 0 を返していた)。実機の録音はブロック単位で書かれるので出なかった。`is_open()` の判定に変え、`tests/test_recording.cpp` に回帰テスト。
4. **`std::complex<float>` の `operator*` は NaN 処理(`__mulsc3`)に落ちて遅い**。回転は float で手書きし、8 サンプル分の位相子を先に作って位相子を
   8 サンプルごとに進める(直列の 1 サンプル乗算より 4 倍速)。STD-T98 の `Receiver::process` の回転(サンプルごとに `std::cos/sin` + complex 乗算)も同じ形にできる。

## 基盤への指摘 — 残っているもの
5. **実測の負荷**: 受信機単体で 8 Msps 1 秒あたり ~160 ms(3 GHz、FIR ~95 ms、PPM 検出 ~55 ms、回転 ~10 ms)= 1 コアの 16 %。
   `spear-adsb-decode` は 53M サンプルの録音を 1.5 s。GUI では録音再生中に DSP 60 %(CPU 0.9 GHz、バッテリー powersave 時)、drops 0。
   合成源の場合は源の生成自体が同じ機で重い。PPM 検出は移動和(直列)とパルス / 無音和(ベクトル化済み)で、さらに削るならチップ和を 4 サンプル
   ずつの部分和にする。
6. **Event に generation が無い**: `Frame` は radio.rx の sample index を持つが generation を持たず、Event の `SampleRange.generation` は 0 で出している
   (STD-T98 も同じ)。Receiver に generation を渡す口を SDK として揃えるべき。
7. **表とミニマップは App 内**(`AdsbPage.qml` の ListView、`map/MapItem`)。地図は APRS / AIS / ラジオゾンデ等の候補 App でも要るので、2 つ目の利用者が出た時点で
   `map/` を `Spear.Widgets`(地図データは共通の資源)に抽出する。表の model は 250 ms ごとに作り直す QVariantList なので delegate も作り直される
   (数十機なら問題ないが、多数なら差分更新の model が要る)。ミニマップの下地は表示範囲が変わるたび(自動フィット中は 4 Hz)に描き直す。
   世界規模の幅では陸リングが多く 1 回数十 ms かかる(実用の 20–300 km では数 ms)。
8. **sc16 → cf32 変換と回転**はどの App でも書いている(demod / STD-T98 / ADS-B)。`docs/dsp-boundary.md` の基準(数学的定義、定数なし、2 つ目の利用者、参照ベクトル)を
   満たすので共通 DSP への昇格候補(回転は 4. の形で)。
9. **Event の頻度制御は SDK に無い**。App 側で「事象の粒度を選ぶ」規約で足りるか、DIAGNOSTICS のイベントログに間引きが要るかは実機で判断する。
10. **`--source file:` の再生は generation が進む**(ループごと)。受信機は入力 index の不連続で provenance の原点を取り直し、溜めた電力列を捨てる。
    これは App が各自書いている(STD-T98 は原点を最初のブロックに固定したまま)。SDK の規約として「generation 切替で受信機を reset する」を置くべき。

## 画面
`docs/gui/adsb.png`(実録音 `rec_20260917_130714` を再生。羽田 34L/34R への最終進入 2 機と出発 1 機、滑走路は OurAirports の実寸)。
