# mrl-celp-c(取り込み)

https://github.com/tallcat4/mrl-celp-c の commit `e9fbb4e`(2026-11-03)の `decoder/` から、JR TRAIN の音声に使うファイルだけを変更せずに置いたもの。

- `include/mrlcelp.h`、`src/{bits,decoder,dsp,enhance,mpostfilter,tables}.c` と `src/{core,fixed,mpostfilter,tables}.h`
- 使っていないもの: `src/widen.c`(帯域拡張。宣言は `mrlcelp.h` に残るが、ビルドしない)、`fec/`(spear は `protocol/voice_frame` を持つ)、エンコーダ
- ライセンス: `LICENSE`(MIT。jr-conventional-line-radio-receiver と mcelp-codec の派生物)

更新するときは、同じファイルを新しい commit のものに置き換えて、この commit の記載を変える。
