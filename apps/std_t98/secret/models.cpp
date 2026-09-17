#include "models.hpp"

// SPEAR_STD_T98_MODELS_DIR は CMake が apps/std_t98/models の絶対パスで定義する。GNU as の .incbin(GCC / Clang、Linux)
#define SPEAR_STR2(x) #x
#define SPEAR_STR(x) SPEAR_STR2(x)
#define SPEAR_INCBIN(sym, file)                                                         \
    asm(".section .rodata\n"                                                            \
        ".balign 64\n"                                                                  \
        ".global " #sym "_data\n" #sym "_data:\n"                                       \
        ".incbin \"" SPEAR_STR(SPEAR_STD_T98_MODELS_DIR) "/" file "\"\n"                \
        ".global " #sym "_end\n" #sym "_end:\n"                                         \
        ".byte 0\n"                                                                     \
        ".previous\n");                                                                 \
    extern "C" const uint8_t sym##_data[], sym##_end[];

SPEAR_INCBIN(spear_std_t98_ffnn, "ambe2_ffnn.safetensors")
SPEAR_INCBIN(spear_std_t98_hybrid, "ambe2_hybrid.onnx")

namespace spear::std_t98::secret {

std::span<const uint8_t> embedded_ffnn_safetensors() { return {spear_std_t98_ffnn_data, static_cast<std::size_t>(spear_std_t98_ffnn_end - spear_std_t98_ffnn_data)}; }
std::span<const uint8_t> embedded_hybrid_onnx() { return {spear_std_t98_hybrid_data, static_cast<std::size_t>(spear_std_t98_hybrid_end - spear_std_t98_hybrid_data)}; }

} // namespace spear::std_t98::secret
