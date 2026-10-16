# blip25c(取り込み)

AMBE+2 半レート(3600x2450 / 2450)の FEC と音声合成。blip25-vocoder(https://github.com/OpenBLIP25/blip25-vocoder、Rust、MIT)の
デコーダを C99 にビット一致で移植したもの。取り込み元は作者による blip25c(2026-10-16 版。軟判定の入口 `blip25_decode_3600_soft` / `blip25_fec_3600_soft` を含む)。

- 取り込んだもの: `include/blip25.h`、`src/`(`mbelib_compat.c` を除く)、`LICENSE`。テスト・ツール・mbelib 互換 API(`include/mbelib.h`)は取り込んでいない。
- **ビット一致を保つため手を加えない**。更新するときは取り込み元から丸ごと差し替え、`apps/std_t98/tests/test_ambe.cpp` の golden(Rust 版の出力)で確認する。
- ビルドは `apps/std_t98/CMakeLists.txt` の `spear_blip25c`(C99、警告なし)。
- 使い方は `apps/std_t98/ambe.cpp`。背景と検証は `docs/apps/std_t98.md`「AMBE デコーダ」。
- AMBE+2 には特許がある(US 8,359,197 ほか)。blip25-vocoder の PATENT_NOTICE.md を参照。
