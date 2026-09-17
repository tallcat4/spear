// S.P.E.A.R. core — TAP (要件 §5.3)
//
// DSP 中間点の開発用出力。registry も dynamic tap も使わない。
//   TAP("std_t98.discriminator", buf, n);
// Development build (SPEAR_TAP_ENABLED): $SPEAR_TAP_DIR (default ./tap) 配下へ raw で追記。
// Release build: 完全に消滅(コード生成なし)。
// 解析は本体 GUI とは独立したツール(numpy 等)で行う。ファイル名に dtype を含める。
#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <type_traits>

namespace spear::tap {

class Channel {
public:
    explicit Channel(std::string id, DataType dtype);
    ~Channel();
    void write(const void* data, std::size_t bytes);
    static void set_directory(std::string dir);   // 起動時のみ
    static bool enabled();
private:
    std::string id_;
    DataType dtype_;
    std::mutex mu_;
    std::FILE* fp_ = nullptr;
    bool tried_ = false;
};

template <class T>
inline void write(Channel& ch, const T* data, std::size_t n) {
    ch.write(static_cast<const void*>(data), n * sizeof(T));
}

} // namespace spear::tap

#if defined(SPEAR_TAP_ENABLED) && SPEAR_TAP_ENABLED
#define TAP(id, ptr, n)                                                                  \
    do {                                                                                 \
        using spear_tap_elem_t = std::remove_cv_t<std::remove_pointer_t<std::decay_t<decltype(ptr)>>>; \
        static ::spear::tap::Channel spear_tap_ch_{(id), ::spear::dtype_of<spear_tap_elem_t>()}; \
        ::spear::tap::write(spear_tap_ch_, (ptr), (n));                                  \
    } while (0)
#else
#define TAP(id, ptr, n) ((void)0)
#endif
