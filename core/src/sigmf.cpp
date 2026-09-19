#include "spear/core/sigmf.hpp"

#include <cstdio>
#include <filesystem>
#include <iterator>
#include <sstream>

namespace spear::sigmf {

std::string data_path(const std::string& b)    { return b + ".sigmf-data"; }
std::string meta_path(const std::string& b)    { return b + ".sigmf-meta"; }
std::string sidecar_path(const std::string& b) { return b + ".spear.json"; }

std::string datatype_string(DataType t) {
    switch (t) {
    case DataType::ComplexInt16:   return "ci16_le";
    case DataType::ComplexFloat32: return "cf32_le";
    case DataType::Float32:        return "rf32_le";
    case DataType::Int16:          return "ri16_le";
    case DataType::UInt8:          return "ru8";
    default:                       return "unknown";
    }
}

DataType parse_datatype(const std::string& s) {
    if (s == "ci16_le") return DataType::ComplexInt16;
    if (s == "cf32_le") return DataType::ComplexFloat32;
    if (s == "rf32_le") return DataType::Float32;
    if (s == "ri16_le") return DataType::Int16;
    if (s == "ru8")     return DataType::UInt8;
    return DataType::Unknown;
}

namespace {

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\t': o += "\\t";  break;
        default:   o += c;
        }
    }
    return o + "\"";
}

std::string jnum(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

bool write_file(const std::string& path, const std::string& body, std::string* err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    f << body;
    return static_cast<bool>(f);
}

// 最小 JSON 走査: "key": <value> の value 文字列を返す(先頭一致)。ネストは考慮しない。
bool scan_value(const std::string& s, const std::string& key, std::size_t from, std::string& out, std::size_t* pos_out = nullptr) {
    const std::string k = "\"" + key + "\"";
    auto p = s.find(k, from);
    if (p == std::string::npos) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\t' || s[p] == '\r')) ++p;
    if (p >= s.size()) return false;
    if (s[p] == '"') {
        auto e = s.find('"', p + 1);
        if (e == std::string::npos) return false;
        out = s.substr(p + 1, e - p - 1);
        if (pos_out) *pos_out = e + 1;
    } else {
        auto e = p;
        while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != ']' && s[e] != '\n') ++e;
        out = s.substr(p, e - p);
        if (pos_out) *pos_out = e;
    }
    return true;
}

} // namespace

bool write(const std::string& base, const Meta& m, std::string* err) {
    // ---- SigMF meta (interop 部分) ----
    std::ostringstream j;
    j << "{\n  \"global\": {\n"
      << "    \"core:datatype\": " << jstr(datatype_string(m.dtype)) << ",\n"
      << "    \"core:sample_rate\": " << jnum(m.sample_rate) << ",\n"
      << "    \"core:version\": \"1.0.0\",\n"
      << "    \"core:hw\": " << jstr(m.hw) << ",\n"
      << "    \"core:author\": " << jstr(m.author) << ",\n"
      << "    \"core:description\": " << jstr(m.description) << ",\n"
      << "    \"core:recorder\": \"spear\",\n";
    if (m.lo_correction_ppm != 0) j << "    \"spear:lo_correction_ppm\": " << jnum(m.lo_correction_ppm) << ",\n";
    if (!m.rx_port.empty()) j << "    \"spear:rx_port\": " << jstr(m.rx_port) << ",\n";
    j << "    \"spear:sidecar\": " << jstr(std::filesystem::path(sidecar_path(base)).filename().string()) << "\n"
      << "  },\n  \"captures\": [\n";
    for (std::size_t i = 0; i < m.captures.size(); ++i) {
        const auto& c = m.captures[i];
        j << "    {\"core:sample_start\": " << c.sample_start
          << ", \"core:frequency\": " << jnum(c.frequency) << "}" << (i + 1 < m.captures.size() ? "," : "") << "\n";
    }
    j << "  ],\n  \"annotations\": []\n}\n";
    if (!write_file(meta_path(base), j.str(), err)) return false;

    // ---- sidecar ----
    std::ostringstream s;
    s << "{\n  \"spear:version\": 1,\n"
      << "  \"time_reference\": " << jstr(m.time_reference) << ",\n"
      << "  \"time_accuracy\": " << jstr(m.time_accuracy) << ",\n"
      << "  \"total_samples\": " << m.total_samples << ",\n"
      << "  \"captures\": [\n";
    for (std::size_t i = 0; i < m.captures.size(); ++i) {
        const auto& c = m.captures[i];
        s << "    {\"sample_start\": " << c.sample_start << ", \"generation\": " << c.generation
          << ", \"sample_index\": " << c.sample_index << ", \"frequency\": " << jnum(c.frequency)
          << ", \"hw_time\": " << jnum(c.hw_time.seconds()) << ", \"hw_time_valid\": " << (c.hw_time.valid ? "true" : "false")
          << ", \"host_ns\": " << c.host_ns << "}" << (i + 1 < m.captures.size() ? "," : "") << "\n";
    }
    s << "  ],\n  \"discontinuities\": [\n";
    for (std::size_t i = 0; i < m.discontinuities.size(); ++i) {
        const auto& d = m.discontinuities[i];
        s << "    {\"file_sample\": " << d.file_sample << ", \"generation\": " << d.generation
          << ", \"sample_index\": " << d.sample_index << ", \"missing_samples\": " << d.missing_samples
          << ", \"flags\": " << jstr(d.flags) << "}" << (i + 1 < m.discontinuities.size() ? "," : "") << "\n";
    }
    s << "  ],\n  \"tuning\": [\n";
    for (std::size_t i = 0; i < m.tuning.size(); ++i) {
        const auto& t = m.tuning[i];
        s << "    {\"generation\": " << t.generation << ", \"sample_index\": " << t.sample_index
          << ", \"frequency\": " << jnum(t.frequency) << ", \"host_ns\": " << t.host_ns << "}"
          << (i + 1 < m.tuning.size() ? "," : "") << "\n";
    }
    s << "  ],\n  \"time_references\": [\n";
    for (std::size_t i = 0; i < m.time_refs.size(); ++i) {
        const auto& t = m.time_refs[i];
        s << "    {\"generation\": " << t.generation << ", \"hw_time\": " << jnum(t.hw_time_s)
          << ", \"hw_sample_index\": " << t.hw_sample_index << ", \"host_mono_ns\": " << t.host_mono_ns
          << ", \"utc_ns\": " << t.utc_ns << "}" << (i + 1 < m.time_refs.size() ? "," : "") << "\n";
    }
    s << "  ],\n  \"events\": [\n";
    for (std::size_t i = 0; i < m.events.size(); ++i) {
        const auto& e = m.events[i];
        s << "    {\"host_ns\": " << e.host_ns << ", \"kind\": " << jstr(e.kind) << ", \"source\": " << jstr(e.source)
          << ", \"detail\": " << jstr(e.detail) << ", \"generation\": " << e.range.generation
          << ", \"begin\": " << e.range.begin << ", \"end\": " << e.range.end << ", \"value\": " << e.value << "}"
          << (i + 1 < m.events.size() ? "," : "") << "\n";
    }
    s << "  ]\n}\n";
    return write_file(sidecar_path(base), s.str(), err);
}

bool read(const std::string& base, Meta& m, std::string* err) {
    std::ifstream f(meta_path(base), std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + meta_path(base); return false; }
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string v;
    if (!scan_value(s, "core:datatype", 0, v)) { if (err) *err = "core:datatype missing"; return false; }
    m.dtype = parse_datatype(v);
    if (m.dtype == DataType::Unknown) { if (err) *err = "unsupported datatype " + v; return false; }
    if (!scan_value(s, "core:sample_rate", 0, v)) { if (err) *err = "core:sample_rate missing"; return false; }
    m.sample_rate = std::stod(v);
    if (scan_value(s, "core:description", 0, v)) m.description = v;
    if (scan_value(s, "core:hw", 0, v)) m.hw = v;

    m.captures.clear();
    std::size_t pos = 0;
    while (scan_value(s, "core:sample_start", pos, v, &pos)) {
        Capture c;
        c.sample_start = std::stoull(v);
        std::string fv;
        if (scan_value(s, "core:frequency", pos, fv, &pos)) c.frequency = std::stod(fv);
        m.captures.push_back(c);
    }

    // sidecar は任意(他ツールで作った SigMF も再生できる)
    std::ifstream sc(sidecar_path(base), std::ios::binary);
    if (sc) {
        std::string t((std::istreambuf_iterator<char>(sc)), std::istreambuf_iterator<char>());
        if (scan_value(t, "time_reference", 0, v)) m.time_reference = v;
        if (scan_value(t, "time_accuracy", 0, v)) m.time_accuracy = v;
        if (scan_value(t, "total_samples", 0, v)) m.total_samples = std::stoull(v);
        // captures の generation / sample_index を突き合わせる(順序一致前提)
        pos = 0;
        std::size_t i = 0;
        std::string g;
        auto cap_start = t.find("\"captures\"");
        pos = cap_start == std::string::npos ? t.size() : cap_start;
        while (i < m.captures.size() && scan_value(t, "generation", pos, g, &pos)) {
            std::string si;
            m.captures[i].generation = std::stoull(g);
            if (scan_value(t, "sample_index", pos, si, &pos)) m.captures[i].sample_index = std::stoull(si);
            std::string ht;
            if (scan_value(t, "hw_time", pos, ht, &pos)) {
                double sec = std::stod(ht);
                m.captures[i].hw_time.full_secs = static_cast<int64_t>(sec);
                m.captures[i].hw_time.frac_secs = sec - static_cast<double>(m.captures[i].hw_time.full_secs);
                std::string hv;
                if (scan_value(t, "hw_time_valid", pos, hv, &pos)) m.captures[i].hw_time.valid = (hv == "true");
            }
            ++i;
            auto disc = t.find("\"discontinuities\"");
            if (disc != std::string::npos && pos > disc) break;
        }
    }
    return true;
}

} // namespace spear::sigmf
