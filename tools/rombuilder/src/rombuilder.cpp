// rombuilder - builds the Phyzo wave ROM (.bin) from user-supplied inputs.
// Everything wave-specific comes from the input files: manifest CSV (IDs, names), sources CSV (which file feeds
// which zone), EPS .img sets, zone JSON files, the EXP-3 text listing, WAVs. No manufacturer data lives in this code.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include "eps_img.hpp"
#include "exp3_text.hpp"
#include "fzrom/format.hpp"
#include "io.hpp"
#include "native_image.hpp"

using namespace fzrom;

struct Args { std::string manifest, sources, dataRoot = ".", exp3Text, out, reportDir, baseRom, nativeOut, osImage; int romMajor = 0, romMinor = 1, romPatch = 0; bool nativeResolve = false; std::string buildDate; };

struct BuiltZone { Zone z; std::vector<int16_t> pcm; std::string source, note; int epsRoot = -1, epsTune = 0; uint32_t epsPcmOffset = 0; };
struct BuiltWave {
    DirEntry d; std::string name, sourceClass, note; bool inPresets = false;
    std::vector<BuiltZone> zones;
};

static uint16_t hex16(const std::string &s) { return (uint16_t)std::stoul(s, nullptr, 16); }
static uint32_t hex32(const std::string &s) { return (uint32_t)std::stoul(s, nullptr, 16); }
static uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static std::string upper(std::string s) { for (auto &c : s) c = (char)toupper((unsigned char)c); return s; }
static int toInt(const std::string &s, int def) { return s.empty() ? def : std::stoi(s); }
[[noreturn]] static void die(const std::string &m) { throw std::runtime_error(m); }

static int32_t pitchFromRootTune(int root, int tune, double tuneUnitsPerSemitone, double rateHz) {
    double z = kPitchOffset - kUnitsPerSemitone * root + (kUnitsPerSemitone / tuneUnitsPerSemitone) * tune +
               kUnitsPerOctave * std::log2(rateHz / kOutputRate);
    return (int32_t)std::lround(z);
}

int main(int argc, char **argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--manifest") a.manifest = v; else if (k == "--sources") a.sources = v; else if (k == "--data-root") a.dataRoot = v;
        else if (k == "--exp3-text") a.exp3Text = v; else if (k == "--out") a.out = v; else if (k == "--report-dir") a.reportDir = v;
        else if (k == "--rom-version") { int p3[3] = {0, 0, 0}; std::stringstream vs(v); std::string part; int n = 0; while (n < 3 && std::getline(vs, part, '.')) p3[n++] = std::stoi(part); a.romMajor = p3[0]; a.romMinor = p3[1]; a.romPatch = p3[2]; }
        else if (k == "--base-rom") a.baseRom = v; else if (k == "--native-out") a.nativeOut = v; else if (k == "--os-image") a.osImage = v;
        else if (k == "--native-resolve") a.nativeResolve = v == "1" || v == "yes";
        else if (k == "--build-date") a.buildDate = v;
        else { std::cerr << "unknown option " << k << "\n"; return 2; }
    }
    if ((a.manifest.empty() && a.baseRom.empty()) || a.sources.empty() || a.out.empty() || (!a.nativeOut.empty() && a.osImage.empty())) {
        std::cerr << "usage: rombuilder (--manifest M.csv | --base-rom old.bin) --sources S.csv --data-root DIR [--exp3-text T.txt] --out rom.bin\n"
                     "                  [--report-dir DIR] [--rom-version 0.3.1] [--build-date ISO]\n"
                     "                  [--native-out native.bin --os-image OS.bin [--native-resolve 1]]\n"; return 2;
    }
    if (a.buildDate.empty()) { std::time_t t = std::time(nullptr); char b[32]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t)); a.buildDate = b; }
    std::ostringstream log;
    try {
        // ---------- base ROM (optional): every wave starts as an exact copy; waves named in the sources CSV are rebuilt
        std::map<int, BuiltWave> waves; std::set<uint32_t> ids; Rom base; std::set<int> replaced;
        if (!a.baseRom.empty()) {
            if (!loadRom(io::readFile(a.baseRom), base)) die("base ROM: " + base.error);
            for (auto &d : base.dir) {
                BuiltWave w; w.d = d; w.name = std::string(d.name, strnlen(d.name, 24)); w.inPresets = (d.flags & DIR_IN_FACTORY_PRESETS) != 0;
                w.d.flags &= (uint16_t)~DIR_HAS_ZONES;
                for (uint32_t k = 0; k < d.zoneCount; ++k) {
                    BuiltZone bz; bz.z = base.zones[d.firstZone + k]; bz.z.flags &= ~(uint32_t)ZONE_PCM_SHARED;
                    bz.pcm.assign(base.pcm.begin() + bz.z.pcmOffset, base.pcm.begin() + bz.z.pcmOffset + bz.z.pcmLength);
                    bz.source = "base ROM " + std::to_string(base.h.romMajor) + "." + std::to_string(base.h.romMinor) + " zone " + std::to_string(d.firstZone + k);
                    w.zones.push_back(std::move(bz));
                }
                ids.insert(d.presetId); waves[d.waveSelect] = std::move(w);
            }
            log << "base ROM " << a.baseRom << ": " << waves.size() << " waves, " << base.zones.size() << " zones (ROM " << base.h.romMajor << "." << base.h.romMinor << ")\n";
        }
        // ---------- manifest
        std::vector<io::CsvRow> man; if (!a.manifest.empty()) man = io::readCsv(a.manifest);
        if (!a.baseRom.empty() && !man.empty()) die("give either --manifest or --base-rom, not both");
        for (auto &r : man) {
            BuiltWave w; int sel = std::stoi(r.at("wave_select"));
            w.d.waveSelect = (uint16_t)sel; w.d.number = hex16(r.at("number")); w.d.checksumSpec = hex16(r.at("checksum_spec_order"));
            w.d.presetId = hex32(r.at("preset_id_as_stored")); w.name = r.at("name"); w.sourceClass = r.at("source");
            w.inPresets = upper(r.at("in_factory_presets")) == "YES"; w.note = r.count("notes") ? r.at("notes") : "";
            if (w.d.presetId != (((uint32_t)w.d.number << 16) | bswap16(w.d.checksumSpec)))
                die("manifest row " + std::to_string(sel) + ": preset_id_as_stored is not number + byte-swapped checksum_spec_order");
            if (!ids.insert(w.d.presetId).second) die("duplicate preset ID in manifest");
            if (waves.count(sel)) die("duplicate wave_select " + std::to_string(sel));
            std::string sc = upper(w.sourceClass);
            // Source label -> class: "...expansion..." -> expansion, "...internal..." -> internal ROM, any other
            // non-empty label -> native-only (the manifest labels are user data and are not interpreted further).
            w.d.waveClass = sc.find("EXPANSION") != std::string::npos ? CLASS_EXPANSION : sc.find("INTERNAL") != std::string::npos ? CLASS_INTERNAL : !sc.empty() ? CLASS_NATIVE_ONLY : CLASS_UNKNOWN;
            if (w.name.size() > 23) die("name too long: " + w.name);
            std::strncpy(w.d.name, w.name.c_str(), 23);
            waves[sel] = w;
        }
        if (!man.empty()) log << "manifest: " << waves.size() << " waves, all preset IDs = number + byte-swapped spec checksum\n";

        std::map<std::string, std::vector<exp3::Sample>> text;
        if (!a.exp3Text.empty()) text = exp3::parse(a.exp3Text);

        // ---------- sources
        auto src = io::readCsv(a.sources);
        for (auto &r : src) {
            int sel = std::stoi(r.at("wave_select"));
            if (!waves.count(sel)) die("sources: wave_select " + std::to_string(sel) + " not in manifest");
            BuiltWave &w = waves[sel];
            if (upper(r.at("name")) != upper(w.name)) die("sources: name mismatch for wave " + std::to_string(sel));
            if (!a.baseRom.empty() && !replaced.count(sel)) { w.zones.clear(); replaced.insert(sel); log << "rebuilding wave " << sel << " " << w.name << " from sources\n"; }
            std::string kind = r.at("kind"); auto prm = io::params(r.at("params"));
            std::string path = r.at("source").empty() ? "" : a.dataRoot + "/" + r.at("source");
            if (kind == "none") { w.d.flags |= DIR_SOURCE_PENDING; if (!r.at("note").empty()) w.note = r.at("note"); continue; }
            if (kind == "exp3") {
                auto it = text.find(r.at("text_wave")); if (it == text.end()) die("EXP-3 text has no wave '" + r.at("text_wave") + "'");
                int n = std::stoi(r.at("text_sample")); const exp3::Sample *s = nullptr;
                for (auto &x : it->second) if (x.n == n) s = &x;
                if (!s) die("EXP-3 text: no sample " + std::to_string(n) + " in " + it->first);
                auto ws = eps::readImg(path);
                std::string tag = r.at("source");
                if ((long)ws.sampleEnd != s->length) die(tag + ": sample length " + std::to_string(ws.sampleEnd) + " != text " + std::to_string(s->length));
                if ((long)ws.loopStart != s->loopStart || (long)ws.loopEnd != s->loopEnd) die(tag + ": loop points differ from text");
                if ((long)ws.sampleStart != s->playStart) die(tag + ": play start differs from text");
                BuiltZone bz; Zone &z = bz.z;
                double rate = std::stod(prm.at("rate_hz")), tu = std::stod(prm.at("tune_units"));
                z.zonePitch = pitchFromRootTune(s->root, s->tune, tu, rate);
                z.sourceRateMilliHz = (uint32_t)std::lround(rate * 1000.0);
                z.lowKey = (uint8_t)toInt(r.at("key_lo_override"), s->low); z.highKey = (uint8_t)toInt(r.at("key_hi_override"), s->high);
                if (z.lowKey != s->low || z.highKey != s->high) z.flags |= ZONE_KEY_OVERRIDE;
                z.sourceKind = SRC_EPS_IMG; z.playStart = (uint32_t)s->playStart; z.loopStart = (uint32_t)s->loopStart; z.loopEnd = (uint32_t)s->loopEnd;
                z.frameSize = z.loopEnd - z.loopStart; z.frameCount = (uint32_t)s->frames; z.srcRoot = (int16_t)s->root; z.srcTune = (int16_t)s->tune;
                if (s->mode == "TranswaveForward") { z.loopType = LOOP_FORWARD; z.sweepType = SWEEP_TABLE; }
                else if (s->mode == "TranswaveBidirectional") { z.loopType = LOOP_BIDIRECTIONAL; z.sweepType = SWEEP_TABLE; }
                else if (s->mode == "LoopStartX") { z.loopType = LOOP_FORWARD; z.sweepType = SWEEP_LOOPSTARTX; z.frameCount = 1; }
                else die(tag + ": unsupported text loop mode " + s->mode);
                if (z.sweepType == SWEEP_TABLE && (uint64_t)z.loopStart + (uint64_t)z.frameSize * z.frameCount > ws.sampleEnd) die(tag + ": frames exceed sample length");
                bz.pcm = std::move(ws.pcm); bz.source = tag; bz.epsRoot = ws.rootKey; bz.epsTune = ws.fineTune; bz.epsPcmOffset = ws.pcmObjectOffset;
                bz.note = r.at("note"); z.zoneIndex = (uint8_t)n;
                w.zones.push_back(std::move(bz));
            } else if (kind == "zonefile" || kind == "mr") {
                io::Json j = io::readJson(path);
                if ((uint16_t)j["directory_number"].i() != w.d.number || (uint16_t)j["directory_checksum"].i() != w.d.checksumSpec)
                    die(r.at("source") + ": directory number/checksum differ from manifest");
                if (upper(j["wave"].s) != upper(w.name)) die(r.at("source") + ": wave name differs from manifest");
                double rate = j["sample_rate_hz"].n;
                for (auto &zj : j["zones"].a) {
                    BuiltZone bz; Zone &z = bz.z;
                    for (auto &v : zj["audio_int16"].a) { long long x = v.i(); if (x < -32768 || x > 32767) die("zone-file audio out of range"); bz.pcm.push_back((int16_t)x); }
                    long long st = zj["start"].i(), ls = zj["loop_start"].i(), le = zj["loop_end"].i(); int mode = (int)zj["loop_mode"].i();
                    z.zoneIndex = (uint8_t)zj["index"].i(); z.lowKey = (uint8_t)zj["low_key"].i(); z.highKey = (uint8_t)zj["top_key"].i();
                    z.zonePitch = (int32_t)zj["pitch_F"].i(); z.sourceRateMilliHz = (uint32_t)std::lround(rate * 1000.0); z.sourceKind = SRC_MR_ZONE;
                    size_t n = bz.pcm.size();
                    if (mode == 6 || mode == 7) {
                        uint32_t W = (uint32_t)zj["frame_size"].i(), N = (uint32_t)zj["frames"].i();
                        if (n != (size_t)(le - ls) || n != (size_t)W * N || W != (uint32_t)(st - ls)) die(r.at("source") + ": mode-6/7 zone inconsistent");
                        z.loopType = LOOP_FORWARD; z.sweepType = mode == 7 ? SWEEP_TABLE_NOTE_START : SWEEP_TABLE;
                        z.loopStart = 0; z.loopEnd = (uint32_t)n; z.frameSize = W; z.frameCount = N; z.playStart = W;
                    } else if (mode == 5) {
                        if (n != (size_t)(le - ls) || n != (size_t)zj["cycle_length"].i()) die(r.at("source") + ": mode-5 zone inconsistent");
                        z.loopType = LOOP_FORWARD; z.sweepType = SWEEP_STRETCH; z.loopStart = 0; z.loopEnd = (uint32_t)n; z.frameSize = (uint32_t)n; z.frameCount = 1;
                        z.playStart = 0; z.flags |= ZONE_VIRTUAL_ZEROS_BEFORE;
                    } else if (mode == 2) {
                        if (n != (size_t)(le - st) || ls < st) die(r.at("source") + ": mode-2 zone inconsistent");
                        z.loopType = LOOP_PLAIN; z.sweepType = SWEEP_NONE; z.playStart = 0; z.loopStart = (uint32_t)(ls - st); z.loopEnd = (uint32_t)n; z.frameSize = 0; z.frameCount = 0;
                    } else die(r.at("source") + ": unsupported zone-file loop mode " + std::to_string(mode));
                    bz.source = r.at("source") + " z" + std::to_string(z.zoneIndex);
                    w.zones.push_back(std::move(bz));
                }
            } else if (kind == "sine") {
                int L = std::stoi(prm.at("cycle")); double peak = std::stod(prm.at("peak")), hz = std::stod(prm.at("hz_at_c4"));
                BuiltZone bz; Zone &z = bz.z;
                for (int i = 0; i < L; ++i) bz.pcm.push_back((int16_t)std::lround(peak * std::sin(2.0 * M_PI * i / L)));
                z.zonePitch = (int32_t)std::lround(kUnitsPerOctave * std::log2(hz * L / kOutputRate) - kUnitsPerSemitone * 60 + kPitchOffset);
                z.sourceRateMilliHz = (uint32_t)std::lround(kOutputRate * 1000); z.lowKey = 0; z.highKey = 127; z.zoneIndex = 1;
                z.loopType = LOOP_FORWARD; z.sweepType = SWEEP_NONE; z.loopStart = 0; z.loopEnd = (uint32_t)L; z.frameSize = (uint32_t)L; z.frameCount = 1;
                z.sourceKind = SRC_GENERATED; bz.source = "generated sine " + r.at("params"); bz.note = r.at("note");
                w.zones.push_back(std::move(bz));
            } else if (kind == "wav") {
                auto wv = io::readWav(path); BuiltZone bz; Zone &z = bz.z;
                double rate = prm.count("rate_hz") ? std::stod(prm.at("rate_hz")) : wv.rate; int root = std::stoi(prm.at("root"));
                z.zonePitch = pitchFromRootTune(root, prm.count("tune") ? std::stoi(prm.at("tune")) : 0, 64, rate);
                z.sourceRateMilliHz = (uint32_t)std::lround(rate * 1000); z.lowKey = (uint8_t)std::stoi(r.at("key_lo_override")); z.highKey = (uint8_t)std::stoi(r.at("key_hi_override"));
                z.loopType = LOOP_ONESHOT; z.sweepType = SWEEP_NONE; z.loopStart = z.loopEnd = (uint32_t)wv.samples.size(); z.sourceKind = SRC_WAV; z.srcRoot = (int16_t)root;
                z.zoneIndex = (uint8_t)(w.zones.size() + 1); bz.pcm = wv.samples; bz.source = r.at("source"); bz.note = r.at("note");
                if (prm.count("confidence") && prm.at("confidence") == "low") z.flags |= ZONE_LOW_CONFIDENCE;
                if (z.lowKey > z.highKey) die(r.at("source") + ": inverted key range");
                w.zones.push_back(std::move(bz));
            } else die("unknown source kind " + kind);
        }

        // ---------- per-wave checks: zone order, key coverage 0..127 without gaps or overlaps
        for (auto &[sel, w] : waves) {
            std::sort(w.zones.begin(), w.zones.end(), [](const BuiltZone &x, const BuiltZone &y) { return x.z.lowKey < y.z.lowKey; });
            if (w.zones.empty()) { if (!(w.d.flags & DIR_SOURCE_PENDING)) die("wave " + w.name + " has no sources and is not marked pending"); continue; }
            int expect = 0;
            for (auto &bz : w.zones) {
                if (bz.z.lowKey != expect) die("wave " + w.name + ": key gap/overlap at key " + std::to_string(expect));
                if (bz.z.highKey < bz.z.lowKey) die("wave " + w.name + ": inverted key range");
                expect = bz.z.highKey + 1; bz.z.waveSelect = (uint16_t)sel;
                bz.z.pcmCrc32 = crc32Samples(bz.pcm.data(), bz.pcm.size());
            }
            if (expect != 128) die("wave " + w.name + ": zones end at key " + std::to_string(expect - 1));
            w.d.flags |= DIR_HAS_ZONES;
        }
        for (auto &[sel, w] : waves) if (w.inPresets) w.d.flags |= DIR_IN_FACTORY_PRESETS;

        // ---------- PCM pool (exact/prefix sharing keeps every zone byte-exact)
        std::vector<int16_t> pool; std::vector<Zone> zones; std::vector<const BuiltZone *> zsrc;
        for (auto &[sel, w] : waves) {
            w.d.firstZone = (uint32_t)zones.size(); w.d.zoneCount = (uint16_t)w.zones.size();
            for (auto &bz : w.zones) {
                Zone z = bz.z; bool shared = false;
                for (size_t k = 0; k < zones.size() && !shared; ++k)
                    if (zones[k].pcmLength >= bz.pcm.size() && std::equal(bz.pcm.begin(), bz.pcm.end(), pool.begin() + zones[k].pcmOffset)) {
                        z.pcmOffset = zones[k].pcmOffset; shared = true; z.flags |= ZONE_PCM_SHARED; }
                if (!shared) { while (pool.size() % 8) pool.push_back(0); z.pcmOffset = (uint32_t)pool.size(); pool.insert(pool.end(), bz.pcm.begin(), bz.pcm.end()); }
                z.pcmLength = (uint32_t)bz.pcm.size();
                zones.push_back(z); zsrc.push_back(&bz);
            }
        }

        // ---------- serialise
        Header h; h.romMajor = (uint16_t)a.romMajor; h.romMinor = (uint16_t)a.romMinor; h.romPatch = (uint16_t)a.romPatch; std::strncpy(h.buildDate, a.buildDate.c_str(), 23);
        h.waveCount = (uint32_t)waves.size(); h.dirOffset = kHeaderSize;
        h.zoneCount = (uint32_t)zones.size(); h.zoneOffset = ((h.dirOffset + h.waveCount * kDirEntrySize + 15) / 16) * 16;
        h.pcmOffset = ((h.zoneOffset + h.zoneCount * kZoneSize + 15) / 16) * 16; h.pcmSamples = (uint32_t)pool.size();
        h.fileSize = h.pcmOffset + 2 * h.pcmSamples;
        Writer w; writeHeader(w, h);
        for (auto &[sel, bw] : waves) writeDir(w, bw.d);
        w.pad(16); for (auto &z : zones) writeZone(w, z);
        w.pad(16); for (int16_t s : pool) w.s16(s);
        if (w.b.size() != h.fileSize) die("internal: size mismatch");
        uint32_t crc = crc32(w.b.data(), w.b.size());
        for (int i = 0; i < 4; ++i) w.b[kCrcOffset + i] = (crc >> (8 * i)) & 0xFF;
        io::writeFile(a.out, w.b);

        // ---------- verify from disk
        Rom rom; if (!loadRom(io::readFile(a.out), rom)) die("verification: " + rom.error);
        size_t exact = 0;
        for (size_t k = 0; k < zones.size(); ++k) {
            const Zone &z = rom.zones[k]; const auto &p = zsrc[k]->pcm;
            if (z.pcmLength != p.size() || !std::equal(p.begin(), p.end(), rom.pcm.begin() + z.pcmOffset)) die("verification: PCM differs for zone " + std::to_string(k));
            if (crc32Samples(&rom.pcm[z.pcmOffset], z.pcmLength) != z.pcmCrc32) die("verification: zone CRC differs " + std::to_string(k));
            ++exact;
        }
        for (auto &[sel, bw] : waves) {
            const DirEntry *d = rom.find(bw.d.number, (uint16_t)(bw.d.presetId & 0xFFFF));
            if (!d || d->waveSelect != sel) die("verification: wave " + bw.name + " not found by preset ID");
            if (d->zoneCount) { int expect = 0; for (int i = 0; i < d->zoneCount; ++i) { auto &z = rom.zones[d->firstZone + i]; if (z.lowKey != expect) die("verification: coverage"); expect = z.highKey + 1; } if (expect != 128) die("verification: coverage"); }
        }
        if (!a.baseRom.empty()) {
            size_t same = 0, total = 0, rec = 0;
            for (auto &bd : base.dir) {
                if (replaced.count(bd.waveSelect)) continue;
                const DirEntry *nd = rom.find(bd.number, (uint16_t)(bd.presetId & 0xFFFF));
                if (!nd || nd->zoneCount != bd.zoneCount) die("base check: wave " + std::to_string(bd.waveSelect) + " changed");
                for (uint32_t k = 0; k < bd.zoneCount; ++k) {
                    Zone o = base.zones[bd.firstZone + k], n = rom.zones[nd->firstZone + k]; ++total;
                    if (!std::equal(base.pcm.begin() + o.pcmOffset, base.pcm.begin() + o.pcmOffset + o.pcmLength, rom.pcm.begin() + n.pcmOffset) || o.pcmLength != n.pcmLength)
                        die("base check: PCM differs, wave " + std::to_string(bd.waveSelect) + " zone " + std::to_string(k));
                    ++same;
                    Writer wa, wb; Zone o2 = o, n2 = n; o2.pcmOffset = n2.pcmOffset = 0; o2.flags &= ~(uint32_t)ZONE_PCM_SHARED; n2.flags &= ~(uint32_t)ZONE_PCM_SHARED;
                    writeZone(wa, o2); writeZone(wb, n2); if (wa.b == wb.b && o.pcmOffset == n.pcmOffset && o.flags == n.flags) ++rec;
                }
            }
            log << "base check: " << same << "/" << total << " kept zones PCM byte-identical to the base ROM; " << rec << "/" << total
                << " zone records byte-identical including PCM offset and flags\n";
        }
        log << "written " << a.out << ": " << h.fileSize << " bytes, " << h.waveCount << " waves, " << h.zoneCount << " zones, "
            << h.pcmSamples << " PCM samples, CRC32 " << std::hex << std::setw(8) << std::setfill('0') << crc << std::dec << "\n";
        log << "verified from disk: CRC32 OK, " << exact << "/" << zones.size() << " zones byte-identical to source, all " << waves.size()
            << " manifest waves resolve by preset ID, every populated wave covers keys 0-127 without gaps or overlaps\n";

        // ---------- reports
        if (!a.reportDir.empty()) {
            std::ofstream zc(a.reportDir + "/zones.csv");
            zc << "wave_select,name,zone,low_key,high_key,loop_type,sweep_type,zone_pitch,src_root,src_tune,eps_root,eps_tune,pcm_offset,pcm_length,"
                  "play_start,loop_start,loop_end,frame_size,frame_count,pcm_crc32,flags,source,note\n";
            for (size_t k = 0; k < zones.size(); ++k) {
                auto &z = zones[k]; auto *b = zsrc[k];
                zc << z.waveSelect << ",\"" << waves[z.waveSelect].name << "\"," << (int)z.zoneIndex << "," << (int)z.lowKey << "," << (int)z.highKey << ","
                   << (int)z.loopType << "," << (int)z.sweepType << "," << z.zonePitch << "," << z.srcRoot << "," << z.srcTune << ","
                   << (b->epsRoot < 0 ? std::string("") : std::to_string(b->epsRoot)) << "," << (b->epsRoot < 0 ? std::string("") : std::to_string(b->epsTune)) << ","
                   << z.pcmOffset << "," << z.pcmLength << "," << z.playStart << "," << z.loopStart << "," << z.loopEnd << "," << z.frameSize << ","
                   << z.frameCount << "," << std::hex << std::setw(8) << std::setfill('0') << z.pcmCrc32 << std::dec << std::setfill(' ') << "," << z.flags
                   << ",\"" << b->source << "\",\"" << b->note << "\"\n";
            }
            std::ofstream dc(a.reportDir + "/directory.csv");
            dc << "wave_select,name,number,checksum_spec,preset_id,class,flags,zone_count,first_zone,note\n";
            for (auto &[sel, bw] : waves)
                dc << sel << ",\"" << bw.name << "\"," << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << bw.d.number << "," << std::setw(4)
                   << bw.d.checksumSpec << "," << std::setw(8) << bw.d.presetId << std::dec << std::nouppercase << std::setfill(' ') << "," << bw.d.waveClass << ","
                   << bw.d.flags << "," << bw.d.zoneCount << "," << bw.d.firstZone << ",\"" << bw.note << "\"\n";
            std::ofstream lg(a.reportDir + "/build_log.txt"); lg << log.str();
        }
        std::cout << log.str();

        // ---------- native wave image (voice-chip memory layout taken from the user's OS image at build time)
        if (!a.nativeOut.empty()) {
            native::OsTables os; if (!os.load(a.osImage)) die("OS image: " + os.error);
            native::Options no; no.resolve = a.nativeResolve;
            auto nr = native::build(rom, os, no);
            if (!a.reportDir.empty()) native::writeReport(nr, a.reportDir);
            std::cout << "native: " << nr.summary;
            for (auto &p : nr.problems) std::cout << "native: " << p << "\n";
            if (!nr.ok) die("native image not written");
            std::vector<uint8_t> nb(nr.image.size() * 2);
            for (size_t i = 0; i < nr.image.size(); ++i) { uint16_t v = (uint16_t)nr.image[i]; nb[2 * i] = v & 0xFF; nb[2 * i + 1] = v >> 8; }
            io::writeFile(a.nativeOut, nb);
            std::cout << "native: written " << a.nativeOut << " (" << nb.size() << " bytes, 16-bit little-endian, bank base 0x"
                      << std::hex << os.p.bankBase << std::dec << (a.nativeResolve ? ", resolutions applied" : ", strict") << ")\n";
            if (!a.reportDir.empty()) { std::ofstream lg(a.reportDir + "/build_log.txt", std::ios::app); lg << "native: " << nr.summary << "native: written " << a.nativeOut << "\n"; }
        }
    } catch (const std::exception &e) {
        std::cerr << "rombuilder: ERROR: " << e.what() << "\n"; return 1;
    }
    return 0;
}
