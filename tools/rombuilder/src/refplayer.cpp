// refplayer - minimal reference player for the Phyzo wave ROM.
// Plain linear interpolation, no filter, no envelope. Renders every wave at C4 (Start Index 0) and, for
// sweepable zones, a 128-step Start Index sweep (index 0 -> 127, or 127 -> 0 with --descending 1) using the
// Start Index path traced in the OS (routine 0x1AC28, see format.hpp):
//  - position p = Start Index*256 + (mod*amount)/128 + (offset-64)*256, clamped 0..127.99; table frame = (p>>8)*N/128;
//  - index changes are latched and take effect at the next loop wrap, never mid-cycle; bidirectional loops take
//    them at either reflection (loop end or loop start);
//  - TABLE_NOTE_START zones (native loop mode 7) choose their frame only at note start: a sweep retriggers a note per step.
// sweep_switches.csv logs where each latched change took effect.
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <utility>
#include "fzrom/format.hpp"
#include "io.hpp"

using namespace fzrom;

struct Ctl { int modValue = 0, modAmount = 0, offset = 64, semitones = 0; };  // semitones: oscillator Semitone Tune (pitch only; zone follows the played key)

struct Voice {
    const Rom &rom; const Zone &z; int key; Ctl ctl;
    double pos = 0, ws = 0, we = 0, inc = 1; int dir = 1; bool done = false;
    int pendingIndex = -1;           // requested Start Index, applied at the next loop wrap (never mid-cycle)
    long outCount = 0;               // output samples produced so far
    std::vector<std::pair<int, long>> switches;  // (Start Index, output sample at which the new window took effect)
    Voice(const Rom &r, const Zone &zz, int k, Ctl c) : rom(r), z(zz), key(k), ctl(c) {}
    int16_t at(long i) const { return (i >= 0 && i < (long)z.pcmLength) ? rom.pcm[z.pcmOffset + i] : 0; }  // virtual zeros outside
    void windowFor(int startIndex, double &s0, double &e0, double &in) const {
        int32_t p = indexPosition(startIndex, ctl.modValue, ctl.modAmount, ctl.offset);
        double s = 0;
        if (z.sweepType == SWEEP_TABLE || z.sweepType == SWEEP_TABLE_NOTE_START) {
            uint32_t f = tableFrame((uint32_t)(p >> 8), z.frameCount);
            if (f >= z.frameCount) f = z.frameCount ? z.frameCount - 1 : 0;   // clamp to the last frame
            s0 = z.loopStart + (double)f * z.frameSize; e0 = s0 + z.frameSize;
        } else if (z.sweepType == SWEEP_STRETCH) {
            s = stretchShiftP(p, key); e0 = z.loopEnd; s0 = e0 - (z.loopEnd - z.loopStart) * std::pow(2.0, s / kUnitsPerOctave);
        } else { s0 = z.loopStart; e0 = z.loopEnd; }
        in = increment(z.zonePitch, key, s + kUnitsPerSemitone * ctl.semitones);
    }
    bool swept() const { return z.sweepType == SWEEP_TABLE || z.sweepType == SWEEP_STRETCH || z.sweepType == SWEEP_TABLE_NOTE_START; }
    void start(int startIndex) {
        windowFor(startIndex, ws, we, inc); dir = 1; done = false; pendingIndex = -1;
        pos = swept() ? ws : z.playStart;
    }
    // Request a new Start Index. The new loop window is latched and takes effect when playback next reaches a loop
    // boundary; mode-7 zones ignore changes while the note sounds.
    void changeIndex(int startIndex) { if (z.sweepType != SWEEP_TABLE_NOTE_START) pendingIndex = startIndex; }
    void applyPending(bool atStart = false) {
        if (pendingIndex < 0) return;
        double os = ws, oe = we; windowFor(pendingIndex, ws, we, inc);
        double rel = atStart ? (pos - os) : (pos - os);
        pos = ws + rel * (we - ws) / (oe - os);
        switches.push_back({pendingIndex, outCount + 1}); pendingIndex = -1;
    }
    double next() {
        if (done) { ++outCount; return 0.0; }
        long i = (long)std::floor(pos); double fr = pos - i, len = we - ws;
        double a = at(i), b;
        long j = i + 1;                                   // interpolation partner = next address the chip would read
        if (z.loopType == LOOP_BIDIRECTIONAL) { b = at(j >= (long)std::ceil(we) ? i : j); }
        else if (z.loopType != LOOP_ONESHOT && pos >= ws && j >= we) {
            double s2 = ws, e2 = we, in2;                 // across the loop end the next read comes from the window that will be active
            if (pendingIndex >= 0) windowFor(pendingIndex, s2, e2, in2);
            b = at((long)std::floor(s2 + (j - we) + 1e-9));
        } else b = at(j);
        double v = a + (b - a) * fr;
        pos += dir * inc;
        if (z.loopType == LOOP_ONESHOT) { if (pos >= z.pcmLength) done = true; }
        else if (z.loopType == LOOP_BIDIRECTIONAL) {
            for (int g = 0; g < 4; ++g) {
                if (pos >= we) { pos = 2 * we - pos; dir = -1; applyPending(); }            // loop-end reflection
                else if (pos < ws) { pos = 2 * ws - pos; dir = 1; applyPending(true); }     // loop-start reflection
                else break;
            }
        } else {
            while (pos >= we && len > 0) { pos -= len; applyPending(); len = we - ws; }   // loop-end wrap
        }
        ++outCount;
        return v;
    }
};

static std::string safe(std::string s) { for (auto &c : s) if (!isalnum((unsigned char)c)) c = '_'; return s; }
static int16_t clip(double v) { long x = std::lround(v); return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: refplayer rom.bin outdir [--key 60] [--c4-seconds 2.0] [--step-seconds 0.25] [--descending 1]\n"
                     "       [--mod-value 0] [--mod-amount 0] [--offset 64] [--waves 1,2,42] [--keyseq first,last,step]\n"
                     "       [--keyseq-on 0.4] [--keyseq-off 0.1]\n"
                     "       [--notes notes.csv --notes-wave N --notes-out file.wav --gain 0.5]   (notes.csv: time_s,key,duration_s,semitones)\n";
        return 2;
    }
    std::string romPath = argv[1], out = argv[2]; int key = 60; double c4s = 2.0, steps = 0.25, ksOn = 0.4, ksOff = 0.1; bool descending = false;
    Ctl ctl; std::set<int> only; int ksFirst = -1, ksLast = -1, ksStep = 1, notesWave = -1; std::string notesCsv, notesOut; double gain = 0.5;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--key") key = std::stoi(v); else if (k == "--c4-seconds") c4s = std::stod(v); else if (k == "--step-seconds") steps = std::stod(v);
        else if (k == "--descending") descending = v == "1"; else if (k == "--mod-value") ctl.modValue = std::stoi(v);
        else if (k == "--mod-amount") ctl.modAmount = std::stoi(v); else if (k == "--offset") ctl.offset = std::stoi(v);
        else if (k == "--waves") { std::stringstream ss(v); std::string t; while (std::getline(ss, t, ',')) only.insert(std::stoi(t)); }
        else if (k == "--keyseq") { char c; std::stringstream ss(v); ss >> ksFirst >> c >> ksLast >> c >> ksStep; }
        else if (k == "--keyseq-on") ksOn = std::stod(v); else if (k == "--keyseq-off") ksOff = std::stod(v);
        else if (k == "--notes") notesCsv = v; else if (k == "--notes-wave") notesWave = std::stoi(v);
        else if (k == "--notes-out") notesOut = v; else if (k == "--gain") gain = std::stod(v);
        else { std::cerr << "unknown option " << k << "\n"; return 2; }
    }
    Rom rom;
    try { if (!loadRom(io::readFile(romPath), rom)) { std::cerr << "refplayer: " << rom.error << "\n"; return 1; } }
    catch (const std::exception &e) { std::cerr << "refplayer: " << e.what() << "\n"; return 1; }
    if (!notesCsv.empty()) {  // polyphonic note list for one wave, mixed (one voice per note; one-shots play to their end)
        const DirEntry *d = nullptr; for (auto &e : rom.dir) if (e.waveSelect == notesWave) d = &e;
        if (!d || !d->zoneCount) { std::cerr << "refplayer: wave " << notesWave << " has no zones\n"; return 1; }
        auto rows = io::readCsv(notesCsv); std::vector<double> mix; std::ofstream nl(out + "/notes_log.csv");
        nl << "time_s,key,semitones,zone,low_key,high_key,zone_pitch,flags\n";
        for (auto &r : rows) {
            double t = std::stod(r.at("time_s")), dur = r.count("duration_s") && !r.at("duration_s").empty() ? std::stod(r.at("duration_s")) : 0;
            int k = std::stoi(r.at("key")); Ctl c = ctl; c.semitones = r.count("semitones") && !r.at("semitones").empty() ? std::stoi(r.at("semitones")) : 0;
            const Zone *zk = rom.zoneFor(*d, k); if (!zk) continue;
            Voice q(rom, *zk, k, c); q.start(0);
            size_t s0 = (size_t)std::lround(t * kOutputRate), n = zk->loopType == LOOP_ONESHOT ? (size_t)(zk->pcmLength / q.inc) + 2 : (size_t)std::lround(dur * kOutputRate);
            if (mix.size() < s0 + n) mix.resize(s0 + n, 0.0);
            for (size_t i = 0; i < n; ++i) mix[s0 + i] += q.next();
            nl << t << "," << k << "," << c.semitones << "," << (int)zk->zoneIndex << "," << (int)zk->lowKey << "," << (int)zk->highKey << "," << zk->zonePitch << "," << zk->flags << "\n";
        }
        std::vector<int16_t> o(mix.size()); for (size_t i = 0; i < mix.size(); ++i) o[i] = clip(mix[i] * gain);
        io::writeWav16(out + "/" + notesOut, o, (uint32_t)kOutputRate);
        std::cout << "rendered " << rows.size() << " notes of wave " << notesWave << " to " << notesOut << "\n";
        return 0;
    }
    std::ofstream log(out + "/render_log.csv");
    log << "wave_select,name,zone,low_key,high_key,loop_type,sweep_type,zone_pitch,increment,period_samples,expected_loop_hz,frame_size,frame_count,c4_file,sweep_file\n";
    std::ofstream swlog(out + "/sweep_switches.csv");
    swlog << "sweep_file,index,requested_sample,applied_sample\n";
    int rendered = 0, swept = 0, seqs = 0;
    for (auto &d : rom.dir) {
        if (!d.zoneCount || (!only.empty() && !only.count(d.waveSelect))) continue;
        const Zone *z = rom.zoneFor(d, key); if (!z) continue;
        std::string base = (d.waveSelect < 10 ? "0" : "") + std::to_string(d.waveSelect) + "_" + safe(d.name);
        Voice v(rom, *z, key, ctl); v.start(0);
        double period = (v.we - v.ws) * (z->loopType == LOOP_BIDIRECTIONAL ? 2 : 1);
        double hz = period > 0 && z->loopType != LOOP_ONESHOT ? v.inc * kOutputRate / period : 0.0;  // one-shots have no loop rate
        std::vector<int16_t> buf((size_t)(c4s * kOutputRate)); for (auto &s : buf) s = clip(v.next());
        io::writeWav16(out + "/" + base + "_C4.wav", buf, (uint32_t)kOutputRate); ++rendered;
        std::string sweepFile;
        bool sweepable = ((z->sweepType == SWEEP_TABLE || z->sweepType == SWEEP_TABLE_NOTE_START) && z->frameCount > 1) ||
                         (z->sweepType == SWEEP_STRETCH && stretchMax(key) > 0);
        if (sweepable) {
            size_t n = (size_t)std::lround(steps * kOutputRate); std::vector<int16_t> sb;
            Voice s(rom, *z, key, ctl); s.start(descending ? 127 : 0);
            for (int st = 0; st < 128; ++st) {
                int idx = descending ? 127 - st : st;
                if (st) { s.changeIndex(idx); if (z->sweepType == SWEEP_TABLE_NOTE_START) s.start(idx); }  // mode 7 ignores the held change; new note per step
                for (size_t i = 0; i < n; ++i) sb.push_back(clip(s.next()));
            }
            sweepFile = base + (descending ? "_sweepdown.wav" : "_sweep.wav"); io::writeWav16(out + "/" + sweepFile, sb, (uint32_t)kOutputRate); ++swept;
            for (auto &sw : s.switches) swlog << sweepFile << "," << sw.first << "," << (long)(descending ? 127 - sw.first : sw.first) * (long)n << "," << sw.second << "\n";
        }
        if (ksFirst >= 0) {  // note sequence across the keyboard (each note: --keyseq-on seconds, then --keyseq-off silence)
            std::vector<int16_t> qb;
            for (int k = ksFirst; k <= ksLast; k += ksStep) {
                const Zone *zk = rom.zoneFor(d, k); size_t on = (size_t)std::lround(ksOn * kOutputRate), off = (size_t)std::lround(ksOff * kOutputRate);
                if (!zk) { qb.insert(qb.end(), on + off, 0); continue; }
                Voice q(rom, *zk, k, ctl); q.start(0);
                for (size_t i = 0; i < on; ++i) qb.push_back(clip(q.next()));
                qb.insert(qb.end(), off, 0);
            }
            io::writeWav16(out + "/" + base + "_keyseq.wav", qb, (uint32_t)kOutputRate); ++seqs;
        }
        log << d.waveSelect << ",\"" << d.name << "\"," << (int)z->zoneIndex << "," << (int)z->lowKey << "," << (int)z->highKey << "," << (int)z->loopType << ","
            << (int)z->sweepType << "," << z->zonePitch << "," << std::setprecision(10) << v.inc << "," << period << "," << hz << "," << z->frameSize << ","
            << z->frameCount << "," << base << "_C4.wav," << sweepFile << "\n";
    }
    std::cout << "rendered " << rendered << " C4 files, " << swept << " sweeps" << (seqs ? ", " + std::to_string(seqs) + " key sequences" : std::string()) << " at key " << key << "\n";
    return 0;
}
