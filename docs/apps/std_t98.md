# STD-T98 モニタ — 移植と実装メモ (§12 M3)

`../std-t98-tools`(Python/GNU Radio, 30ch デジタル簡易無線受信)の S.P.E.A.R. 移植。
M3(§12)では、既存の DSP を変えずに任意の中間 stream に観測点(Scope/Waterfall/Constellation)を追加できることも確認する。

## 秘話モデルの規模判定 (§2.5 / §12 M3 の前提条件)
`core/secret/cracker.py` を読んだ結果:
| モデル | 構造 | 用途 | §2.5 |
|---|---|---|---|
| `ambe2_ffnn` (503 KB, input 980→128→2) | MLP 1 層 | 主力。全 32767 鍵の平文/暗号判定 | **道1(C++ 再実装)で自明** |
| `ambe2_hybrid` (3.8 MB, Conv1d×3 + BiLSTM×2 + attn) | 大 | full search 冒頭の「そもそも平文か」1 回判定のみ | **道2(ONNX)**。両モデルとも劣化なく ONNX 変換済み(2026-09-17) |
**判定: 道1で閉じる**(主力 ffnn は MLP)。単一プロセス・C++ 維持(§2.5)。段階 2 で実装。
→ 実装(2026-09-17、下記「秘話」): ffnn は道1(safetensors 直読み + C++ 推論)、hybrid は道2(ONNX Runtime、必須依存)。モデルはバイナリに埋め込む。

## 段階
- **1a(完了)**: プロトコルデコーダ純 C++ 移植。frame layout / dewhiten / RICH / SACCH・PICH(Viterbi K=5 + CRC-6/12)/ TCH 分割。
  Python 実装を真値に 40 ケースの golden test で等価性を確認(`tests/gen_golden.py` → `golden_vectors.hpp`)。SDR 不要・決定的。
- **1b(完了)**: 純 C++ 30ch 受信機 `apps/std_t98/receiver.*`(Qt/UHD 非依存)。resamp → `dsp::PfbChannelizer` → ch ごとに
  `dsp::FirInterpolator`×10 → 直交検波 → RC/sinc 受信フィルタ(firdes.py 移植)→ Gardner symbol sync(GR symbol_sync_ff 相当)
  → 同期語 SSE 検出 → 1a のデコーダ。フレームは radio.rx の sample index まで provenance を持つ。
  - 実機録音(`~/spear/recordings/rec_20260917_035942`, 4 Msps, ch1–3 送信)で 100 フレーム、SACCH CRC OK 98/100、
    PICH CSM 一致。**B2xx 個体の LO 誤差(この個体は約 +1 kHz = +2.93 ppm)**は当初 `ReceiverConfig.freq_err_hz`(site.conf `std_t98.freq_err_hz`)で
    打ち消していたが、装置の性質なので 2026-09-19 に Core の Radio へ移した(`radio.freq_err_ppm`、LO 側で補正)。`freq_err_hz` は帯域中心を DC へ
    戻す回転と、録音ファイル固有の残留誤差(golden / `--freq-err`)にだけ使う。
  - 検証方針: std-t98-tools 自体が RX 専用で実機で検証されているので、規格外の合成変調器を発明せず、
    実録音の 1.2 s 抜粋(`~/spear/golden/std_t98/ch3_pich.sigmf-*` + 期待値 `ch3_pich.golden.json`、リポジトリ外、無ければ skip)を golden test にした
    (13 フレーム、PICH 1 + SACCH 12、全 CRC OK、フレーム間隔 192 シンボル = 320000 サンプル)。
  - `dsp::PfbChannelizer`(共通、critically-sampled polyphase)。単一トーン分離を参照ベクトルで検証(channel c ↔ c·fs/M)。
  - FFTW プランナは dsp 全体で 1 mutex に統一(spectrum と channelizer が別 mutex だと危険だった)。
  - headless: `spear-std-t98-decode <sigmf base> --freq-err <Hz> [--wav base] [--payloads f] [--log f]`。
    **App の録音(LO = 帯域中心 − 250 kHz)には `--freq-err 250000`** が要る(tool は録音中心を帯域中心と見なす。GUI は `band_center − LO` を自分で渡す)。
- **2(完了)**: AMBE 3600x2450 → PCM 8 kHz。FEC と音声合成は `apps/std_t98/blip25c/`(blip25-vocoder の C 移植、下記「AMBE デコーダ」)。
  当初は mbelib-neo の AMBE 経路を C++ に書き直したもの(`apps/std_t98/ambe/`)を使っていたが、2026-10-16 に置き換えた。
- **1c(完了)**: UI — 30ch 俯瞰スペクトラム + 選択chアイパターン + フレーム表示 + 音声再生。
- **3(完了、2026-09-17)**: 秘話(下記)。

## 秘話(音声スクランブル解除、§2.5)
`../std-t98-tools` の `core/crypto/*`、`core/secret/cracker.py`、`std_t98_multi_audio_service.py` の秘話部分、`std_t98_multi_secret_service.py` を
`apps/std_t98/secret/` に移植した。tools では別プロセス + UDS だったものを **単一プロセス内のワーカースレッド 1 本** にした(§2.5)。

| 部品 | 内容 | 検証 |
|---|---|---|
| `pn.*` | 15 bit LFSR の PN 196 bit(鍵 = 初期状態 1..32767)、ThumbDV 順 / mbelib d 順の keystream | Python `generate_pn_sequence_196` / `descramble_burst` と同値(`test_secret.cpp`) |
| `models.*` | `apps/std_t98/models/` の `ambe2_ffnn.safetensors` と `ambe2_hybrid.onnx` を `.incbin` でバイナリに埋め込む(実行時のパス無し、read-only rootfs 可) | `EmbeddedModelsLoad` |
| `safetensors.*` | safetensors の最小リーダ(メモリ上、F32 のみ、JSON ヘッダは自前の部分パーサ) | ffnn のロード |
| `ffnn.*` | `AMBE2Classifier`(Linear 980→128, ReLU, Linear 128→2)の C++ 推論。入力が 0/1 なので fc1 は「立っているビットの列の総和」 | torch と logits 最大差 9.5e-6、argmax 一致(golden 64 ケース) |
| `hybrid.*` | `AMBE2HybridClassifier`(Conv1d×3 + BN + GELU、BiLSTM×2、attention pooling)を ONNX Runtime(必須依存、`onnxruntime-cpu`)で。モデルはメモリから `Ort::Session` | torch と最大差 3.8e-6(export 時 512 乱数 + golden 64) |
| `silence.*` | 無音の既知ビットから GF(2) の線形計算で鍵候補を復元。全フィールドを検証し、異なる TCH 内位置の一致で採用 | 全 32767 鍵 × 4 位置、競合・破損・ランダム入力のテスト |
| `cracker.*` | 無音による解析 → 今の鍵の検証 → 全チャネル共通キャッシュ(16、上位 2 検証)→ 全鍵探索(hybrid 平文判定 → ffnn 全 32767 鍵 → hybrid 採点 → ブロック 2 で順位) | 解析不成立時は従来の探索。既存 Python 比較は下記 E2E |
| `tracker.*` | チャネルごとの要求ポリシー(5 バーストで最初の要求、5 ごとに直近最大 10 で再確認、同時 1 要求、セッション番号で古い結果を捨てる) | 単体テスト |
| `worker.*` | 専用スレッド。モデルのロードもこのスレッド。要求は同一チャネルの未処理分を置き換え、停止時は探索を打ち切る | 単体テスト、TSan |

**データの流れ**(DSP thread): TCH フレーム → SACCH(CRC OK)の `call_stat == 1` で秘話呼と判定 → `AmbeDecoder::decode_3600(block, pcm, keystream)` が
FEC 後の 49 bit に鍵の PN を XOR してから音声合成(`secret_voice.py` と同じ位置)→ XOR 前の 49 bit × 4 を `Tracker` の窓に積む → 要求が立てば
`Worker::submit`。結果はワーカースレッドから mailbox に入り、DSP thread が次のブロックの先頭で取り込む(`secret_drain_results`)。
鍵が変われば PN を作り直す。結果は Event(`secret chN key K (full search hit, 0.58 s)`)にも残す。

**ビット順の事実**: Python の `THUMBDV_MAP`(keystream の並べ替え表)と C++ デコーダの `kThumbDv`(ThumbDV bit i ↔ mbelib `d[kThumbDv[i]]`)は
同じ表。したがって **mbelib d 順では `d[j] ^= pn[frame*49 + j]` と並べ替えなしで掛かる**。モデル入力は学習時どおり ThumbDV 順のまま。
テスト `DescrambleMatchesPythonInBothBitOrders` が両方の順で同じ結果になることを確認する。

**ソルバー単独評価 (2026-10-21)**: `CrackerConfig::ai_fallback=false` を既定とし、AI による既存鍵・キャッシュの検証と全鍵探索を停止。Worker は AI モデルをロードしない。解析不成立なら `search miss` を返す。再確認は 5 バースト間隔に短縮。すでに復元した鍵による音声復号は従来どおり。AI 経路の回帰テストだけは明示的に有効化する。

**無音からの解析的な鍵復元** (`secret/silence.*`):
- 根拠は [ambe2-c の encoder.md](https://github.com/tallcat4/ambe2-c/blob/main/docs/encoder.md) の AMBE-3000R 既定モードの実測。デジタル無音の `b0=119, b1=16, b2=0, b6=14` は 21 ビットだが、LFSR の鍵に対して独立なのは 14 本。観測された `b5∈{2,8}` に共通する `ambe_d[26]=0` を加えると、全 4 位置で階数 15 になる。
- `pn[t+15]=pn[t] XOR pn[t+1]` の係数行列をコンパイル時に消去し、各フレームでは XOR とパリティで鍵を直接求める。22 本すべての式の整合性と、b0〜b8 すべてが観測範囲内かを検査する。32,767 鍵の列挙もモデル推論も行わない。
- 1 枚の一致は候補にとどめ、TCH 内の異なる位置の 2 枚以上で同じ鍵が得られた場合だけ採用する。別の TCH にまたがってもよい。同じ位置での繰り返しは独立した証拠に数えない。複数の鍵がこの条件を満たせば採用せず既存探索へ戻る。鍵 0 も競合判定に含め、0 だけが確認できた場合は平文として鍵なしを返す。
- 成功時の Event は `analytic silence hit`。既存の鍵キャッシュにも保存する。解析を既存鍵のモデル検証より先に行うため、別の鍵を記憶していても更新できる。
- Tracker は初回 5 TCH (約 0.4 秒)、以後 5 TCH 間隔で直近 10 TCH を再確認する。短い PTT の初回窓に無音が少なくても、次の窓で復元できる。
- この条件は一般の「静かな音」を意味しない。マイク雑音、b0=124,b1=16 以外の背景雑音モード、リセット直後の b0=76、未観測の符号帳値、FEC 後に誤りが残ったフレームなどでは成立しない場合がある。条件を緩めて誤り訂正することはせず、窓内のほかの無音か従来の探索を使う。下記の 3 PTT 録音以外での成立率・誤検出率は未評価。
- `test_silence.cpp` は資料にある実測フレームを既知鍵でスクランブルして全 131,068 組を検査する。全 6 位置ペア、同位置だけの反復、鍵競合 (平文含む)、単一ビット誤り、非対応モード、10,000 個のランダム窓も検査する。乱数試験の成功は実際の音声における誤検出率の保証ではない。

AI 単独探索との実録音ベンチマークは [比較結果](../benchmarks/std_t98_secret_20261021.md) を参照。通常起動はソルバー単独を維持する。

**背景雑音からの解析 (2026-10-21)**:
- 正解鍵 32767 の 3 PTT 実録音は、b0=124,b1=16 の背景雑音フレームが主体。b2 とスペクトルは変わるため、従来の b0=119,b2=0 の条件では全窓 MISS になった。
- b0/b1 の既知 12 bit を異なる TCH 位置の 2 枚から組み合わせ、24 本の線形式を解く。全 6 位置ペアで階数 15 をコンパイル時に検証する。32,767 鍵の列挙・AI 推論・正解鍵の埋め込みは行わない。ソルバー単独では起動時の全鍵 PN 表も作らない。
- ペアの一致だけでは採用しない。3 位置以上で各 2 枚以上、かつ 2 バースト以上の一致を要求する。複数鍵、平文鍵 0、従来の無音解析との競合も検査する。b2〜b8 を特定値に固定しない。
- `RecordedThreePttRecovers32767UsingSolverOnly` は IQ → 受信 DSP → 軟判定 FEC → Tracker → ソルバーを通し、PTT ごとに鍵・キャッシュを消して 3 回とも 32767 を復元することを検査する。録音はリポジトリ外。`SPEAR_STD_T98_SECRET_IQ` で検証用 SigMF base を指定でき、無ければ skip。
- `BackgroundRecoversAllKeysWithoutGainOrSpectrumAssumptions` は実測背景雑音フレームから鍵 0〜32767 をすべて検査。背景雑音の証拠不足・競合、既存の乱数 10,000 窓も検査する。
- 診断用 CLI の `--raw2450 <path>` でチャネル・シンボル番号・FEC 後の秘話解除前フレームを保存できる。

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

## スケルチ — 床相対・出力ゲート(2026-09-21、実地での不安定さから。2026-10-16 に廃止、上記「チャネルの活動」)
2026-09-20 のアマチュアイベント(`~/spear/recordings/rec_20260920_014553` / `rec_20260920_021454`、ch23 の交信)で「遠い局が不規則に途切れる、
空きチャネルが開く」症状。録音を解析した結果:
- **AGC が全チャネルの床を一斉に揺らしていた**。会場に ADC フルスケールに達する広帯域のバースト雑音(サンプルの 1.5 % がクリップ)があり、
  AGC(UHD 既定の AD9361 slow AGC)が ~0.3 ms ごとにゲインを階段状に変える。30 ch のブロック電力の相関 0.95、中央値 −41 dB、ブロック間で
  3〜4 dB(p90 で 7〜10 dB)動く。自宅(gain 30 固定)では床 −85 dB、±2 dB。
- 旧スケルチ(絶対 −40 dB)はその床の真上: 空きチャネルが 16 ms ブロックの 40 % で開き毎秒 27 回開閉、弱い送信(床 + 3〜10 dB)は
  1 回の送信で 10〜140 回開閉してフレームの 30〜75 % を落とした。閉で IQ を 0 にしていたので、開くたびにシンボル同期の再引き込み。
- シンボル同期そのものは正常: Gardner ループを Python で再現し総当たりの最良位相と比べると、強・中信号で 0.1〜0.3 s で収束し判定点の質は
  最良位相と同等。弱信号(SNR ≈ 10 dB)で ±6 sample ふらつくのは SNR の限界(多値用 TED 修正・loop_bw 0.03 でも改善なし)。
  アイが「ふらふら」見えたのはスケルチの開閉のたびにアイがリセットされ引き込み過渡を見せていたため。

対策(`ReceiverConfig`):
- **閾値 = そのブロックの床 + margin**(既定 8 dB、`std_t98.squelch_margin_db` / GUI の SQUELCH)。床 = 全チャネルのブロック電力の中央値
  (全チャネル共通。半数以上が同時送信なら床を過大に見るが、実用上起きない)。ヒステリシス 3 dB、閉じるのは 120 ms 下回り続けたとき。
- **DSP は止めない**。閉でも復調・シンボル同期・同期語検出は回し(CPU は以前と同じ: 補間は元から全 ch で回っていた)、出すものだけをゲートする:
  フレームは開いているとき、または **CRC が通ったとき**(床 + margin に届かない遠い局の確かな証拠。閉のまま CRC 不良は捨てる = 雑音の偽同期を
  音にしない)。CRC OK で開き、hold を延ばす。アイは開いているときだけ出す(履歴は途切れさせない)。
  → 2026-10-16 にフレームのゲートは同期の状態機械に移し(上記「フレーム同期」)、squelch そのものも廃止した(上記「チャネルの活動」)。
- 実録音での前後: 空きチャネルの開放 40 % → 0.1〜0.7 %、開閉 27/s → <1/s、偽フレーム 0。ch23(強)181/89 vs 177/87(フレーム / SACCH OK)、
  ch24(弱)78/48 vs 94/40 — CRC 不良の雑音フレームが減り、CRC OK が増えた。
- 会場のような環境では **AGC を切って手動ゲイン**にすれば床の揺れ自体が消える(録音からは検証できないので次の実地で)。
- 平均周期がクランプ端(26.04 ± 0.02)に張り付く傾向は残っている(復号には影響しない)。

## フレーム同期 — 同期の状態機械とフライホイール(2026-10-16)
スケルチを床相対にしたあとも、SINR が十分高い近い局以外は途切れ途切れだった。原因は 2 つの仕事の重なり:
フレームを出すかどうかを squelch(電力)と同期語の両方で決めていたこと、そして同期語を毎フレーム探し直していたこと
(192 シンボルを集めると探索に戻り、次の同期語が崩れていればそのフレームを落とす)。JR TRAIN RADIO の `Synchronizer`
(捕捉 / 喪失のヒステリシス)を参考に、`SyncCorrelator`(`dsp/sync_correlator.*`)を同期の状態機械にした:
- **SEARCH**: 従来どおり 1 シンボルずつ厳しい閾値(`sync_ratio` 0.2)で同期語を探す。同期語と CRC が揃えば即 LOCKED。
- **TENTATIVE**: CRC 不良の候補 1 つを保留し、次の予測位置(+192 ±1)で同期語(厳しい閾値)が取れれば LOCKED(保留も出す)。
  取れなければ保留を捨て、保留した同期語の次のシンボルから探し直す(偽同期の陰にある本物の先頭を逃さない)。
  **CRC だけでは確認しない**: SACCH の CRC は 6 bit で、雑音の偽同期の次の位置で 1/64 で通る(9/20 の録音の空きチャネルで 3 回起きた)。
- **LOCKED**: 予測位置 ±1 シンボル(symbol sync の 1 シンボルの滑り)だけを緩い閾値(`sync_locked_ratio` 0.6)で見る。同期語が崩れていても
  予測位置でフレームを作り、本物かどうかは CRC と後続のフレームに任せる(フライホイール)。同期語か CRC が取れたフレームは「良」、
  良でないフレームは保留して次の良で出す。`sync_max_misses`(3 = 240 ms)回続けて良でなければ同期喪失 — 保留は捨て、
  最初の保留の位置から探し直す(送信の終わりの雑音を音にしない、すぐ後の次の呼を逃さない)。

これに合わせて役割を分けた(状態の持ち主は 1 つ):
- **フレーム・音声・呼の区切りは同期だけが決める**。秘話の呼の終わり(`tracker.on_clear`)も同期の喪失で判定する。
- 保留で遅れるのは最大 3 フレーム(240 ms)で、ミキサの遅延(300 ms)に収まる。

## チャネルの活動 — squelch を廃止(2026-10-16)
フレームのゲートを同期に移したあとも、squelch(床 + margin の電力判定)は CARRIER の表示・電力のバーと数値・アイの出力に残っていた。
音に効かない調整項目(SQUELCH)が画面にあり、電力が AGC で揺れて閾値の近くでチャネルの表示がちらつくので、**squelch を丸ごと削除**した
(`ReceiverConfig` の squelch_* と床の計算、App の `squelchMarginDb` と保存、SQUELCH キー、tool の `--squelch-margin`)。
- **活動 = フレーム同期中**(`ChannelMetrics::active`)。同期を失っても `active_hold_s`(1 s)は活動中とする(JR TRAIN RADIO の「直近 1 秒に同期」と同じ)。
  CRC による捕捉も LOCKED として入るので、活動は同期と CRC だけで決まる。電力は判定に使わない。
- 画面: チャネル格子は活動中だけ緑の帯と SYNC、状態表示は SYNC / IDLE。電力のバー・閾値の目印・FLOOR / SQL は削除し、
  選択チャネルの POWER は数値だけ(常に表示)。FLOOR / SQL の欄は同期の統計 LOCK / FLY / DROP にした。
- アイパターンと周波数誤差の推定は活動中だけ。
- 9/20 の録音(132 s): 空きチャネルが活動中になったことは 0 回。ch23 / ch24 の活動の切り替わりは 13 / 12 回(送信の回数とほぼ同じ)。
  以前の電力 squelch は空きチャネルで毎秒 27 回開閉していた。
- テスト(`StdT98Activity`): ゲインが 20 ms ごとに ±10 dB 動く雑音と、床 + 20 dB の無変調の搬送波で、どのチャネルも活動しない(フレーム・アイも出ない)。

以下の「スケルチ」は経緯として残す。

9/20 の会場録音(`--freq-err 250000 --no-secret`、フレーム / SACCH CRC OK):

| 録音 | ch | 以前 | 状態機械 |
|---|---|---|---|
| `rec_20260920_014553`(132 s) | 23(強) | 181 / 89 | 279 / 113 |
| | 24(弱) | 78 / 48 | 192 / 72 |
| `rec_20260920_021454`(437 s) | 23(強) | 1396 / 1118 | 1548 / 1190 |
| | 24(弱) | 26 / 15 | 43 / 16 |

空きチャネルの偽フレームは 0。フライホイールで出したフレームの 3〜4 割は CRC が通り、残りもほとんど RICH のパリティが通る
(本物のフレームの誤り)。

## AMBE デコーダ — blip25c(2026-10-16)
音声の FEC と合成は blip25c(`apps/std_t98/blip25c/`)。blip25-vocoder(Rust、MIT)の AMBE+2 半レート・デコーダの C99 移植で、
Rust 版とビット一致する。AMBE-3000R 自身の復号音との差(mel スペクトル距離)は 0.77 dB で、以前使っていた mbelib は 5.7 dB。
整数演算のみ、1 デコーダの状態は約 2.5 KB。ビット一致を保つため手を加えない(警告もこのターゲットだけ消している)。
- STD-T98 の TCH の 9 byte は AMBE-3000R の rate 33(P25 Phase 2 Annex S のインターリーブ)と同じ並び。以前の pyambelib の
  rW/rX/rY/rZ でほどいた 49 bit と、blip25c の `blip25_fec_3600` の 49 bit は、golden の 48 ブロックと 9/20 の録音の 740 ブロックすべてで一致した。
- 訂正数の数え方は違う: 以前は C0 の 12 データビットの訂正だけを数えていた。blip25c は C0 を拡張 Golay [24,12] として 24 ビットで数え、
  4 ビット誤り(検出のみ)を消失として前フレームの繰り返しとフェードにする。STD-T98 の 24 ビット目は本当に Golay のパリティ
  (C1 に誤りのないフレームでは C0 = 0 が大半)。訂正数は「誤りの程度」として合成に渡す(`blip25_decode_3600` と同じ)。
- 9/20 の会場録音では、CRC が通った SACCH のフレームでも C1 が訂正の上限(3)のものが 1/3 あり、C0 の訂正不能が 1 割ある
  (自宅の強い信号では全ブロック誤り 0)。会場の雑音・AGC による実際のビット誤りで、軟判定の余地が大きい。
- **軟判定**(2026-10-16): 受信機は TCH の量子化前のシンボルから 4 ブロック × 72 の LLR を作り(`traffic_llr`)、`blip25_fec_3600_soft`
  (Golay を Chase-II で軟判定復号、blip25-vocoder の `decode_frame_soft` と同じ)で 49 bit にする。ビット対応(+1→00 +3→01 −1→10 −3→11)から
  上位ビット = −y、下位ビット = |y| − 2(× 32、±127 で飽和)。符号は 9 byte の硬判定と一致する(0 は情報なし)。
  合成は blip25-vocoder の `decode_soft` と同じく誤りなしとして行うが、最良の候補でも C0 が訂正不能なフレームだけは消失にする
  (硬判定の経路と同じ扱い。`decode_soft` は常に誤りなし)。9/20 の録音の 740 ブロックで、硬判定と 49 bit が違うのは 274、C0 の訂正不能は 79 → 63。
  C0 はほぼ上位ビット(シンボルの符号)に、C1 はほぼ下位ビット(内側 / 外側のレベル)に載るので、会場の録音で C1 の誤りが多いのは
  レベルの判定の誤り(|y| は内側 1.0、外側 3.4 ± 0.9、1.5〜2.5 に 18 %)。レベルの追従(閾値 2 の適応)は次の改善候補。
  App も tool も軟判定で復号する。tool の `--hard` は比較用に 9 byte の硬判定で、`--llr base` はチャネルごとの LLR 列(golden 用)を書く。
- 秘話: keystream は FEC 後の 49 bit(ambe_d 順)に XOR してから `blip25_decode_bits` に渡す(以前と同じ位置)。鍵探索の窓も同じ 49 bit(軟判定の FEC 後)。
- PCM は int16 の全幅(mbelib の float 経路は 1/7 だった)。App のミキサの係数は 1/32768。
- 検証(`tests/test_ambe.cpp`): 9/20 の録音の 740 ブロック(誤り 550、消失 79 を含む)を blip25c の `tools/golden`(blip25-vocoder 1.0)で
  復号した golden(`~/spear/golden/std_t98/ambe_blip25_golden.txt`、音声を含むのでリポジトリ外)と、49 bit・訂正数・PCM がすべて一致。
  軟判定は、同じ録音の LLR を Rust 版の `fecsoft` と `dec2450`(C0 訂正不能なら消失)で復号した golden(`~/spear/golden/std_t98/ambe_soft/`)と一致。

## 実機で見つかった移植バグ
- **symbol sync の max_deviation の単位**(2026-09-17、実機で発覚): GR `symbol_sync_ff` の `max_deviation` は「平均クロック周期の
  公称値からの絶対偏差 [samples/symbol]」(0.02 → 26.04 ± 0.02)。移植では比率と解釈して ±0.52 sample を許していたため、
  雑音区間で平均周期が誤った値に流れ着き、そこから抜けられずアイが開かない(運よく一度ロックすれば動く)挙動になった。
  録音再生では偶然ロックしていたので golden test は通っていた → **golden test は「動く」ことしか保証しない。パラメータの単位は
  一次資料(GR ソース)と突き合わせること**。修正後、平均周期の上下限テストを追加。

## ライセンス
`apps/std_t98/blip25c/` は blip25-vocoder(MIT, Copyright (c) Chance Lindsey)の派生物で MIT(`blip25c/LICENSE`)。GPL-3.0 の本体と結合してよい。
AMBE+2 には特許がある(US 8,359,197 ほか。blip25-vocoder の PATENT_NOTICE.md)。
以前の `apps/std_t98/ambe/`(mbelib / mbelib-neo 由来、GPL-2.0-or-later)は 2026-10-16 に削除した。残っている `ambe/bits.hpp` は 49 bit の並びの表だけ。

## プラットフォーム検証メモ
- 純 C++ プロトコルデコーダは Qt/SDR 非依存ライブラリ(`spear_std_t98_protocol`)として App から独立にテストできた。
  golden vector を外部リポジトリの Python から生成する方式は、移植の等価性検証に有効(他の復号系 App でも使える)。
- 受信機の検証は「合成信号」より「実録音の抜粋を golden にする」ほうが正しかった。合成変調器は規格の送信整形が不明なまま
  パラメータ合わせになり(RRC/RC/NRZ どれも実信号の統計に合わない)、無駄だった。録音 → 抜粋 → golden の流れを
  `RecordingSource` + `spear-std-t98-decode --payloads` で回せる。ADS-B でも同じ方針にする。
- 30ch 受信機は 4 Msps 入力で実時間の約 1 倍(1.2 s を 1.2 s で処理、-O2 単スレッド)。GUI ではワーカースレッド 1 本で足りるが
  余裕は小さい。FirInterpolator ×10 を 30 本回す部分が支配的 → 必要なら PFB 出力 6.25 kHz のまま demod する案(sps≈2.6 は
  Gardner には厳しい)か、×4 に落とす案。
- `Provenance` の連鎖(resamp1 → PFB → 補間 → シンボル判定点)でフレーム先頭を radio.rx sample index に戻せた(§3.6)。
  PFB の provenance は「M 入力 → 1 出力」の整数比で表せるので StageInfo で十分だった。
- (mbelib のころ)AMBE の float 経路は ±4447 でソフトクリップされる(mbelib の short 経路は ×7)。blip25c は int16 の全幅で出す。
