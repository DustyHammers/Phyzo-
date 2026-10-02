// native_image - writes the voice chip's native wave memory image from a FZWAVROM container.
//
// The original OS keeps the wave directory and every zone descriptor in its own image and programs the voice
// chip with absolute sample addresses taken from those tables. This module reads those tables from the user's
// OS image at build time (locations below are addresses from our own analysis, not copied content), places
// each zone's PCM at the address the OS will use, and reads everything back through the same tables.
//
// Rules (docs/NATIVE_IMAGE.md):
//  - Anchor: one-shots align our sample 0 with the OS start; every looping zone aligns our loop start with
//    the OS loop start.
//  - Length: our PCM must cover exactly the OS region [min(start, loop start), loop end), optionally followed
//    by 2 guard samples (EPS .img sources carry them) that land in otherwise unused memory.
//  - Overlapping OS regions (shared or nested zones) must receive identical samples.
//  - WAV-sourced one-shot zones are FITTED: resampled to the rate the OS plays them at their root key, then
//    cut or zero-padded to the OS span; the change is reported.
//  - Anything else is a mismatch. Strict mode (default) writes nothing and reports. --native-resolve applies
//    the documented resolution per mismatch class and reports every one.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include "fzrom/format.hpp"
#include "io.hpp"

namespace native {

using namespace fzrom;

// ---- Locations in the supported OS image (v1.10 decoded, loaded at 0x4000). Addresses only.
struct OsProfile {
    uint32_t loadAddress = 0x4000;
    uint32_t firstTable = 0x19A28, firstCount = 400;     // FIRST[wave number] -> slot, 0xFFFF none
    uint32_t checksumTable = 0x19D48;                     // CHECKSUM[slot], spec order; 0 ends a number group
    uint32_t zone0Table = 0x19DD8;                        // ZONE0[slot] -> first zone record
    uint32_t zoneRecords = 0x18F78, zoneRecordSize = 16;  // top key, mode, s16 pitch, start, loop start, loop end
    uint32_t maxSlots = 72;
    uint32_t bankBase = 0x20000000;                       // internal wave memory (bank 2)
    uint32_t imageSamples = 0x200000;                     // 2,097,152 samples (4 MB)
};

struct OsZone { uint32_t index; uint8_t top, mode; int16_t pitch; uint32_t st16, ls16, le16; };
struct OsSlot { uint32_t slot; uint16_t number, checksumSpec; uint32_t presetId; std::vector<OsZone> zones; };

struct OsTables {
    std::vector<uint8_t> img; OsProfile p; std::vector<OsSlot> slots; std::string error;
    uint8_t u8(uint32_t a) const { uint32_t o = a - p.loadAddress; if (o >= img.size()) throw std::runtime_error("OS image too short"); return img[o]; }
    uint16_t u16(uint32_t a) const { return (uint16_t)(u8(a) << 8 | u8(a + 1)); }
    uint32_t u32(uint32_t a) const { return (uint32_t)u16(a) << 16 | u16(a + 2); }
    OsZone zone(uint32_t i) const {
        uint32_t a = p.zoneRecords + i * p.zoneRecordSize;
        return OsZone{i, u8(a), u8(a + 1), (int16_t)u16(a + 2), u32(a + 4), u32(a + 8), u32(a + 12)};
    }
    bool load(const std::string &path) {
        img = io::readFile(path);
        // header: +0 end of body, +4 entry; trailer at end of body = 32-bit sum of big-endian words
        uint32_t bodyEnd = u32(p.loadAddress), sum = 0;
        if (bodyEnd <= p.loadAddress || bodyEnd + 4 - p.loadAddress > img.size()) { error = "OS header: body end out of range"; return false; }
        for (uint32_t a = p.loadAddress; a < bodyEnd; a += 2) sum += u16(a);
        if (sum != u32(bodyEnd)) { error = "OS trailer checksum mismatch (not the supported image?)"; return false; }
        std::set<uint32_t> seen;
        for (uint32_t wn = 0; wn < p.firstCount; ++wn) {
            uint32_t s = u16(p.firstTable + 2 * wn);
            if (s == 0xFFFF) continue;
            for (; s < p.maxSlots; ++s) {
                uint16_t c = u16(p.checksumTable + 2 * s);
                if (c == 0 || seen.count(s)) break;
                seen.insert(s);
                OsSlot sl; sl.slot = s; sl.number = (uint16_t)wn; sl.checksumSpec = c;
                sl.presetId = (uint32_t)wn << 16 | (uint16_t)((c >> 8) | (c << 8));
                for (uint32_t i = u16(p.zone0Table + 2 * s);; ++i) {
                    OsZone z = zone(i); sl.zones.push_back(z);
                    if (z.top == 127) break;
                    if (sl.zones.size() > 64) { error = "zone list without top key 127"; return false; }
                }
                slots.push_back(sl);
            }
        }
        return true;
    }
};

// ---- Band-limited resampler (Kaiser-windowed sinc), used only to fit WAV one-shots to the OS spans.
inline double bessel0(double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
inline std::vector<double> resample(const std::vector<int16_t> &in, double ratio /* out samples per in sample */) {
    const int half = 32; const double beta = 9.0, cutoff = std::min(1.0, ratio) * 0.97;
    size_t n = (size_t)std::llround(in.size() * ratio); std::vector<double> out(n);
    for (size_t j = 0; j < n; ++j) {
        double x = j / ratio; long c = (long)std::floor(x); double acc = 0, wsum = 0;
        for (long k = c - half + 1; k <= c + half; ++k) {
            double d = x - k; double sinc = d == 0 ? 1.0 : std::sin(M_PI * d * cutoff) / (M_PI * d * cutoff);
            double r = d / half; double w = std::fabs(r) >= 1 ? 0 : bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta);
            double v = (k >= 0 && k < (long)in.size()) ? in[k] : 0.0; acc += v * sinc * w * cutoff; wsum += sinc * w * cutoff;
        }
        out[j] = acc;  // unity-gain filter; DC gain ~1 by construction
    }
    return out;
}

struct Options { bool resolve = false; };

struct ZoneResult {
    std::string wave, cls, resolution, note; uint32_t osZone = 0; int romZone = -1; int lowKey = 0, topKey = 0;
    int ourLo = 0, ourHi = 0; int osMode = 0; int32_t osPitch = 0, ourPitch = 0;
    double osW = 0, osN = 0, ourW = 0, ourN = 0, osLoop = 0, ourLoop = 0, osLead = 0, ourLead = 0, osSpan = 0;
    int64_t base = 0; uint32_t ourLen = 0, placed = 0, regionStart = 0, regionEnd = 0; bool fitted = false;
    double fitRatio = 0, fitRateHz = 0; long fitLen = 0, fitPadOrCut = 0; int root = 0;
};

struct Result { bool ok = false; std::vector<ZoneResult> zones; std::vector<std::string> problems, gaps; std::vector<int16_t> image; std::string summary; };

inline const char *modeName(int m) { m &= 7; return m == 0 ? "one-shot" : m == 2 ? "plain loop" : m == 5 ? "stretch" : m == 6 ? "table" : m == 7 ? "table@start" : "?"; }

inline Result build(const Rom &rom, const OsTables &os, const Options &opt) {
    Result R; const OsProfile &P = os.p;
    std::vector<int32_t> img(P.imageSamples, 0); std::vector<int32_t> owner(P.imageSamples, -1);  // owner = result index
    std::map<uint32_t, const DirEntry *> byId; for (auto &d : rom.dir) byId[d.presetId] = &d;
    int mismatches = 0, conflicts = 0;

    auto place = [&](ZoneResult &zr, int idx, int64_t base, const std::vector<int32_t> &pcm, bool allowOverwrite) {
        uint32_t written = 0;
        for (size_t i = 0; i < pcm.size(); ++i) {
            int64_t a = base + (int64_t)i;
            if (a < 0 || a >= (int64_t)P.imageSamples) { R.problems.push_back(zr.wave + " OS zone " + std::to_string(zr.osZone) + ": data outside wave memory"); ++conflicts; return; }
            if (owner[a] >= 0 && img[a] != pcm[i]) {
                if (!allowOverwrite) { ++conflicts; zr.note += (zr.note.empty() ? "" : "; ") + std::string("conflict at sample ") + std::to_string(a) + " with OS zone " + std::to_string(R.zones[owner[a]].osZone); return; }
            }
            if (owner[a] < 0) owner[a] = idx;
            img[a] = pcm[i]; ++written;
        }
        zr.placed = written;
    };

    // ---- pass 1: map and classify every OS zone
    struct Job { int idx; int64_t base; std::vector<int32_t> pcm; int pri; };
    std::vector<Job> jobs;
    for (auto &sl : os.slots) {
        auto it = byId.find(sl.presetId);
        if (it == byId.end()) { R.problems.push_back("OS wave " + std::to_string(sl.number) + " slot " + std::to_string(sl.slot) + " has no wave in the ROM"); ++mismatches; continue; }
        const DirEntry &d = *it->second; int lo = 0;
        for (auto &oz : sl.zones) {
            ZoneResult zr; zr.wave = d.name; zr.osZone = oz.index; zr.lowKey = lo; zr.topKey = oz.top; zr.osMode = oz.mode & 7; zr.osPitch = oz.pitch;
            const Zone *z = rom.zoneFor(d, oz.top); int zi = z ? (int)(z - rom.zones.data()) : -1; zr.romZone = zi;
            double st = oz.st16 / 16.0, ls = oz.ls16 / 16.0, le = oz.le16 / 16.0; int m = oz.mode & 7;
            uint32_t rs = std::min(oz.st16, oz.ls16) >> 4, re = (oz.le16 + 15) >> 4;
            if (m == 5) rs = oz.ls16 >> 4;  // stretch: the cycle is the region; reads before it are the stretch window
            zr.regionStart = rs; zr.regionEnd = re; zr.osSpan = le - (m == 5 ? ls : std::min(st, ls));
            if (m == 5 && oz.st16 < oz.ls16) zr.note = "OS start lies " + std::to_string((int)(ls - st)) + " samples before the cycle (Open: what the OS uses it for)";
            if (m == 6 || m == 7) { zr.osW = st - ls; zr.osN = (le - ls) / zr.osW; }
            if (m == 5) zr.osLoop = le - ls;
            if (m == 2) { zr.osLoop = le - ls; zr.osLead = ls - st; }
            lo = oz.top + 1;
            if (!z) { zr.cls = "NO_ROM_ZONE"; R.problems.push_back(zr.wave + ": no ROM zone for key " + std::to_string(oz.top)); ++mismatches; R.zones.push_back(zr); continue; }
            zr.ourLo = z->lowKey; zr.ourHi = z->highKey; zr.ourPitch = z->zonePitch; zr.ourLen = z->pcmLength; zr.root = z->srcRoot;
            std::vector<int32_t> pcm(rom.pcm.begin() + z->pcmOffset, rom.pcm.begin() + z->pcmOffset + z->pcmLength);
            bool ourTable = z->sweepType == SWEEP_TABLE || z->sweepType == SWEEP_TABLE_NOTE_START;
            if (ourTable) { zr.ourW = z->frameSize; zr.ourN = z->frameCount; }
            if (z->sweepType == SWEEP_STRETCH || z->sweepType == SWEEP_LOOPSTARTX || z->loopType == LOOP_PLAIN || (z->loopType == LOOP_FORWARD && z->sweepType == SWEEP_NONE)) { zr.ourLoop = z->loopEnd - z->loopStart; zr.ourLead = z->loopStart - z->playStart; }
            int idx = (int)R.zones.size();

            // ---- WAV one-shots: fitted to the OS span
            if (z->sourceKind == SRC_WAV && m == 0) {
                zr.fitted = true; zr.cls = "FITTED";
                double rateOs = kOutputRate * std::pow(2.0, (oz.pitch + kUnitsPerSemitone * z->srcRoot - kPitchOffset) / (double)kUnitsPerOctave);
                double srcRate = z->sourceRateMilliHz / 1000.0; zr.fitRateHz = rateOs; zr.fitRatio = rateOs / srcRate;
                std::vector<int16_t> s16(rom.pcm.begin() + z->pcmOffset, rom.pcm.begin() + z->pcmOffset + z->pcmLength);
                auto rs2 = resample(s16, zr.fitRatio); zr.fitLen = (long)rs2.size();
                long span = (long)(le - st); zr.fitPadOrCut = span - zr.fitLen;
                std::vector<int32_t> out(span, 0);
                for (long i = 0; i < span && i < (long)rs2.size(); ++i) out[i] = std::clamp((int)std::lround(rs2[i]), -32768, 32767);
                jobs.push_back({idx, (int64_t)(oz.st16 >> 4), out, 0}); zr.base = oz.st16 >> 4;
                R.zones.push_back(zr); continue;
            }

            // ---- kind compatibility
            bool kindOk = (m == 6 || m == 7) ? ourTable
                        : m == 5 ? (z->sweepType == SWEEP_STRETCH || z->sweepType == SWEEP_LOOPSTARTX)
                        : m == 2 ? (z->loopType == LOOP_PLAIN || (z->loopType == LOOP_FORWARD && z->sweepType == SWEEP_NONE))
                        : m == 0 ? z->loopType == LOOP_ONESHOT : false;
            int64_t anchorOurs = z->loopType == LOOP_ONESHOT ? 0 : (int64_t)z->loopStart;
            int64_t anchorOs = z->loopType == LOOP_ONESHOT ? (oz.st16 >> 4) : (oz.ls16 >> 4);
            int64_t base = anchorOs - anchorOurs; zr.base = base;
            int64_t ourEnd = base + (int64_t)pcm.size();
            bool fracEnd = (oz.le16 & 15) != 0;

            if (!kindOk) {
                if (m == 2 && ourTable && z->frameSize == (uint32_t)std::lround(le - ls)) {
                    zr.cls = "NO_SOURCE_SPLIT"; ++mismatches;
                    zr.note = "OS plays a fixed single cycle here (mode 2, " + std::to_string((int)(le - ls)) + " samples); our zone sweeps a table over these keys";
                    if (opt.resolve) {  // Open: hardware content unknown; use our table's last frame
                        std::vector<int32_t> cyc(pcm.begin() + z->loopStart + (z->frameCount - 1) * z->frameSize, pcm.begin() + z->loopStart + z->frameCount * z->frameSize);
                        jobs.push_back({idx, (int64_t)(oz.st16 >> 4), cyc, 2}); zr.base = oz.st16 >> 4; zr.resolution = "filled with our table's last frame (Open: no source for this cycle)";
                    }
                } else if (m == 2 && z->sourceKind == SRC_GENERATED) {
                    zr.cls = "GENERATED_LOOP_DIFFERS"; ++mismatches;
                } else { zr.cls = std::string("KIND_MISMATCH os ") + modeName(m); ++mismatches; }
                if (zr.cls != "GENERATED_LOOP_DIFFERS") { R.zones.push_back(zr); continue; }
            }
            if (zr.cls == "GENERATED_LOOP_DIFFERS" || (m == 2 && z->sourceKind == SRC_GENERATED && std::fabs(zr.osLoop - zr.ourLoop) > 1e-9)) {
                if (zr.cls.empty()) { zr.cls = "GENERATED_LOOP_DIFFERS"; ++mismatches; }
                // The OS table fixes the loop length and pitch, so the native loop is our generated loop treated as one
                // period and resampled (periodic band-limited interpolation) to the OS loop length. Report the pitch
                // relation between the OS zone and our FZWAVROM zone at the same key.
                long L = (long)zr.ourLoop, Lo = (long)std::lround(zr.osLoop);
                double osIncPerKey = std::pow(2.0, (oz.pitch - z->zonePitch) / (double)kUnitsPerOctave);
                double cents = 1200.0 * std::log2(osIncPerKey * (double)L / (double)Lo);
                zr.note = "our generated loop: " + std::to_string(L) + " samples (one period) at zone pitch " + std::to_string(z->zonePitch) +
                          "; OS: " + std::to_string(Lo) + " samples at " + std::to_string(oz.pitch) + "; at the same key the OS plays " +
                          std::to_string(cents) + " cents relative to our FZWAVROM zone";
                if (opt.resolve) {
                    std::vector<int32_t> cyc(Lo);
                    for (long j = 0; j < Lo; ++j) {
                        double x = j * (double)L / (double)Lo, acc = 0;
                        for (long k = 0; k < L; ++k) { double dd = x - k; double s2 = std::sin(M_PI * dd);
                            double w = std::fabs(s2) < 1e-12 ? (std::fabs(std::remainder(dd, (double)L)) < 1e-9 ? 1.0 : 0.0)
                                                             : (L % 2 == 0 ? s2 / (L * std::tan(M_PI * dd / L)) : s2 / (L * std::sin(M_PI * dd / L)));
                            acc += pcm[z->loopStart + k] * w; }
                        cyc[j] = std::clamp((int)std::lround(acc), -32768, 32767);
                    }
                    jobs.push_back({idx, (int64_t)(oz.st16 >> 4), cyc, 2}); zr.base = oz.st16 >> 4;
                    zr.resolution = "regenerated: our generated loop as one period, periodically resampled to the OS loop length (pitch follows the OS zone)";
                }
                R.zones.push_back(zr); continue;
            }

            // ---- geometry checks
            std::vector<std::string> diff;
            if ((m == 6 || m == 7) && std::fabs(zr.osW - zr.ourW) > 1e-9) diff.push_back("W");
            if ((m == 2) && std::fabs(zr.osLead - zr.ourLead) > 1e-9) diff.push_back("lead-in");
            double ourLoopCmp = zr.ourLoop;
            if ((m == 2 || m == 5) && std::fabs(std::floor(zr.osLoop) - ourLoopCmp) > 1e-9) diff.push_back("loop length");
            int64_t rStart = (int64_t)rs, rEndExact = (int64_t)std::floor(le);
            int64_t extraAfter = ourEnd - rEndExact, extraBefore = rStart - base;
            if (!diff.empty()) { zr.cls = "GEOMETRY_MISMATCH (" + diff[0] + ")"; ++mismatches; R.zones.push_back(zr); continue; }
            if (extraBefore == 0 && extraAfter == 0 && !fracEnd) zr.cls = "EXACT";
            else if (extraBefore == 0 && extraAfter == 2 && !fracEnd) zr.cls = "EXACT+GUARD";
            else if (extraBefore == 0 && extraAfter == 0 && fracEnd) {
                zr.cls = "FRACTIONAL_END"; ++mismatches; zr.note = "OS loop end has a fraction; the sample at the integer loop end is played but not in our source";
                if (opt.resolve) { pcm.push_back(pcm[z->loopStart]); zr.resolution = "sample at the integer loop end = our cycle's first sample (wrap partner)"; }
            } else if (extraBefore > 0 && (m == 5)) {
                zr.cls = "LEAD_BEFORE_CYCLE"; ++mismatches;
                zr.note = std::to_string(extraBefore) + " source samples before the cycle; the OS stretch window reads back up to " + std::to_string((int)(4 * zr.osLoop)) + " samples before the loop end";
                if (opt.resolve) zr.resolution = "placed whole (fits unused memory before the OS region)";
            } else if (extraBefore == 0 && extraAfter > 2) {
                zr.cls = "EXTRA_AFTER_OS_REGION"; ++mismatches;
                zr.note = "our PCM runs " + std::to_string(extraAfter) + " samples past the OS loop end (OS frames " + std::to_string(zr.osN) + ", ours " + std::to_string(zr.ourN) + ")";
                if (opt.resolve) zr.resolution = "placed whole where memory is unused, else cut at the next zone";
            } else { zr.cls = "LENGTH_MISMATCH"; ++mismatches; zr.note = "before " + std::to_string(extraBefore) + " after " + std::to_string(extraAfter); }
            bool accept = zr.cls == "EXACT" || zr.cls == "EXACT+GUARD" || opt.resolve;
            if (accept && zr.cls != "LENGTH_MISMATCH") jobs.push_back({idx, base, pcm, zr.cls.rfind("EXACT", 0) == 0 ? 0 : 1});
            R.zones.push_back(zr);
        }
    }

    // ---- pass 2: place exact zones first, then resolved ones (which may not overwrite other zones' data)
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job &a, const Job &b) { return a.pri < b.pri; });
    for (auto &j : jobs) {
        ZoneResult &zr = R.zones[j.idx];
        if (j.pri == 0) { place(zr, j.idx, j.base, j.pcm, false); continue; }
        // resolved: write sample by sample; stop at memory owned by another zone (different data)
        uint32_t w = 0, cut = 0;
        for (size_t i = 0; i < j.pcm.size(); ++i) {
            int64_t a = j.base + (int64_t)i; if (a < 0 || a >= (int64_t)P.imageSamples) { ++cut; continue; }
            if (owner[a] >= 0 && owner[a] != j.idx && img[a] != j.pcm[i]) { ++cut; continue; }
            if (owner[a] < 0) owner[a] = j.idx; img[a] = j.pcm[i]; ++w;
        }
        zr.placed = w; if (cut) zr.resolution += "; " + std::to_string(cut) + " samples not written (memory owned by another zone)";
    }

    // ---- gaps and overlaps (by OS regions)
    std::vector<std::pair<uint32_t, uint32_t>> reg; for (auto &z : R.zones) reg.push_back({z.regionStart, z.regionEnd});
    std::sort(reg.begin(), reg.end()); reg.erase(std::unique(reg.begin(), reg.end()), reg.end());
    uint32_t cursor = 0;
    for (auto &r : reg) {
        if (r.first > cursor) R.gaps.push_back("gap " + std::to_string(cursor) + "-" + std::to_string(r.first - 1) + " (" + std::to_string(r.first - cursor) + " samples)");
        else if (r.first < cursor) R.gaps.push_back("overlap at " + std::to_string(r.first) + "-" + std::to_string(std::min(cursor, r.second) - 1));
        cursor = std::max(cursor, r.second);
    }
    if (cursor < P.imageSamples) R.gaps.push_back("gap " + std::to_string(cursor) + "-" + std::to_string(P.imageSamples - 1) + " (" + std::to_string(P.imageSamples - cursor) + " samples)");

    // ---- read back every non-fitted zone through the OS tables and compare with the ROM
    int readOk = 0, readTotal = 0;
    for (auto &zr : R.zones) {
        if (zr.fitted || zr.romZone < 0) continue;
        if (zr.cls.rfind("EXACT", 0) != 0 && zr.cls != "EXTRA_AFTER_OS_REGION" && zr.cls != "LEAD_BEFORE_CYCLE" && zr.cls != "FRACTIONAL_END") continue;
        const Zone &z = rom.zones[zr.romZone]; ++readTotal; uint32_t same = 0, n = z.pcmLength;
        for (uint32_t i = 0; i < n; ++i) { int64_t a = zr.base + i; if (a >= 0 && a < (int64_t)P.imageSamples && img[a] == rom.pcm[z.pcmOffset + i]) ++same; }
        bool full = same == n; if (full) ++readOk;
        zr.note += (zr.note.empty() ? "" : "; ") + std::string("read-back ") + std::to_string(same) + "/" + std::to_string(n) + (full ? " identical" : " DIFFERENT");
        if (!full && zr.cls.rfind("EXACT", 0) == 0) { R.problems.push_back(zr.wave + " OS zone " + std::to_string(zr.osZone) + ": read-back differs"); ++conflicts; }
    }

    R.image.resize(P.imageSamples); for (size_t i = 0; i < img.size(); ++i) R.image[i] = (int16_t)img[i];
    std::ostringstream s;
    s << "OS slots " << os.slots.size() << ", OS zone references " << R.zones.size() << ", mismatches " << mismatches << ", conflicts " << conflicts
      << ", read-back identical " << readOk << "/" << readTotal << " (non-fitted zones in exact or resolved classes)\n";
    R.summary = s.str();
    R.ok = conflicts == 0 && (mismatches == 0 || opt.resolve);
    if (mismatches && !opt.resolve) R.problems.push_back(std::to_string(mismatches) + " zone mismatches: refusing to write the native image (see native_zones.csv; --native-resolve applies the documented resolutions)");
    return R;
}

inline void writeReport(const Result &R, const std::string &dir) {
    std::ofstream c(dir + "/native_zones.csv");
    c << "wave,os_zone,keys_os,keys_ours,os_mode,class,os_pitch,our_pitch,pitch_diff_cents,os_W,our_W,os_N,our_N,os_loop,our_loop,os_lead,our_lead,"
         "os_region_start,os_region_end,base,our_len,placed,fit_rate_hz,fit_ratio,fit_ratio_cents,fit_len,os_span,fit_pad(+)_or_cut(-),root,resolution,note\n";
    for (auto &z : R.zones) {
        double dc = z.fitted ? 0.0 : (z.ourPitch - z.osPitch) * 100.0 / 256.0;
        c << '"' << z.wave << "\"," << z.osZone << "," << z.lowKey << "-" << z.topKey << "," << z.ourLo << "-" << z.ourHi << "," << modeName(z.osMode) << ",\"" << z.cls << "\","
          << z.osPitch << "," << z.ourPitch << "," << std::fixed << std::setprecision(2) << (z.fitted ? std::string("n/a") : std::to_string(dc)) << ","
          << z.osW << "," << z.ourW << "," << z.osN << "," << z.ourN << "," << z.osLoop << "," << z.ourLoop << "," << z.osLead << "," << z.ourLead << ","
          << z.regionStart << "," << z.regionEnd << "," << z.base << "," << z.ourLen << "," << z.placed << ",";
        if (z.fitted) c << std::setprecision(1) << z.fitRateHz << "," << std::setprecision(5) << z.fitRatio << "," << std::setprecision(1) << 1200 * std::log2(z.fitRatio) << "," << z.fitLen << "," << (long)z.osSpan << "," << z.fitPadOrCut << "," << z.root;
        else c << ",,,,,," ;
        c << ",\"" << z.resolution << "\",\"" << z.note << "\"\n";
    }
    std::ofstream g(dir + "/native_gaps.txt"); for (auto &x : R.gaps) g << x << "\n";
    std::ofstream p(dir + "/native_summary.txt"); p << R.summary; for (auto &x : R.problems) p << "PROBLEM: " << x << "\n";
}

}  // namespace native
