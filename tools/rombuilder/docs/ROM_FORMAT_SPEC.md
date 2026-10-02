# Phyzo wave ROM format — specification v1.3

Status: v1.3 (adds header field rom_patch), used by wave ROM v0.3.1 (2026-10-01). v1.2 (zone flag LOW_CONFIDENCE) was used by ROM v0.3. v1.1 (sweep type 4) was used by ROM v0.2, v1.0 by ROM v0.1; older files stay valid. Container designed for the Phyzo recreation's
`WaveStore`. The container and this document hold no wave data; every ROM file is built from user-supplied
inputs by `tools/rombuilder`. The plugin does not read the container itself: it loads the native wave image built
from it (NATIVE_IMAGE.md) from `~/Documents/Phyzo/roms/`.

## 1. General rules

- Little-endian throughout. All multi-byte integers are unsigned unless marked `s`.
- Three sections follow the header: wave directory, zone records, PCM block. Each section starts on a
  16-byte boundary; records have fixed sizes given in the header, so readers must use `dir_entry_size` and
  `zone_size` as strides (a later minor version may append fields).
- Strings are fixed-length, NUL-padded ASCII.
- PCM is signed 16-bit, mono. Each zone's PCM starts on an 8-sample boundary inside the PCM block. Zones whose
  PCM is identical to, or a prefix of, an earlier zone's PCM point at the same samples (`ZONE_PCM_SHARED`).
- Versioning: `format_major` changes break readers; `format_minor` adds fields only. `rom_major.rom_minor`
  versions the content (a wave fix means a new ROM version, never an engine change).

## 2. Header (96 bytes, offset 0)

| Offset | Type | Field | Notes |
|---:|---|---|---|
| 0 | char[8] | magic | `FZWAVROM` |
| 8 | u16 | format_major | 1 |
| 10 | u16 | format_minor | 2 (1.0 and 1.1 files remain valid) |
| 12 | u16 | rom_major | content version, e.g. 0 |
| 14 | u16 | rom_minor | content version, e.g. 1 |
| 16 | char[24] | build_date | ISO-8601 UTC, e.g. `2026-10-01T12:00:00Z` |
| 40 | u32 | file_size | must equal the file length |
| 44 | u32 | header_size | 96 |
| 48 | u32 | wave_count | directory entries (58 for Phyzo) |
| 52 | u32 | dir_offset | |
| 56 | u32 | dir_entry_size | 48 |
| 60 | u32 | zone_count | |
| 64 | u32 | zone_offset | |
| 68 | u32 | zone_size | 64 |
| 72 | u32 | pcm_offset | |
| 76 | u32 | pcm_samples | length of the PCM block in samples |
| 80 | u32 | crc32 | CRC-32 (IEEE, reflected, init/xorout 0xFFFFFFFF) of the whole file with these 4 bytes set to 0 |
| 84 | u32 | flags | 0 |
| 88 | u16 | rom_patch | third component of the ROM version (v0.3.1 → 1); 0 in files older than format 1.3 (the field was reserved and zero) |
| 90 | u16 | reserved | 0 |
| 92 | u32 | reserved | 0 |

A reader rejects the file on wrong magic, unknown `format_major`, size mismatch, CRC mismatch, or any
directory/zone/PCM reference outside the file. The plugin then shows the "wave ROM missing/invalid" message.

## 3. Wave directory (48 bytes per entry, ordered by wave_select)

| Offset | Type | Field | Notes |
|---:|---|---|---|
| 0 | u16 | wave_select | 1–58, the front-panel/manual order |
| 2 | u16 | number | waveform number (native) |
| 4 | u16 | checksum_spec | checksum as written in the original SysEx spec |
| 6 | u16 | class | 1 internal ROM, 2 expansion, 3 native-only |
| 8 | u16 | flags | bit0 HAS_ZONES, bit1 SOURCE_PENDING, bit2 IN_FACTORY_PRESETS |
| 10 | u16 | zone_count | 0 for pending waves |
| 12 | u32 | preset_id | `number << 16 | stored checksum`, exactly as the 4 bytes appear in preset/sound data |
| 16 | u32 | first_zone | index of the first zone record |
| 20 | char[24] | name | display name |
| 44 | u32 | reserved | 0 |

Lookup key: presets carry the checksum byte-swapped relative to the SysEx spec, so `preset_id` is
`number << 16 | byteswap16(checksum_spec)`. The builder enforces this identity for every entry. Engines resolve a
preset's (number, checksum) pair against `preset_id`; `checksum_spec` is kept for SysEx documentation lookups.

A wave whose source is not available yet has `SOURCE_PENDING` set and `zone_count = 0`. It still resolves, so
the engine can tell "known wave, no audio in this ROM" from "unknown wave".

## 4. Zone records (64 bytes each, grouped by wave, sorted by low_key)

| Offset | Type | Field | Notes |
|---:|---|---|---|
| 0 | u16 | wave_select | owning wave |
| 2 | u8 | zone_index | source zone/sample number (1-based) |
| 3 | u8 | low_key | MIDI note, inclusive |
| 4 | u8 | high_key | MIDI note, inclusive |
| 5 | u8 | loop_type | 0 one-shot, 1 forward, 2 bidirectional, 3 plain loop (native loop mode 2) |
| 6 | u8 | sweep_type | 0 none, 1 stored table, 2 single-cycle stretch, 3 LoopStartX placeholder, 4 stored table chosen at note start (native loop mode 7) |
| 7 | u8 | source_kind | 1 EPS .img, 2 zone file, 3 WAV, 4 generated (provenance only) |
| 8 | s32 | zone_pitch | Phyzo pitch-law zone pitch (section 5) |
| 12 | u32 | source_rate_mhz | source sample rate in milli-Hz (informational; already folded into zone_pitch) |
| 16 | u32 | pcm_offset | first sample, relative to the PCM block |
| 20 | u32 | pcm_length | samples |
| 24 | u32 | play_start | where a note starts when the zone has no sweep (sample index) |
| 28 | u32 | loop_start | loop/table start (sample index) |
| 32 | u32 | loop_end | exclusive |
| 36 | u32 | frame_size | W: samples per frame (table), cycle length L (stretch) |
| 40 | u32 | frame_count | N (table), 1 (stretch) |
| 44 | u32 | pcm_crc32 | CRC-32 of this zone's PCM as little-endian bytes |
| 48 | u32 | flags | bit0 KEY_OVERRIDE (range differs from source listing), bit1 VIRTUAL_ZEROS_BEFORE, bit2 PCM_SHARED, bit3 LOW_CONFIDENCE (mapping approved but not confirmed; informational, playback unaffected) |
| 52 | s16 | src_root | source root key (EXP-3 text) — provenance |
| 54 | s16 | src_tune | source tune in source units (EXP-3: 1/64 semitone) — provenance |
| 56 | u32[2] | reserved | 0 |

Every wave with zones covers keys 0–127 exactly once (no gaps, no overlaps); the builder refuses to write a ROM
that breaks this. Zone selection: the zone whose `[low_key, high_key]` contains the played (shifted) key.

## 5. Pitch law (one formula for every zone)

```
p  = zone_pitch + 256 * key - 1741          (+ stretch shift s for sweep_type 2)
increment = 2^(p / 3072)                     source samples per output sample at 44.1 kHz
```
The source rate is folded into `zone_pitch` by the builder:

- EXP-3 zones: `zone_pitch = round(1741 - 256*root + 4*tune + 3072*log2(44642.857/44100))`, with `root` and
  `tune` (1/64 semitone) from the EXP-3 text listing. The EPS calibration fields in the .img files are not used.
- zone-file zones: `zone_pitch = F` from the zone file, verbatim.
- Generated/WAV zones: `zone_pitch = round(1741 - 256*root + 3072*log2(rate/44100))`.

## 6. Playback semantics by type

**Loop types**
- `ONE_SHOT` (0): play from `play_start` to `pcm_length`, then stop; pitch-tracked by the same law as every zone (e.g. the drum-kit wave).
- `FORWARD` (1): play forward; on reaching `loop_end` jump back by `loop_end - loop_start`. For sweepable zones
  the loop window is replaced by the current frame window (below).
- `BIDIRECTIONAL` (2): as forward, but reflect at both window edges; the period is twice the window length.
- `PLAIN_LOOP` (3): native loop mode 2. Play from `play_start` (lead-in included) through `loop_end`, then loop
  `[loop_start, loop_end)`. No sweep. Kept distinct from FORWARD for provenance; the engine path is the same.

**Start Index position** (OS routine 0x1AC28), 8.8 fixed point:
`p = StartIndex × 256 + (modValue × modAmount) / 128 + (offset − 64) × 256`, clamped to 0 … 0x7FFF (0–127.99).
`offset` is a byte centred at 64 (most likely perf2, the transwave-position knob, Derived); the scale of `modValue` is still Open. `index = p >> 8`.

**Sweep types**
- `NONE` (0): window = `[loop_start, loop_end)`.
- `TABLE` (1): stored frames. `frame = index * N / 128` (integer), clamped to the last frame; window =
  `[loop_start + frame*W, loop_start + (frame+1)*W)`. Used for native loop mode 6 and all EXP-3 transwaves.
- `STRETCH` (2): native loop mode 5, single cycle of length L stored. Shift
  `s = p * smax(key) / 32768` (integer; equals `index * smax(key) / 128` for whole indices), `smax(key) = 0x1800` for key ≤ 66, else `max(0, 0x5A00 - 256*key)`.
  Window = the last `L * 2^(s/3072)` samples ending at the cycle end; reads before the cycle return 0
  (`VIRTUAL_ZEROS_BEFORE`); pitch is raised by `s` units, so the loop rate stays constant. At key ≤ 66 this is
  `k = 2^(48*index/3072)`. The per-index scaling above key 66 is Derived from the zone files'
  `max_shift_at_low_key` values (= smax × 127/128); the C4 renders do not depend on it.
- `TABLE_NOTE_START` (4): native loop mode 7. As `TABLE`, but the frame is chosen only when the note starts; index changes
  while the note sounds are ignored. (Format 1.1; no zone in ROM v0.2 uses it.)
- `LOOPSTARTX` (3): placeholder for EXP-3 LoopStartX waves (RAT BREATH X). Behaviour Open (Start Index may move
  the sample start). Engines treat it as `NONE` until specified; the zone already carries the full sample.

When the index changes while a note sounds, the new window is latched and takes effect only when playback next
passes the current loop end; bidirectional loops take it at either reflection (loop end or loop start), as the
hardware does. The cycle in progress always finishes. At that point the overshoot carries into the new window (`new_pos = new_start + overshoot * new_length /
old_length`), and the interpolation partner of the last sample before the wrap is read from the new window.
(Changed 2026-10-01: earlier drafts switched mid-cycle, then latched bidirectional loops at the loop end only. ROM data is unaffected.)

## 7. Reader checklist (WaveStore)

1. Read the file once, check magic, version, size and CRC; share it read-only between plugin instances.
2. Build a hash map `preset_id → directory entry`.
3. For each note: resolve the wave, pick the zone by key, then apply section 5 and 6.
4. Report missing or invalid ROMs, and pending waves, through the Files tab.
