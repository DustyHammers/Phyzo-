// Generic input helpers: files, CSV, a minimal JSON parser, WAV read/write.
#pragma once
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace io {

inline std::vector<uint8_t> readFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline void writeFile(const std::string &path, const std::vector<uint8_t> &b) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write((const char *)b.data(), (std::streamsize)b.size());
}
inline std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// ---- CSV with header row; handles quotes and CRLF
using CsvRow = std::map<std::string, std::string>;
inline std::vector<CsvRow> readCsv(const std::string &path) {
    auto raw = readFile(path); std::string t(raw.begin(), raw.end());
    if (t.size() >= 3 && (uint8_t)t[0] == 0xEF) t = t.substr(3);  // BOM
    std::vector<std::vector<std::string>> rows; std::vector<std::string> row; std::string cell; bool q = false;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (q) { if (c == '"') { if (i + 1 < t.size() && t[i + 1] == '"') { cell += '"'; ++i; } else q = false; } else cell += c; continue; }
        if (c == '"') q = true; else if (c == ',') { row.push_back(cell); cell.clear(); }
        else if (c == '\n') { row.push_back(cell); cell.clear(); if (!(row.size() == 1 && trim(row[0]).empty())) rows.push_back(row); row.clear(); }
        else if (c != '\r') cell += c;
    }
    if (!cell.empty() || !row.empty()) { row.push_back(cell); rows.push_back(row); }
    std::vector<CsvRow> out; if (rows.empty()) return out;
    for (size_t r = 1; r < rows.size(); ++r) { CsvRow m; for (size_t c = 0; c < rows[0].size(); ++c) m[trim(rows[0][c])] = c < rows[r].size() ? trim(rows[r][c]) : ""; out.push_back(m); }
    return out;
}
// "a=1;b=2" -> map
inline std::map<std::string, std::string> params(const std::string &s) {
    std::map<std::string, std::string> m; std::stringstream ss(s); std::string kv;
    while (std::getline(ss, kv, ';')) { auto e = kv.find('='); if (e != std::string::npos) m[trim(kv.substr(0, e))] = trim(kv.substr(e + 1)); }
    return m;
}

// ---- Minimal JSON
struct Json {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false; double n = 0; std::string s; std::vector<Json> a; std::map<std::string, Json> o;
    const Json &operator[](const std::string &k) const {
        auto it = o.find(k); if (it == o.end()) throw std::runtime_error("JSON key missing: " + k); return it->second;
    }
    bool has(const std::string &k) const { return o.count(k) != 0; }
    long long i() const { return (long long)n; }
};
struct JsonParser {
    const std::string &s; size_t p = 0;
    explicit JsonParser(const std::string &str) : s(str) {}
    void ws() { while (p < s.size() && isspace((unsigned char)s[p])) ++p; }
    [[noreturn]] void fail(const char *m) { throw std::runtime_error(std::string("JSON: ") + m + " at " + std::to_string(p)); }
    Json value() {
        ws(); if (p >= s.size()) fail("eof"); Json j; char c = s[p];
        if (c == '{') { j.t = Json::Obj; ++p; ws(); if (s[p] == '}') { ++p; return j; }
            for (;;) { ws(); Json k = value(); if (k.t != Json::Str) fail("key"); ws(); if (s[p++] != ':') fail(":"); j.o[k.s] = value(); ws();
                if (s[p] == ',') { ++p; continue; } if (s[p] == '}') { ++p; return j; } fail("object"); } }
        if (c == '[') { j.t = Json::Arr; ++p; ws(); if (s[p] == ']') { ++p; return j; }
            for (;;) { j.a.push_back(value()); ws(); if (s[p] == ',') { ++p; continue; } if (s[p] == ']') { ++p; return j; } fail("array"); } }
        if (c == '"') { j.t = Json::Str; ++p; while (p < s.size() && s[p] != '"') { if (s[p] == '\\') { ++p; char e = s[p];
                    if (e == 'n') j.s += '\n'; else if (e == 't') j.s += '\t'; else if (e == 'u') { j.s += '?'; p += 4; } else j.s += e; } else j.s += s[p]; ++p; }
            ++p; return j; }
        if (s.compare(p, 4, "true") == 0) { p += 4; j.t = Json::Bool; j.b = true; return j; }
        if (s.compare(p, 5, "false") == 0) { p += 5; j.t = Json::Bool; return j; }
        if (s.compare(p, 4, "null") == 0) { p += 4; return j; }
        size_t q = p; while (p < s.size() && (isdigit((unsigned char)s[p]) || strchr("+-.eE", s[p]))) ++p;
        if (q == p) fail("value");
        j.t = Json::Num; j.n = std::stod(s.substr(q, p - q)); return j;
    }
};
inline Json readJson(const std::string &path) { auto r = readFile(path); std::string t(r.begin(), r.end()); JsonParser jp(t); return jp.value(); }

// ---- WAV (PCM 16-bit mono). Tolerates a wrong RIFF size field; trusts the data chunk.
struct Wav { uint32_t rate = 0; uint16_t channels = 0, bits = 0; std::vector<int16_t> samples; };
inline Wav readWav(const std::string &path) {
    auto d = readFile(path); Wav w;
    if (d.size() < 12 || std::string(d.begin(), d.begin() + 4) != "RIFF" || std::string(d.begin() + 8, d.begin() + 12) != "WAVE") throw std::runtime_error("not a WAV: " + path);
    auto u32 = [&](size_t o) { return (uint32_t)d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24); };
    size_t pos = 12; bool gotData = false;
    while (pos + 8 <= d.size()) {
        std::string id(d.begin() + pos, d.begin() + pos + 4); uint32_t sz = u32(pos + 4); size_t body = pos + 8;
        if (id == "fmt ") { if ((d[body] | (d[body + 1] << 8)) != 1) throw std::runtime_error("WAV not PCM: " + path);
            w.channels = d[body + 2] | (d[body + 3] << 8); w.rate = u32(body + 4); w.bits = d[body + 14] | (d[body + 15] << 8); }
        if (id == "data") { if (body + sz > d.size()) throw std::runtime_error("WAV data chunk truncated: " + path);
            if (w.bits != 16 || w.channels != 1) throw std::runtime_error("WAV must be 16-bit mono: " + path);
            for (size_t i = 0; i < sz / 2; ++i) w.samples.push_back((int16_t)(d[body + 2 * i] | (d[body + 2 * i + 1] << 8)));
            gotData = true; }
        pos = body + sz + (sz & 1);
    }
    if (!gotData) throw std::runtime_error("WAV has no data chunk: " + path);
    return w;
}
inline void writeWav16(const std::string &path, const std::vector<int16_t> &s, uint32_t rate) {
    std::vector<uint8_t> b; auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); };
    auto p16 = [&](uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); };
    uint32_t dataBytes = (uint32_t)s.size() * 2;
    b.insert(b.end(), {'R', 'I', 'F', 'F'}); p32(36 + dataBytes); b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    p32(16); p16(1); p16(1); p32(rate); p32(rate * 2); p16(2); p16(16); b.insert(b.end(), {'d', 'a', 't', 'a'}); p32(dataBytes);
    for (int16_t v : s) p16((uint16_t)v);
    writeFile(path, b);
}

}  // namespace io
