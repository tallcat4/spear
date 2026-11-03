// TOKUDER デコーダーの重み(models/tokuder48_causal_v1.bin)をバイナリに埋め込む(docs/DEVELOPMENT.md: 実行時にパスを持たない)。
// SPEAR_STD_T99_MODELS_DIR は CMake が apps/std_t99/models の絶対パスで定義する。GNU as の .incbin(GCC / Clang、Linux)。
// 書き方は apps/std_t98/secret/models.cpp と同じ。読むのは voice/tokuder.cpp の model()。
#include <cstdint>

#define SPEAR_STR2(x) #x
#define SPEAR_STR(x) SPEAR_STR2(x)

asm(".section .rodata\n"
    ".balign 64\n"
    ".global spear_std_t99_tokuder_data\n"
    "spear_std_t99_tokuder_data:\n"
    ".incbin \"" SPEAR_STR(SPEAR_STD_T99_MODELS_DIR) "/tokuder48_causal_v1.bin\"\n"
    ".global spear_std_t99_tokuder_end\n"
    "spear_std_t99_tokuder_end:\n"
    ".byte 0\n"
    ".previous\n");
