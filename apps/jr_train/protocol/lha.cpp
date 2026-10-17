// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// LHA level-1 の -lh5- 展開(メモリ上、上限つき)。原本: crates/receiver-core/src/data/lha.rs
// (原本には「reference のメモリ上の読み込みから移植」とある。出典の記載は無く、原本と同じく MIT として扱う: docs/apps/jr_train.md「ライセンス」)
#include "data.hpp"

#include <limits>
#include <unordered_map>

namespace spear::jr_train::protocol {
namespace {

struct Bits {
    const uint8_t* bytes;
    std::size_t n;
    std::size_t pos = 0;
    std::size_t read(std::size_t count) {
        if (count > 16 || pos + count > n * 8) throw DataError("Truncated LH5 stream");
        std::size_t v = 0;
        for (std::size_t i = 0; i < count; ++i) {
            v = (v << 1) | ((bytes[pos / 8] >> (7 - pos % 8)) & 1);
            ++pos;
        }
        return v;
    }
};

struct Tree {
    std::optional<std::size_t> constant;
    std::unordered_map<std::size_t, std::size_t> symbols;   // (1 << 長さ) | 符号 → 記号

    static Tree make_constant(std::size_t s, std::size_t alphabet) {
        if (s >= alphabet) throw DataError("Invalid constant Huffman symbol");
        Tree t;
        t.constant = s;
        return t;
    }
    static Tree make(const std::vector<std::size_t>& lengths) {
        std::size_t counts[17] = {}, next[17] = {};
        for (std::size_t l : lengths) {
            if (l > 16) throw DataError("Invalid Huffman length");
            if (l != 0) ++counts[l];
        }
        std::size_t code = 0;
        for (std::size_t l = 1; l <= 16; ++l) {
            code = (code + counts[l - 1]) << 1;
            if (code + counts[l] > (std::size_t{1} << l)) throw DataError("Oversubscribed Huffman tree");
            next[l] = code;
        }
        if (code + counts[16] != 65536) throw DataError("Incomplete Huffman tree");
        Tree t;
        for (std::size_t s = 0; s < lengths.size(); ++s) {
            const std::size_t l = lengths[s];
            if (l != 0) {
                t.symbols.emplace((std::size_t{1} << l) | next[l], s);
                ++next[l];
            }
        }
        return t;
    }
    std::size_t decode(Bits& b) const {
        if (constant) return *constant;
        std::size_t code = 1;
        for (int i = 0; i < 16; ++i) {
            code = (code << 1) | b.read(1);
            if (auto it = symbols.find(code); it != symbols.end()) return it->second;
        }
        throw DataError("Invalid Huffman symbol");
    }
};

Tree lengths(Bits& b, std::size_t alphabet, std::size_t count_bits, std::size_t skip) {
    const std::size_t count = b.read(count_bits);
    if (count == 0) return Tree::make_constant(b.read(count_bits), alphabet);
    if (count > alphabet) throw DataError("Invalid Huffman length count");
    std::vector<std::size_t> ls(alphabet, 0);
    std::size_t i = 0;
    while (i < count) {
        std::size_t l = b.read(3);
        if (l == 7) {
            while (b.read(1) != 0) {
                ++l;
                if (l > 16) throw DataError("Huffman code too long");
            }
        }
        ls[i] = l;
        ++i;
        if (i == skip) {
            i += b.read(2);
            if (i > count) throw DataError("Invalid length run");
        }
    }
    return Tree::make(ls);
}

Tree literals(Bits& b, const Tree& tree) {
    const std::size_t count = b.read(9);
    if (count == 0) return Tree::make_constant(b.read(9), 510);
    if (count > 510) throw DataError("Invalid literal count");
    std::vector<std::size_t> ls(510, 0);
    std::size_t i = 0;
    while (i < count) {
        const std::size_t symbol = tree.decode(b);
        if (symbol > 2) {
            ls[i] = symbol - 2;
            ++i;
        } else {
            i += symbol == 0 ? 1 : (symbol == 1 ? b.read(4) + 3 : b.read(9) + 20);
            if (i > count) throw DataError("Invalid literal run");
        }
    }
    return Tree::make(ls);
}

std::size_t le16(const uint8_t* b) { return static_cast<std::size_t>(b[0] | (b[1] << 8)); }
std::size_t le32(const uint8_t* b) { return static_cast<std::size_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24)); }

} // namespace

std::vector<uint8_t> lha_expand(const uint8_t* a, std::size_t n) {
    if (n < 27 || std::string_view(reinterpret_cast<const char*>(a + 2), 5) != "-lh5-" || a[20] != 1) throw DataError("Unsupported archive");
    std::size_t end = static_cast<std::size_t>(a[0]) + 2;
    const std::size_t name = a[21];
    if (end > n || end < 27 + name) throw DataError("Truncated archive header");
    uint8_t checksum = 0;
    for (std::size_t i = 2; i < end; ++i) checksum = static_cast<uint8_t>(checksum + a[i]);
    if (checksum != a[1]) throw DataError("Archive header checksum mismatch");
    std::size_t packed = le32(a + 7);
    const std::size_t size = le32(a + 11);
    if (size == 0 || size > 65536 || packed > 65536) throw DataError("Archive size limit");
    const std::size_t expected = le16(a + 22 + name);
    std::size_t ext = le16(a + end - 2);
    std::size_t start = end;
    while (ext != 0) {
        if (ext < 3 || ext > packed || start + ext > n) throw DataError("Invalid archive extension");
        packed -= ext;
        start += ext;
        ext = le16(a + start - 2);
    }
    end = start + packed;
    if (end > n || n > end + 1 || (n == end + 1 && a[end] != 0)) throw DataError("Invalid archive length");
    Bits bits{a + start, packed};
    std::vector<uint8_t> output;
    output.reserve(size);
    while (output.size() < size) {
        const std::size_t symbols = bits.read(16);
        if (symbols == 0) throw DataError("Empty compressed block");
        const Tree codes = lengths(bits, 19, 5, 3);
        const Tree literal = literals(bits, codes);
        const Tree distance = lengths(bits, 14, 4, std::numeric_limits<std::size_t>::max());
        for (std::size_t k = 0; k < symbols; ++k) {
            if (output.size() == size) throw DataError("Compressed block exceeds size");
            const std::size_t s = literal.decode(bits);
            if (s < 256) {
                output.push_back(static_cast<uint8_t>(s));
                continue;
            }
            const std::size_t count = s - 256 + 3;
            const std::size_t db = distance.decode(bits);
            const std::size_t d = db == 0 ? 0 : (std::size_t{1} << (db - 1)) + bits.read(db - 1);
            if (count > size - output.size()) throw DataError("Compressed match exceeds size");
            for (std::size_t j = 0; j < count; ++j) output.push_back(output.size() <= d ? uint8_t{0x20} : output[output.size() - d - 1]);
        }
    }
    uint16_t crc = 0;
    for (uint8_t v : output) {
        crc ^= v;
        for (int k = 0; k < 8; ++k) crc = static_cast<uint16_t>((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
    }
    if (crc != expected) throw DataError("Expanded archive CRC mismatch");
    return output;
}

} // namespace spear::jr_train::protocol
