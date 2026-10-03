// phyzo_play: boots the user-supplied OS image with the voice-chip core and the user's native wave image, then
// runs a timed event script (MIDI, panel bytes, RAM pokes) in real-time steps and records the dry stereo
// voice-chip output to WAV (44.1 kHz, 32-bit float; 1.0 = the chip's 20-bit full scale).
//
// Script lines (times in ms from the end of boot + settle; '#' starts a comment):
//   <t> midi <hex bytes...>        bytes into MIDI in (serial channel B)
//   <t> panel <hex bytes...>       bytes from the front-panel controller (serial channel A)
//   <t> poke8|poke16|poke32 <addr> <value>   write emulated RAM (diagnostics only)
//   <t> find <hex bytes...>        log every RAM address holding this byte string
//   <t> rec <name>                 start recording to <out>/<name>.wav (stops any running recording)
//   <t> stop                       stop recording
//   <t> display [label]           log the 4-character display
//   <t> esp2reg <hex addr...>      log ESP2 register values
//   <t> end                        end of script
// Options: --state-limit <bits> sets the resonant filter state saturation (internal units, default 21 = 18-bit registers).
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "machine.h"
#include "os_image.h"
#include "rom_id.h"

namespace {
struct Event { double t; std::string cmd; std::vector<std::string> args; };

// 32-bit float WAV, 1.0 = full scale of the chip's 20-bit output (2^19); values beyond 1.0 are kept.
void writeWavFloat(const std::string& path, const std::vector<int32_t>& lr) {
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    auto p32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };   // host is little-endian (Linux/macOS)
    auto p16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(lr.size() * 4);
    std::fwrite("RIFF", 1, 4, f); p32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    p32(16); p16(3); p16(2); p32(44100); p32(44100 * 8); p16(8); p16(32); std::fwrite("data", 1, 4, f); p32(bytes);
    for (int32_t s : lr) { float v = float(s) / float(1 << 19); std::fwrite(&v, 4, 1, f); }
    std::fclose(f);
}
// 24-bit DAC words (ESP2 serial output) to 32-bit float WAV, 1.0 = 2^23; nothing is clamped beyond the chip's own width.
void writeWavDac(const std::string& path, const std::vector<int32_t>& lr) {
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    auto p32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto p16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(lr.size() * 4);
    std::fwrite("RIFF", 1, 4, f); p32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    p32(16); p16(3); p16(2); p32(44100); p32(44100 * 8); p16(8); p16(32); std::fwrite("data", 1, 4, f); p32(bytes);
    for (int32_t s : lr) { float v = float(s) / 8388608.0f; std::fwrite(&v, 4, 1, f); }
    std::fclose(f);
}
// Minimal WAV reader for the audio-input test signal: 16-bit PCM or 32-bit float, mono or stereo, any length.
bool readWavStereo(const std::string& path, std::vector<float>& out) {
    FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return false;
    std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4)) return false;
    int fmt = 0, ch = 0, bits = 0; size_t pos = 12;
    while (pos + 8 <= b.size()) {
        uint32_t len = b[pos + 4] | b[pos + 5] << 8 | b[pos + 6] << 16 | uint32_t(b[pos + 7]) << 24;
        if (!std::memcmp(&b[pos], "fmt ", 4)) { fmt = b[pos + 8] | b[pos + 9] << 8; ch = b[pos + 10]; bits = b[pos + 22]; }
        if (!std::memcmp(&b[pos], "data", 4)) {
            size_t end = std::min(b.size(), pos + 8 + len), step = size_t(ch * bits / 8);
            for (size_t i = pos + 8; i + step <= end; i += step)
                for (int c = 0; c < 2; ++c) {
                    size_t o = i + size_t(std::min(c, ch - 1) * bits / 8);
                    float v; if (fmt == 3 && bits == 32) std::memcpy(&v, &b[o], 4); else if (bits == 16) v = int16_t(b[o] | b[o + 1] << 8) / 32768.0f; else return false;
                    out.push_back(v);
                }
            return !out.empty();
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}
std::vector<uint8_t> hexBytes(const std::vector<std::string>& a, size_t from) {
    std::vector<uint8_t> b; for (size_t i = from; i < a.size(); ++i) b.push_back(uint8_t(std::strtoul(a[i].c_str(), nullptr, 16))); return b;
}
}  // namespace

int main(int argc, char** argv) {
    std::string osPath, wavePath, romsDir, script, out = "play_out", expect = "P 01";
    bool noAnswerF4 = false;
    int stateLimitBits = 0; bool esp2Stub = false, ram16 = false, profileRun = false, esp2Ref = false, esp2Checkpoint = false, esp2Alias = false, traceEsp2 = false; std::string audioInPath; int ips = 192; double bootMs = 8000, settleMs = 1000, stepMs = 10; bool trace = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::exit(2); } return argv[++i]; };
        if (a == "--os") osPath = next(); else if (a == "--wave") wavePath = next(); else if (a == "--script") script = next();
        else if (a == "--roms") romsDir = next();
        else if (a == "--out") out = next(); else if (a == "--expect") expect = next(); else if (a == "--step-ms") stepMs = std::atof(next().c_str());
        else if (a == "--settle-ms") settleMs = std::atof(next().c_str()); else if (a == "--trace") trace = true;
        else if (a == "--state-limit") stateLimitBits = std::atoi(next().c_str());
        else if (a == "--esp2-stub") esp2Stub = true;
        else if (a == "--no-answer-f4") noAnswerF4 = true;
        else if (a == "--profile") profileRun = true;
        else if (a == "--esp2-ref") esp2Ref = true;
        else if (a == "--esp2-checkpoint") esp2Checkpoint = true;
        else if (a == "--esp2-ram-alias") esp2Alias = true;
        else if (a == "--trace-esp2") traceEsp2 = true;
        else if (a == "--esp2-ram16") ram16 = true;
        else if (a == "--esp2-ips") ips = std::atoi(next().c_str());
        else if (a == "--audio-in") audioInPath = next();
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (!romsDir.empty()) {   // find the ROMs by checksum; an explicit --os/--wave wins
        romid::ScanResult rs = romid::scanFolder(romsDir);
        if (osPath.empty()) osPath = rs.osPath;
        if (wavePath.empty()) wavePath = rs.wavePath;
        if (osPath.empty() || wavePath.empty()) { std::fprintf(stderr, "%s\n", rs.problem().c_str()); return 2; }
    }
    if (osPath.empty() || wavePath.empty() || script.empty()) {
        std::puts("usage: phyzo_play (--os <image> --wave <native wave image> | --roms <folder>) --script <file> [--out dir] [--trace] [--step-ms 10]"); return 2;
    }
    mkdir(out.c_str(), 0755);
    std::vector<Event> ev;
    { std::ifstream f(script); std::string line;
      while (std::getline(f, line)) { auto h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
          std::istringstream ss(line); Event e; if (!(ss >> e.t >> e.cmd)) continue; std::string x; while (ss >> x) e.args.push_back(x); ev.push_back(e); }
      std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.t < b.t; }); }

    OsImage os; std::string err;
    if (!os.load(osPath, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (os.md5 != romid::kOsImageMd5)
        std::fprintf(stderr, "warning: OS image MD5 %s is not the supported version (%s)\n", os.md5.c_str(), romid::kOsImageMd5);
    Machine m; Machine::Config cfg; cfg.esp2Stub = esp2Stub; cfg.esp2InstrPerSample = ips;
    if (!m.init(os, cfg, err) || !m.loadWaveMemory(wavePath, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (trace) m.setTraceDir(out);
    m.panel.answerF4 = !noAnswerF4;
    m.esp2.ram16 = ram16; m.esp2.ramAlias = esp2Alias;
    if (traceEsp2 && !trace) { m.esp2.log = std::fopen((out + "/esp2_commands.csv").c_str(), "w"); if (m.esp2.log) std::fprintf(m.esp2.log, "t_ms,source,op,addr,value\n"); }
    m.esp2.extLog = std::fopen((out + "/esp2_external_writes.csv").c_str(), "w");
    if (m.esp2.extLog) std::fprintf(m.esp2.extLog, "esp2_cycle,pc,addr,value,decode\n"); m.profile = profileRun; m.esp2.fastPath = !esp2Ref; m.esp2.specFixes = !esp2Checkpoint;
    if (const char* w = std::getenv("ESP2_WATCH")) m.esp2.watch = std::atoi(w);
    if (!audioInPath.empty() && !readWavStereo(audioInPath, m.audioIn)) { std::fprintf(stderr, "cannot read %s (16-bit PCM or 32-bit float WAV)\n", audioInPath.c_str()); return 2; }
    if (stateLimitBits) m.voice.stateLimit = int32_t(1) << stateLimitBits;   // resonant-section saturation (default 18-bit width)
    FILE* sum = std::fopen((out + "/summary.txt").c_str(), "w");
    auto both = [&](const char* fmt, auto... args) { std::printf(fmt, args...); if (sum) std::fprintf(sum, fmt, args...); };
    both("os_md5: %s\nwave_image: %s\nscript: %s\n", os.md5.c_str(), wavePath.c_str(), script.c_str());

    auto seen = [&]() { return m.panel.text() == expect; };
    if (m.runUntil(m.cyclesFromMs(bootMs), seen) != Machine::Stop::Predicate) { both("boot FAILED, display \"%s\"\n", m.panel.text().c_str()); return 1; }
    m.runUntil(m.cycles() + m.cyclesFromMs(settleMs));
    both("boot: \"%s\" at %.1f ms (+%.0f ms settle)\n", m.panel.text().c_str(), m.ms(m.cycles()) - settleMs, settleMs);
    if (!esp2Stub) both("esp2_reg_0F9: 0x%06X\n", m.esp2.reg(0x0F9));

    const uint64_t t0 = m.cycles();
    std::string recName; size_t recStart = 0; double recHost = 0, recEmu = 0, totalHost = 0, totalEmu = 0; uint64_t l5Start = m.irqTaken[5];
    uint64_t resStart = 0, clampStart = 0;
    struct EspSnap { uint64_t macSat, aluSat, satDac, dacSat, portClips, overruns, unmapped, executed, suspended; };
    auto snap = [&]() { return EspSnap{m.esp2.macSat, m.esp2.aluSat, m.esp2.satToDac, m.esp2.dacSatSamples, m.voicePortClips, m.esp2.overruns, m.esp2.memUnmapped, m.esp2.executed, m.esp2.suspendedCycles}; };
    EspSnap e0 = snap();
    auto sat0 = m.esp2.satDest;
    std::map<uint32_t, double> over0;
    FILE* clip = std::fopen((out + "/clip_report.csv").c_str(), "w");
    if (clip) std::fprintf(clip, "recording,seconds,dry_peak_dbfs,dry_over_20bit,voice_port_clamped_words,wet_peak_dbfs,wet_rms_dbfs,dac_fullscale_samples,esp2_mac_saturations,esp2_alu_saturations,saturated_writes_to_dac_regs,esp2_overruns,esp2_unmapped_mem,esp2_load_pct,saturations_by_destination\n");
    auto stopRec = [&]() {
        if (recName.empty()) return;
        std::vector<int32_t> seg(m.audio.begin() + long(recStart), m.audio.end());
        int32_t peak = 0; uint64_t clips = 0; double rms = 0;
        for (int32_t s : seg) { peak = std::max(peak, std::abs(s)); if (s >= (1 << 19) || s < -(1 << 19)) ++clips; rms += double(s) * s; }
        rms = seg.empty() ? 0 : std::sqrt(rms / seg.size());
        writeWavFloat(out + "/" + recName + ".wav", seg);
        if (!esp2Stub) {
            std::vector<int32_t> w(m.wet.begin(), m.wet.end());
            int32_t wp = 0; double wr = 0;
            for (int32_t x : w) { wp = std::max(wp, std::abs(x)); wr += double(x) * x; }
            wr = w.empty() ? 0 : std::sqrt(wr / w.size());
            writeWavDac(out + "/" + recName + "_wet.wav", w);
            EspSnap e1 = snap();
            std::string satBy;                     // saturations in this recording by destination register
            for (uint32_t k = 0; k < Esp2Core::kSatKeys; ++k) {
                const uint64_t n0 = sat0[k], n = m.esp2.satDest[k];
                if (n > n0) { char b[64]; std::snprintf(b, sizeof b, "%s%s%03X:%llu:%.3g", satBy.empty() ? "" : " ", (k & 0x1000) ? "MAC>" : "ALU>", k & 0x3FF, (unsigned long long)(n - n0), m.esp2.satOver[k]); satBy += b; }
            }
            double load = e1.executed > e0.executed ? 100.0 * double(e1.executed - e0.executed) / double(e1.executed - e0.executed + e1.suspended - e0.suspended) : 0;
            auto db = [](double v, double fs) { return v > 0 ? 20 * std::log10(v / fs) : -999.0; };
            both("    wet %-36s peak %6.1f dBFS  rms %6.1f dBFS  dac_fullscale %llu  mac_sat %llu  alu_sat %llu  sat->dac %llu  port_clamps %llu  overruns %llu  esp2_load %.1f%%\n", recName.c_str(),
                 db(wp, 8388608.0), db(wr, 8388608.0), (unsigned long long)(e1.dacSat - e0.dacSat), (unsigned long long)(e1.macSat - e0.macSat),
                 (unsigned long long)(e1.aluSat - e0.aluSat), (unsigned long long)(e1.satDac - e0.satDac), (unsigned long long)(e1.portClips - e0.portClips),
                 (unsigned long long)(e1.overruns - e0.overruns), load);
            if (!satBy.empty()) both("        saturations by destination: %s\n", satBy.c_str());
            if (clip) std::fprintf(clip, "%s,%.3f,%.2f,%llu,%llu,%.2f,%.2f,%llu,%llu,%llu,%llu,%llu,%llu,%.1f,%s\n", recName.c_str(), seg.size() / 2 / 44100.0,
                 db(peak, double(1 << 19)), (unsigned long long)clips, (unsigned long long)(e1.portClips - e0.portClips), db(wp, 8388608.0), db(wr, 8388608.0),
                 (unsigned long long)(e1.dacSat - e0.dacSat), (unsigned long long)(e1.macSat - e0.macSat), (unsigned long long)(e1.aluSat - e0.aluSat),
                 (unsigned long long)(e1.satDac - e0.satDac), (unsigned long long)(e1.overruns - e0.overruns), (unsigned long long)(e1.unmapped - e0.unmapped), load, satBy.c_str());
        }
        both("rec %-40s %7.3f s  peak %6.1f dBFS  rms %6.1f dBFS  over20bit %llu  rtf %.2fx  L5 irqs %llu  res_samples %llu  filter_clamps %llu\n", recName.c_str(), seg.size() / 2 / 44100.0,
             peak ? 20 * std::log10(peak / double(1 << 19)) : -999.0, rms > 0 ? 20 * std::log10(rms / double(1 << 19)) : -999.0,
             (unsigned long long)clips, recHost > 0 ? recEmu / recHost : 0.0, (unsigned long long)(m.irqTaken[5] - l5Start),
             (unsigned long long)(m.voice.resonantSamples - resStart), (unsigned long long)(m.voice.stateClamps - clampStart));
        recName.clear(); m.captureAudio = false; m.audio.clear(); m.wet.clear();
    };
    size_t k = 0; bool done = false;
    while (!done) {
        double now = m.ms(m.cycles() - t0);
        while (k < ev.size() && ev[k].t <= now + 1e-9) {
            const Event& e = ev[k++];
            if (e.cmd == "midi") m.serial.queueRx(1, hexBytes(e.args, 0), m.cycles());
            else if (e.cmd == "panel") m.panel.inject(hexBytes(e.args, 0), m.cycles(), "script");
            else if (e.cmd.rfind("poke", 0) == 0) {
                int size = e.cmd == "poke8" ? 1 : e.cmd == "poke16" ? 2 : 4;
                m.write(uint32_t(std::strtoul(e.args[0].c_str(), nullptr, 16)), uint32_t(std::strtoul(e.args[1].c_str(), nullptr, 16)), size);
            } else if (e.cmd == "find") {
                auto pat = hexBytes(e.args, 0); both("find %s:", "");
                for (uint32_t a = Machine::kRamBase; a + pat.size() <= Machine::kRamBase + Machine::kRamSize; ++a) {
                    bool ok = true; for (size_t i = 0; i < pat.size() && ok; ++i) ok = m.peek(a + uint32_t(i), 1) == pat[i];
                    if (ok) both(" %08X", a);
                }
                both("%s", "\n");
            } else if (e.cmd == "rec") { stopRec(); recName = e.args.at(0); m.captureAudio = true; recStart = m.audio.size(); recHost = recEmu = 0; l5Start = m.irqTaken[5]; resStart = m.voice.resonantSamples; clampStart = m.voice.stateClamps; m.wet.clear(); e0 = snap(); sat0 = m.esp2.satDest; m.esp2.satOver.fill(0); }
            else if (e.cmd == "stop") stopRec();
            else if (e.cmd == "display") both("display %.0f ms: \"%s\"  %s\n", e.t, m.panel.text().c_str(), e.args.empty() ? "" : e.args[0].c_str());
            else if (e.cmd == "esp2epoch") { ++m.esp2.epoch; if (e.args.size() >= 2) { m.esp2.staleWinLo = uint32_t(std::strtoul(e.args[0].c_str(), nullptr, 16)); m.esp2.staleWinHi = uint32_t(std::strtoul(e.args[1].c_str(), nullptr, 16)); } both("esp2epoch %.0f ms: %u\n", e.t, m.esp2.epoch); }
            else if (e.cmd == "esp2stats") {
                const Esp2Core& q = m.esp2;
                both("esp2stats %.0f ms %s: mac_sat %llu alu_sat %llu stale_reads %llu stale_span %06X-%06X region_violations %llu unmapped %llu ext_writes %llu overruns %llu\n",
                     e.t, e.args.empty() ? "" : e.args[0].c_str(), (unsigned long long)q.macSat, (unsigned long long)q.aluSat, (unsigned long long)q.staleReads,
                     q.staleLow == 0xFFFFFFFF ? 0 : q.staleLow, q.staleHigh, (unsigned long long)q.regionViolations, (unsigned long long)q.memUnmapped,
                     (unsigned long long)q.extWrites, (unsigned long long)q.overruns);
            }
            else if (e.cmd == "esp2dump") {         // esp2dump <name> [ram_start_hex count]: instruction memory, registers, RAM words + write epochs
                FILE* f = std::fopen((out + "/" + e.args.at(0) + ".dump").c_str(), "w");
                if (f) {
                    std::fprintf(f, "# t_ms %.0f epoch %u pc %03X\n", e.t, m.esp2.epoch, m.esp2.pc());
                    for (int a = 0; a < Esp2Core::kInstr; ++a) { std::fprintf(f, "I %03X ", a); for (int b = 0; b < 12; ++b) std::fprintf(f, "%02X", m.esp2.imem()[a][b]); std::fprintf(f, "\n"); }
                    for (int a = 0; a < Esp2Core::kRegs; ++a) std::fprintf(f, "R %03X %06X\n", a, m.esp2.reg(a));
                    if (e.args.size() >= 3) {
                        uint32_t s0 = uint32_t(std::strtoul(e.args[1].c_str(), nullptr, 16)), n = uint32_t(std::strtoul(e.args[2].c_str(), nullptr, 16));
                        for (uint32_t i = 0; i < n; ++i) { uint32_t ra = s0 + i - Esp2Core::kRamBase; std::fprintf(f, "M %06X %06X %u\n", s0 + i, m.esp2.ramWord(ra), ra < Esp2Core::kRamWords ? m.esp2.wEpoch[ra] : 0); }
                    }
                    std::fclose(f);
                }
            }
            else if (e.cmd == "esp2reg") {
                both("esp2reg %.0f ms:", e.t);
                for (auto& a : e.args) { uint32_t r = uint32_t(std::strtoul(a.c_str(), nullptr, 16)); both(" %03X=%06X", r, m.esp2.reg(int(r))); }
                both("%s", "\n");
            }
            else if (e.cmd == "end") done = true;
        }
        if (done) break;
        double h0 = m.hostSeconds;
        Machine::Stop st = m.runUntil(m.cycles() + m.cyclesFromMs(stepMs));
        double dh = m.hostSeconds - h0; totalHost += dh; totalEmu += stepMs / 1000; if (!recName.empty()) { recHost += dh; recEmu += stepMs / 1000; }
        if (st == Machine::Stop::Trap || st == Machine::Stop::HardStall) { both("STOP: %s\n", m.trapReason.c_str()); break; }
    }
    stopRec();
    both("script: emulated %.2f s, host %.2f s, real-time factor %.2fx (CPU + 48-voice core%s, %.1f ms steps)\n", totalEmu, totalHost, totalEmu / totalHost, esp2Stub ? "" : " + ESP2", stepMs);
    if (profileRun) {
        double rest = totalHost - m.profVoice - m.profEsp2;
        both("profile: host %.2f s for %.2f s emulated: 68k CPU + devices %.2f s (%.0f%%), voice core %.2f s (%.0f%%), ESP2 %.2f s (%.0f%%); alone: CPU %.1fx, voices %.1fx, ESP2 %.1fx real time\n",
             totalHost, totalEmu, rest, 100 * rest / totalHost, m.profVoice, 100 * m.profVoice / totalHost, m.profEsp2, 100 * m.profEsp2 / totalHost,
             totalEmu / rest, totalEmu / m.profVoice, m.profEsp2 > 0 ? totalEmu / m.profEsp2 : 0.0);
    }
    both("voice_core: samples %llu starts %llu irqs_raised %llu irq_acks %llu L5_taken %llu voice_stops %llu\n", (unsigned long long)m.voice.samples, (unsigned long long)m.voice.starts,
         (unsigned long long)m.voice.irqsRaised, (unsigned long long)m.voice.irqAcks, (unsigned long long)m.irqTaken[5], (unsigned long long)m.voice.voiceStops);
    both("filter: resonant-mode samples %llu, bypass samples %llu, state saturations %llu (limit 2^%d internal)\n", (unsigned long long)m.voice.resonantSamples,
         (unsigned long long)m.voice.bypassSamples, (unsigned long long)m.voice.stateClamps, int(std::log2(double(m.voice.stateLimit))));
    both("display at end: \"%s\"\n", m.panel.text().c_str());
    if (!esp2Stub) {
        const Esp2Core& e = m.esp2;
        both("esp2_core: executed %llu suspended %llu halted %llu bioz_suspends %llu passes %llu overruns %llu mac_sat %llu alu_sat %llu unmapped %llu ram %06X-%06X port_reads %llu port_writes %llu reserved_mac %llu unknown_spr %llu/%llu\n",
             (unsigned long long)e.executed, (unsigned long long)e.suspendedCycles, (unsigned long long)e.haltedCycles, (unsigned long long)e.biozSuspends,
             (unsigned long long)e.biozPasses, (unsigned long long)e.overruns, (unsigned long long)e.macSat, (unsigned long long)e.aluSat, (unsigned long long)e.memUnmapped,
             e.ramLow, e.ramHigh, (unsigned long long)e.voicePortReads, (unsigned long long)e.voicePortWrites, (unsigned long long)e.reservedMacOps,
             (unsigned long long)e.unknownSprReads, (unsigned long long)e.unknownSprWrites);
        both("%s", "esp2_unmapped_addresses:"); for (auto& kv : e.unmappedAddr) both(" %c%06X x%llu", (kv.first & 0x80000000u) ? 'W' : 'R', kv.first & 0xFFFFFF, (unsigned long long)kv.second); both("%s", "\n");
        both("%s", "esp2_saturations_by_dest:"); for (uint32_t k = 0; k < Esp2Core::kSatKeys; ++k) if (e.satDest[k]) both(" %s%03X x%llu", (k & 0x1000) ? "MAC>" : "ALU>", k & 0x3FF, (unsigned long long)e.satDest[k]); both("%s", "\n");
        both("%s", "esp2_unknown_spr:"); for (auto& kv : e.unknownSpr) both(" %s%03X x%llu", (kv.first & 0x1000) ? "W" : "R", kv.first & 0x3FF, (unsigned long long)kv.second); both("%s", "\n");
        both("voice: bad_channel_codes %llu, voice_port_clamped_words %llu\n", (unsigned long long)m.voice.badChannel, (unsigned long long)m.voicePortClips);
    }
    if (clip) std::fclose(clip);
    both("%s", "cpu_unmapped_accesses:");
    for (auto& kv : m.unmapped) both(" %08X/%c%d x%llu (first pc %06X)", std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), (unsigned long long)kv.second.count, kv.second.firstPc);
    both("%s", "\n");
    if (m.esp2.extLog) std::fclose(m.esp2.extLog);
    if (sum) std::fclose(sum);
    return 0;
}
