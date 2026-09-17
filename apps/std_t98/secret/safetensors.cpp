#include "safetensors.hpp"

#include <cctype>
#include <cstring>

namespace spear::std_t98::secret {
namespace {

// ヘッダに現れる JSON の部分集合(オブジェクト / 配列 / 文字列 / 整数)だけを読む再帰下降パーサ。
struct Json {
    enum class Kind { Null, Str, Int, Arr, Obj } kind = Kind::Null;
    std::string s;
    int64_t i = 0;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
};

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}
    bool parse(Json& out) { ws(); return value(out); }
private:
    const std::string& t_;
    std::size_t p_ = 0;
    void ws() { while (p_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[p_]))) ++p_; }
    bool value(Json& out) {
        if (p_ >= t_.size()) return false;
        const char c = t_[p_];
        if (c == '{') return object(out);
        if (c == '[') return array(out);
        if (c == '"') { out.kind = Json::Kind::Str; return string(out.s); }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            out.kind = Json::Kind::Int;
            std::size_t q = p_ + 1;
            while (q < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[q])) || t_[q] == '.' || t_[q] == 'e' || t_[q] == 'E' || t_[q] == '+' || t_[q] == '-')) ++q;
            out.i = static_cast<int64_t>(std::stod(t_.substr(p_, q - p_)));
            p_ = q;
            return true;
        }
        for (const char* lit : {"true", "false", "null"}) {
            const std::size_t n = std::strlen(lit);
            if (t_.compare(p_, n, lit) == 0) { p_ += n; out.kind = Json::Kind::Null; return true; }
        }
        return false;
    }
    bool string(std::string& s) {
        ++p_;
        while (p_ < t_.size() && t_[p_] != '"') {
            if (t_[p_] == '\\' && p_ + 1 < t_.size()) { ++p_; }
            s.push_back(t_[p_++]);
        }
        if (p_ >= t_.size()) return false;
        ++p_;
        return true;
    }
    bool array(Json& out) {
        out.kind = Json::Kind::Arr;
        ++p_; ws();
        if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
        while (true) {
            Json v;
            ws();
            if (!value(v)) return false;
            out.arr.push_back(std::move(v));
            ws();
            if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
            if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
            return false;
        }
    }
    bool object(Json& out) {
        out.kind = Json::Kind::Obj;
        ++p_; ws();
        if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
        while (true) {
            ws();
            std::string k;
            if (p_ >= t_.size() || t_[p_] != '"' || !string(k)) return false;
            ws();
            if (p_ >= t_.size() || t_[p_] != ':') return false;
            ++p_; ws();
            Json v;
            if (!value(v)) return false;
            out.obj[k] = std::move(v);
            ws();
            if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
            if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
            return false;
        }
    }
};

} // namespace

bool read_safetensors(std::span<const uint8_t> bytes, std::map<std::string, Tensor>& out, std::string* err) {
    if (bytes.size() < 8) { if (err) *err = "safetensors: too short"; return false; }
    uint64_t hlen = 0;
    std::memcpy(&hlen, bytes.data(), 8);   // little endian(x86 前提。要件 §1: 対象機固定)
    if (hlen == 0 || hlen > (1u << 24) || 8 + hlen > bytes.size()) { if (err) *err = "safetensors: bad header length"; return false; }
    const std::string header(reinterpret_cast<const char*>(bytes.data() + 8), hlen);
    const std::span<const uint8_t> data = bytes.subspan(8 + hlen);
    Json root;
    Parser ps(header);
    if (!ps.parse(root) || root.kind != Json::Kind::Obj) { if (err) *err = "safetensors: cannot parse header"; return false; }
    for (const auto& [name, t] : root.obj) {
        if (name == "__metadata__" || t.kind != Json::Kind::Obj) continue;
        const auto dt = t.obj.find("dtype"), sh = t.obj.find("shape"), off = t.obj.find("data_offsets");
        if (dt == t.obj.end() || sh == t.obj.end() || off == t.obj.end() || off->second.arr.size() != 2) { if (err) *err = "safetensors: bad tensor entry " + name; return false; }
        if (dt->second.s != "F32") { if (err) *err = "safetensors: tensor " + name + " is " + dt->second.s + ", only F32 supported"; return false; }
        Tensor tn;
        for (const auto& d : sh->second.arr) tn.shape.push_back(d.i);
        const int64_t begin = off->second.arr[0].i, end = off->second.arr[1].i;
        if (begin < 0 || end < begin || (end - begin) != tn.numel() * 4 || static_cast<uint64_t>(end) > data.size()) { if (err) *err = "safetensors: tensor " + name + " size mismatch"; return false; }
        tn.data.resize(static_cast<std::size_t>(tn.numel()));
        std::memcpy(tn.data.data(), data.data() + begin, static_cast<std::size_t>(end - begin));
        out[name] = std::move(tn);
    }
    return true;
}

} // namespace spear::std_t98::secret
