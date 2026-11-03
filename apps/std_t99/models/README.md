# TOKUDER デコーダーの重み

[tokuder-neurodec](https://github.com/tallcat4/tokuder-neurodec)(作者本人、GPL-3.0-or-later)の `tokuder48_causal_v1`(約 440 万パラメータ)。
std-t99-receiver-sharp の `models/` と同じファイルで、バイナリに埋め込まれる(`voice/model_data.cpp` の `.incbin`)。実行時のパスは無い。

| ファイル | 内容 |
|---|---|
| `tokuder48_causal_v1.bin` | `tokuder48_causal_v1.pt`(SHA256 `4e869d1b52527bcbf9aec92c814263f4f41ab4a9afb0744223de1231ee5f1199`)を std-t99-receiver-sharp の `tools/export-model.py` で並べ直しただけの FP32(SHA256 `7401807cb390c202a0ffa44411e953b885dba702e0847471a018ed37bd2aa13b`)。量子化・再学習はしていない |
| `tokuder48_causal_v1.json` | 構成と学習の記録(原本のまま) |

入力は TCH の 72 bit を軟判定で誤り訂正した 48 bit(`voice/tokuder.cpp` の `soft_decode`、tokuder_neurodec `fec.py` と同じ)、出力は 8 kHz の音声(20 ms / フレーム)。
推論は C++(`voice/tokuder.cpp`、std-t99-receiver-sharp の `native/tokuder.cpp` を名前空間だけ変えたもの)。PyTorch との一致は原本で確認済み
(FEC は完全一致、フロントエンドの差 2e-5 以下、同じ励振雑音で波形の SNR 40 dB 超。std-t99-receiver-sharp `tools/check-native-model.py`)。

学習データは VCTK(CC BY 4.0)、JVS(個人・非商用の研究用途のみ、再配布不可)、JSUT(配布元の条件)。重みとデータの扱いは tokuder-neurodec の README を参照。
TOKUDER はアイコム株式会社の登録商標。この App はアイコム株式会社とは関係が無く、名称は ARIB STD-T99 の音声フレームとの互換性を示すためだけに使う。
