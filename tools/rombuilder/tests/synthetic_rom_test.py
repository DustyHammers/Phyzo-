#!/usr/bin/env python3
"""rombuilder + refplayer test that needs no ROM data.

All inputs are generated in a temporary folder at run time (a generated sine, two generated WAVs on split key
ranges, a zone file with a table-sweep zone and a plain-loop zone, a pending wave). Checks: the build succeeds and
verifies itself, identical inputs give a byte-identical ROM, the header matches the inputs, refplayer renders
deterministically, a base-ROM rebuild keeps the other waves byte-identical, and broken inputs are refused.

Usage: synthetic_rom_test.py <rombuilder> <refplayer>
"""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def write_wav(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def write_csv(path, header, rows):
    with open(path, "w", newline="") as f:
        f.write(",".join(header) + "\n")
        for r in rows:
            f.write(",".join(r) + "\n")


def run(cmd, expect_ok=True):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if (p.returncode == 0) != expect_ok:
        print("command:", " ".join(cmd))
        print(p.stdout, p.stderr)
    return p


def preset_id(number, checksum):
    swapped = ((checksum & 0xFF) << 8) | (checksum >> 8)
    return "%08X" % ((number << 16) | swapped)


MANIFEST_HEADER = ["wave_select", "name", "number", "checksum_spec_order", "preset_id_as_stored", "source",
                   "in_factory_presets", "notes"]
SOURCES_HEADER = ["wave_select", "name", "kind", "source", "text_wave", "text_sample", "key_lo_override",
                  "key_hi_override", "params", "note"]
WAVES = [(1, "TEST SINE", 0x0001, 0x1234, "generated", "YES"),
         (2, "TEST WAV", 0x0002, 0xABCD, "internal", "YES"),
         (3, "TEST TABLE", 0x0003, 0x0F0F, "expansion", "NO"),
         (4, "TEST PENDING", 0x0004, 0x1111, "", "NO")]


def make_inputs(d):
    write_csv(os.path.join(d, "manifest.csv"), MANIFEST_HEADER,
              [[str(s), n, "%04X" % num, "%04X" % ck, preset_id(num, ck), src, fp, ""] for s, n, num, ck, src, fp in WAVES])
    write_csv(os.path.join(d, "sources.csv"), SOURCES_HEADER, [
        ["1", "TEST SINE", "sine", "", "", "", "", "", "cycle=64;peak=20000;hz_at_c4=261.63", "generated"],
        ["2", "TEST WAV", "wav", "low.wav", "", "", "0", "59", "root=48;rate_hz=44100", ""],
        ["2", "TEST WAV", "wav", "high.wav", "", "", "60", "127", "root=72;rate_hz=22050;confidence=low", ""],
        ["3", "TEST TABLE", "zonefile", "table.json", "", "", "", "", "", ""],
        ["4", "TEST PENDING", "none", "", "", "", "", "", "", "awaiting a source"],
    ])
    data = os.path.join(d, "data")
    os.makedirs(data, exist_ok=True)
    write_wav(os.path.join(data, "low.wav"),
              [int(12000 * math.sin(i * 0.02) * math.exp(-i / 3000.0)) for i in range(6000)], 44100)
    write_wav(os.path.join(data, "high.wav"), [(i * 37 % 4001) - 2000 for i in range(3000)], 22050)
    W, N = 32, 8                                             # mode 6: N frames of W samples
    table = [int(15000 * math.sin(2 * math.pi * i / W) * (1 + f) / N) for f in range(N) for i in range(W)]
    plain = [int(9000 * math.sin(i * 0.1)) for i in range(300)]
    json.dump({"directory_number": 3, "directory_checksum": 0x0F0F, "wave": "TEST TABLE", "sample_rate_hz": 44100,
               "zones": [
                   {"index": 1, "low_key": 0, "top_key": 63, "pitch_F": 1500, "start": W, "loop_start": 0,
                    "loop_end": W * N, "loop_mode": 6, "frame_size": W, "frames": N, "audio_int16": table},
                   {"index": 2, "low_key": 64, "top_key": 127, "pitch_F": 1700, "start": 1000, "loop_start": 1100,
                    "loop_end": 1300, "loop_mode": 2, "audio_int16": plain}]},
              open(os.path.join(data, "table.json"), "w"))


def build(rb, d, out, extra=(), expect_ok=True, manifest="manifest.csv", sources="sources.csv"):
    report = out + "_report"
    os.makedirs(report, exist_ok=True)
    cmd = [rb, "--sources", os.path.join(d, sources), "--data-root", os.path.join(d, "data"),
           "--out", out, "--report-dir", report, "--rom-version", "0.1", "--build-date", "2026-10-02T00:00:00Z"]
    if manifest:
        cmd += ["--manifest", os.path.join(d, manifest)]
    return run(cmd + list(extra), expect_ok)


def files_of(folder):
    return {n: open(os.path.join(folder, n), "rb").read() for n in sorted(os.listdir(folder))}


def main():
    rb, rp = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="phyzo_rom_test_") as d:
        make_inputs(d)
        a, b = os.path.join(d, "a.bin"), os.path.join(d, "b.bin")
        pa = build(rb, d, a)
        check(pa.returncode == 0, "rombuilder succeeds on the synthetic inputs")
        check("verified from disk" in pa.stdout, "rombuilder verifies the written ROM")
        check(build(rb, d, b).returncode == 0, "second build succeeds")
        rom = open(a, "rb").read()
        check(rom == open(b, "rb").read(), "same inputs and build date give a byte-identical ROM")

        check(rom[:8] == b"FZWAVROM", "magic")
        fmaj, fmin, rmaj, rmin = struct.unpack_from("<4H", rom, 8)            # offsets: ROM_FORMAT_SPEC.md section 2
        rpat = struct.unpack_from("<H", rom, 88)[0]
        check((fmaj, fmin) == (1, 3), "format version 1.3")
        check((rmaj, rmin, rpat) == (0, 1, 0), "ROM version from --rom-version")
        file_size, header_size, wave_count = struct.unpack_from("<3I", rom, 40)
        check(file_size == len(rom) and header_size == 96, "file and header size fields")
        check(wave_count == 4, "four waves in the directory")
        zone_count = struct.unpack_from("<I", rom, 60)[0]
        check(zone_count == 5, "five zones (1 sine + 2 WAV + 2 zone-file)")

        # Reference player: deterministic renders (C4 for waves 1-3, a table sweep for wave 3).
        r1, r2 = os.path.join(d, "r1"), os.path.join(d, "r2")
        os.makedirs(r1); os.makedirs(r2)
        p1, p2 = run([rp, a, r1]), run([rp, a, r2])
        check(p1.returncode == 0 and p2.returncode == 0, "refplayer runs")
        check("rendered 3 C4 files, 1 sweeps" in p1.stdout, "refplayer renders 3 waves and 1 sweep: " + p1.stdout.strip())
        check(files_of(r1) == files_of(r2), "refplayer output is deterministic")
        rn = os.path.join(d, "rn"); os.makedirs(rn)
        with open(os.path.join(d, "notes.csv"), "w") as f:
            f.write("time_s,key,duration_s,semitones\n0,30,0.2,0\n0.1,90,0.2,0\n0.2,60,0.3,2\n")
        pn = run([rp, a, rn, "--notes", os.path.join(d, "notes.csv"), "--notes-wave", "2", "--notes-out", "notes.wav"])
        check(pn.returncode == 0 and os.path.getsize(os.path.join(rn, "notes.wav")) > 44, "refplayer note list")

        # Base-ROM rebuild of one wave keeps every other zone byte-identical (the builder checks and reports it).
        write_csv(os.path.join(d, "sources_sine.csv"), SOURCES_HEADER,
                  [["1", "TEST SINE", "sine", "", "", "", "", "", "cycle=128;peak=10000;hz_at_c4=261.63", "regenerated"]])
        c = os.path.join(d, "c.bin")
        pc = build(rb, d, c, ["--base-rom", a], manifest=None, sources="sources_sine.csv")
        check(pc.returncode == 0, "base-ROM rebuild succeeds")
        check("4/4 kept zones PCM byte-identical" in pc.stdout, "base-ROM rebuild keeps the other zones: " + pc.stdout.strip())

        # Broken inputs are refused.
        bad = [r[:] for r in ([str(s), n, "%04X" % num, "%04X" % ck, preset_id(num, ck), src, fp, ""] for s, n, num, ck, src, fp in WAVES)]
        bad[1][4] = "00020000"
        write_csv(os.path.join(d, "bad_manifest.csv"), MANIFEST_HEADER, bad)
        pb = build(rb, d, os.path.join(d, "bad1.bin"), expect_ok=False, manifest="bad_manifest.csv")
        check(pb.returncode != 0 and "preset_id_as_stored" in pb.stderr, "wrong preset ID is refused")
        write_csv(os.path.join(d, "gap_sources.csv"), SOURCES_HEADER, [
            ["1", "TEST SINE", "sine", "", "", "", "", "", "cycle=64;peak=20000;hz_at_c4=261.63", ""],
            ["2", "TEST WAV", "wav", "low.wav", "", "", "0", "59", "root=48;rate_hz=44100", ""],
            ["2", "TEST WAV", "wav", "high.wav", "", "", "61", "127", "root=72;rate_hz=22050", ""],
            ["3", "TEST TABLE", "zonefile", "table.json", "", "", "", "", "", ""]])
        pg = build(rb, d, os.path.join(d, "bad2.bin"), expect_ok=False, sources="gap_sources.csv")
        check(pg.returncode != 0 and "key gap" in pg.stderr, "a key-range gap is refused")

    print("synthetic_rom_test:", "FAILED" if failures else "passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
