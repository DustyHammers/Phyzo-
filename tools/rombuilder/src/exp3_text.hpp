// Parser for the EXP-3 text listing ("Wave NAME:" / "Sample n:" / "Loop:" / "Keys:" blocks).
// Note names use C-2 = MIDI 0.
#pragma once
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include "io.hpp"

namespace exp3 {

struct Sample { int n = 0; long length = 0, playStart = 0, loopStart = 0, loopEnd = 0, frames = 0; std::string mode; int root = 0, tune = 0, low = 0, high = 0; std::string lowName, highName, rootName; };

inline int noteNumber(const std::string &s) {
    static const std::regex re(R"(^([A-G])(#?)(-?\d+)$)"); std::smatch m;
    if (!std::regex_match(s, m, re)) throw std::runtime_error("bad note name: " + s);
    static const std::map<char, int> base{{'C', 0}, {'D', 2}, {'E', 4}, {'F', 5}, {'G', 7}, {'A', 9}, {'B', 11}};
    return base.at(m[1].str()[0]) + (m[2].length() ? 1 : 0) + 12 * (std::stoi(m[3]) + 2);
}

inline std::map<std::string, std::vector<Sample>> parse(const std::string &path) {
    auto raw = io::readFile(path); std::string t(raw.begin(), raw.end());
    std::map<std::string, std::vector<Sample>> out; std::string cur; std::stringstream ss(t); std::string line;
    static const std::regex reWave(R"(^Wave (.+?)\s*:\s*$)");
    static const std::regex reSample(R"(^\s+Sample (\d+): length=(-?\d+), playStart=(-?\d+))");
    static const std::regex reLoop(R"(^\s+Loop: mode=(\w+), start=(-?\d+), end=(-?\d+)(?:.*frames=(\d+))?)");
    static const std::regex reKeys(R"(^\s+Keys: root=(\S+), tune=(-?\d+), low=(\S+), high=(\S+))");
    std::smatch m;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_match(line, m, reWave)) { cur = io::trim(m[1]); out[cur]; continue; }
        if (cur.empty()) continue;
        if (std::regex_search(line, m, reSample)) { Sample s; s.n = std::stoi(m[1]); s.length = std::stol(m[2]); s.playStart = std::stol(m[3]); out[cur].push_back(s); continue; }
        if (out[cur].empty()) continue;
        Sample &s = out[cur].back();
        if (std::regex_search(line, m, reLoop)) { s.mode = m[1]; s.loopStart = std::stol(m[2]); s.loopEnd = std::stol(m[3]); s.frames = m[4].matched ? std::stol(m[4]) : 0; continue; }
        if (std::regex_search(line, m, reKeys)) { s.rootName = m[1]; s.root = noteNumber(m[1]); s.tune = std::stoi(m[2]); s.lowName = m[3]; s.highName = m[4]; s.low = noteNumber(m[3]); s.high = noteNumber(m[4]); }
    }
    return out;
}

}  // namespace exp3
