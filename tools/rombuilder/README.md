# rombuilder

Builds and checks the Phyzo wave ROM container (`docs/ROM_FORMAT_SPEC.md`). Plain C++17, no JUCE, Linux/macOS.

This directory contains code only. Wave data, IDs and file maps are inputs supplied from the private data repo;
none of them are compiled in.

## Build

    cmake -S tools/rombuilder -B build && cmake --build build -j

## Tools

`rombuilder` — writes the ROM, re-reads it from disk and verifies: CRC32, every zone's PCM byte-identical to its
source, every manifest wave resolvable by preset ID, every populated wave covering keys 0–127 with no gaps or
overlaps. Any failure aborts with a non-zero exit code.

    rombuilder --manifest phyzo_wave_manifest.csv --sources rom_sources.csv --data-root DATA \
               --exp3-text "EXP3 Transwaves.txt" --out phyzo_wave_rom.bin --report-dir report \
               [--rom-version 0.1] [--build-date 2026-10-01T12:00:00Z]

Given the same inputs and `--build-date`, output is byte-identical.

Inputs:
- manifest CSV: `wave_select,name,number,checksum_spec_order,preset_id_as_stored,source,in_factory_presets,notes`
- sources CSV: `wave_select,name,kind,source,text_wave,text_sample,key_lo_override,key_hi_override,params,note`
  with `kind` = `exp3` (EPS .img + text listing entry), `zonefile` (zone JSON; the older keyword `mr` is still accepted), `wav` (params `root`, `rate_hz`, optional `confidence=low`; keys from the overrides), `sine` (generated) or `none`
  (pending). Key overrides must be explained in `note`; they set `KEY_OVERRIDE` on the zone.
- EXP-3 text listing (key ranges, root, tune, loop mode, frame counts).

`refplayer` — minimal reference player (linear interpolation, no filter or envelope). Renders each wave at C4
with Start Index 0 and, for sweepable zones, 128 Start Index steps (0 → 127) of 0.25 s each. Index changes are
latched to the next loop-end wrap, never mid-cycle; `sweep_switches.csv` logs where each one took effect.

    refplayer phyzo_wave_rom.bin renders [--key 60] [--c4-seconds 2] [--step-seconds 0.25] [--descending 1]
              [--mod-value 0] [--mod-amount 0] [--offset 64] [--waves 1,2,42] [--keyseq 21,108,3]
    refplayer phyzo_wave_rom.bin outdir --notes notes.csv --notes-wave N --notes-out file.wav [--gain 0.5]

`--descending 1` renders the sweeps from index 127 down to 0, the order of the hardware recordings. The Start Index
position follows OS routine 0x1AC28 (`--mod-value`, `--mod-amount`, `--offset` feed it). `--keyseq` renders one note per
key (0.4 s on, 0.1 s off) for the waves in `--waves`, matching the private single-wave recordings. `--notes` mixes a note list
(`time_s,key,duration_s,semitones`; one-shots play to their end, `semitones` is the oscillator Semitone Tune) for one wave.

`regress/regress.py` — measures the renders: pitch by harmonic comb vs targets (±3 c), and step similarity of
each sweep step against the source frame the firmware law expects (≥ 0.97). Needs numpy. Targets CSV is private.

`regress/clicks.py` — counts step-boundary clicks in the sweep renders (amplitude-jump and strict slope criteria).

`regress/compare_recordings.py` — compares sweep renders with hardware recordings: loop period, sample-accurate switch
instants, wrap alignment, click counts at graded margins and first/last frame played. Recordings stay private.

`regress/compare_notes.py` — per-note pitch, waveform and level-profile comparison of a `--keyseq` render with a recording.

`ci/build-wave-rom.yml` — example workflow for the private data repo. Public workflows must never touch ROM data.

## Base-ROM builds (ROM v0.3.1 and later)

`--base-rom old.bin` starts every wave as an exact copy of an existing ROM. Only waves that appear in the
`--sources` CSV are rebuilt; `--manifest` and `--exp3-text` are not needed. After writing, the builder checks
that every kept zone is byte-identical to the base ROM (PCM and zone record including PCM offset and flags) and
stops if not. `--rom-version 0.3.1` stores the third component in the header field `rom_patch` (format 1.3).

## Native wave image

`--native-out native.bin --os-image <user's OS image>` writes the voice chip's internal wave memory
(2,097,152 signed 16-bit samples, little-endian, chip addresses 0x20000000 + n). The OS image is read at
build time only: wave slots and zone descriptors come from it, nothing from it is stored in this repository.
See docs/NATIVE_IMAGE.md for the placement rules, mismatch classes and the report files. By default the
builder refuses to write the image on any length mismatch; `--native-resolve 1` applies the documented
resolution per class and reports each one.
