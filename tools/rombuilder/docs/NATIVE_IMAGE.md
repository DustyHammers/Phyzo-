# Native wave image (voice-chip memory) — builder rules

The original OS programs the voice chip with absolute sample addresses taken from its own wave tables. The
native image puts every zone's PCM at exactly those addresses, so the unmodified OS can play it through an
emulated voice chip. The builder reads the tables from the user's OS image at build time; addresses of the
tables are listed in `src/native_image.hpp` (`OsProfile`). No table content is stored in this repository.

## Mapping
- OS wave slots are enumerated from FIRST[number] and CHECKSUM[slot]; each slot's preset ID
  (number << 16 | byte-swapped checksum) selects the ROM wave.
- Every OS zone (sequential top keys, last = 127) maps to the ROM zone that covers its top key.

## Placement and length rules
- One-shots: our sample 0 at the OS start. Looping zones: our loop start at the OS loop start.
- Region: [min(start, loop start), loop end). Stretch zones (mode 5): the cycle [loop start, loop end).
- EXACT: our PCM is the region. EXACT+GUARD: our PCM is the region plus 2 guard samples (EPS .img sources
  carry them) that land in the 2-sample gap before the next zone.
- Overlapping OS regions (shared or nested zones) must receive identical samples; a difference is a conflict.
- FITTED: WAV-sourced one-shots are resampled (Kaiser-windowed sinc) from the source rate to the rate the OS
  plays them at their root key, 44100·2^((pitch + 256·root − 1741)/3072), then cut or zero-padded to the span.

## Mismatch classes (strict mode refuses; `--native-resolve 1` resolves as listed)
| class | meaning | resolution |
|---|---|---|
| EXTRA_AFTER_OS_REGION | our PCM runs past the OS loop end (more frames than the OS addresses) | placed whole where memory is unused; cut where another zone's data begins |
| LEAD_BEFORE_CYCLE | stretch zone whose source has samples before the cycle | placed whole before the cycle where memory is unused |
| FRACTIONAL_END | the OS loop end has a fraction; the sample at the integer end is played | that sample = the cycle's first sample |
| NO_SOURCE_SPLIT | the OS plays a fixed single cycle on keys where our zone sweeps a table | our table's last frame (Open: no source) |
| GENERATED_LOOP_DIFFERS | generated loop of a different length | our loop as one period, periodically resampled to the OS loop length |
| KIND_MISMATCH, GEOMETRY_MISMATCH, LENGTH_MISMATCH | anything else | never resolved; the image is not written |

## Read-back
Every non-fitted zone is read back through the OS tables and compared with the ROM zone's PCM (guard samples
included). Gaps between regions are zero.

## Report files (--report-dir)
`native_zones.csv` (one row per OS zone: keys, mode, class, pitch difference in cents, W/N, loop, lead-in,
region, placement, fit numbers, resolution, read-back), `native_gaps.txt`, `native_summary.txt`.
