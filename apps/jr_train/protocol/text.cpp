// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 文字列(JIS X 0208 の区点、通告文、旅客案内の日英テキスト)。原本: crates/receiver-core/src/data/text.rs
#include "data.hpp"

#include <cstdio>

namespace spear::jr_train::protocol {

extern const uint16_t kJis0208[94 * 94];   // jis0208_table.cpp(refgen が encoding_rs から生成)

namespace {

void utf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xc0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    }
}

const std::string kIdeographicSpace = "\xe3\x80\x80";   // U+3000

struct Unit {
    std::string s;
    uint8_t category;   // 0: 日本語、1: 英語、2: 区切り
    std::size_t consumed;
};

Unit unit(const uint8_t* bytes, std::size_t n) {
    if (n == 0) throw DataError("Truncated text unit");
    const uint8_t a = bytes[0];
    if (n > 1) {
        const uint8_t b = bytes[1];
        if (a == 0x2a && b == 0x6b) return {"", 2, 2};
        if (a == 0xa9 || a == 0xaa) {
            if (!(b >= 0xa1 && b <= 0xfe)) throw DataError("Invalid English cell");
            std::string s;
            if (a == 0xa9) {
                s += static_cast<char>(b & 127);
            } else {
                switch (b) {
                case 0xe0: s = " "; break;
                case 0xe5: utf8(s, 0x14c); break;   // Ō
                case 0xe8: utf8(s, 0x16b); break;   // ū
                case 0xea: utf8(s, 0x14d); break;   // ō
                default: throw DataError("Unsupported English cell");
                }
            }
            return {s, 1, 2};
        }
        if (a == 0x8e && b >= 0xa1 && b <= 0xdf) {
            std::string s;
            utf8(s, 0xff61 + b - 0xa1u);   // 半角カナ
            return {s, 0, 2};
        }
        if (a >= 0xa1 && a <= 0xfe && b >= 0xa1 && b <= 0xfe) return {jis(a & 0x7f, b & 0x7f), 0, 2};
    }
    if (a == 9 || a == 10 || a == 13 || (a >= 0x20 && a <= 0x7e)) return {std::string(1, static_cast<char>(a)), 0, 1};
    throw DataError("Unsupported text byte");
}

} // namespace

std::string jis(uint8_t row, uint8_t cell) {
    if (row == 0x2d || (row >= 0x79 && row <= 0x7c)) throw DataError("Unsupported JIS extension");
    // 原本の特例(encoding_rs の WHATWG 対応とは別の文字にする)
    uint32_t special = 0;
    if (row == 0x21 && cell == 0x41) special = 0x301c;        // 〜
    else if (row == 0x21 && cell == 0x42) special = 0x2016;   // ‖
    else if (row == 0x21 && cell == 0x5d) special = 0x2212;   // −
    else if (row == 0x21 && cell == 0x71) special = 0xa2;     // ¢
    else if (row == 0x21 && cell == 0x72) special = 0xa3;     // £
    else if (row == 0x22 && cell == 0x4c) special = 0xac;     // ¬
    std::string s;
    if (special) {
        utf8(s, special);
        return s;
    }
    if (row < 0x21 || row > 0x7e || cell < 0x21 || cell > 0x7e) throw DataError("Invalid JIS cell");
    const uint16_t c = kJis0208[(row - 0x21) * 94 + (cell - 0x21)];
    if (c == 0) throw DataError("Invalid JIS cell");
    utf8(s, c);
    return s;
}

std::string notice_text(const uint8_t* bytes, std::size_t n, bool trim) {
    std::string out;
    std::size_t pos = 0;
    while (pos < n) {
        const uint8_t a = bytes[pos];
        if (pos + 1 < n) {
            const uint8_t b = bytes[pos + 1];
            const bool a_jis = a >= 0x21 && a <= 0x7e;
            const bool b_high = b >= 0xa1 && b <= 0xfe;
            const bool b_low = b >= 0x21 && b <= 0x7e;
            if (a_jis && (b_high || (b_low && (a >= 0x30 || (a == 0x21 && b == 0x21))))) {
                try {
                    out += jis(a, b & 0x7f);
                    pos += 2;
                    continue;
                } catch (const DataError&) {
                }
            }
            if (a == 13 && b == 10) {
                out += '\n';
                pos += 2;
                continue;
            }
        }
        if (a == 0) {
            utf8(out, 0xb7);   // ·
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "<%02X>", a);
            out += buf;
        }
        ++pos;
    }
    if (trim) {
        while (out.size() >= 3 && out.compare(0, 3, kIdeographicSpace) == 0) out.erase(0, 3);
        while (out.size() >= 3 && out.compare(out.size() - 3, 3, kIdeographicSpace) == 0) out.erase(out.size() - 3);
    }
    return out;
}

std::vector<Field> service_text(const uint8_t* bytes, std::size_t n) {
    std::vector<Field> fields;
    std::size_t pos = 5;
    for (const char* name : {"Line", "From", "To", "Direction", "Cause", "ServiceStatus", "Description"}) {
        if (pos >= n) throw DataError("Missing service field");
        const std::size_t count = bytes[pos];
        ++pos;
        std::string ja;
        std::optional<std::string> en;
        for (std::size_t i = 0; i < count; ++i) {
            if (pos > n) throw DataError("Truncated service text");
            const Unit u = unit(bytes + pos, n - pos);
            pos += u.consumed;
            if (u.category == 2) {
                if (en || i + 1 == count) throw DataError("Invalid bilingual separator");
                en = std::string();
            } else if (u.category == 1) {
                if (!en) throw DataError("English without separator");
                *en += u.s;
            } else {
                if (en) throw DataError("Invalid bilingual order");
                ja += u.s;
            }
        }
        fields.push_back({name, ja, en});
    }
    if (pos != n) throw DataError("Trailing service text");
    return fields;
}

} // namespace spear::jr_train::protocol
