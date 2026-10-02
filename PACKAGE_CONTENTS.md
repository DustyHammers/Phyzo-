# Phyzo public repo — complete package (2026-10-02)

Everything here goes to the public GitHub repo. Source code and docs only: no OS image, wave data, tables,
recordings or renders (checked: no data files, nothing over 200 KB, no embedded byte tables).

| Path | What it is | Version |
|---|---|---|
| `phyzo-emu/` | The emulator: MC68340 (Musashi) + timers/serial/DMA, voice chip with resonant filter, ESP2 core (translated, verified against the reference interpreter), panel model; tools `phyzo_boot`, `phyzo_play`, `filter_test`, `esp2_test`, `os_disasm` | v0.6 (latest) |
| `tools/rombuilder/` | Wave ROM builder, native-image output, reference player, regression scripts, example private CI workflow (`ci/build-wave-rom.yml`, for the private repo only) | latest (voice-chip session) |
| `tools/rombuilder/docs/ROM_FORMAT_SPEC.md` | FZWAVROM container format | v1.3 |
| `tools/rombuilder/docs/NATIVE_IMAGE.md` | How the native wave image is built from the OS tables | — |
| `tools/esp2obj_parser.py` | Parser for ESP2 effect-program objects (format logic from the public spec) | — |
| `tools/analysis/filter_fit/`, `tools/analysis/filter_validation/` | Resonant-filter fitting and held-out validation scripts | — |

Runtime: the plugin and tools load the two ROM files from the user's machine (see the private repo's `ROMs/`).

## Rename to Phyzo: done and verified (2026-10-02)
- The original product and maker names are gone from folders, file names, build targets, code identifiers,
  comments and docs: `phyzo-emu/` (was the boot harness), tools `phyzo_boot`, `phyzo_boot_trace`, `phyzo_play`.
- Verified by building both versions from scratch: the renamed emulator boots the real OS to "P 01", passes the
  button test, and its boot traces and a preset render (dry and wet) are byte-identical to the original's; all 113
  reference-player renders of the wave ROM are byte-identical too.
- Intentionally kept: `THIRD_PARTY_NOTICES.md` unchanged (it credits other projects); the ROM file magic `FZWAVROM`
  and the `fzrom` code namespace (needed to read the existing wave ROM); the source-list keyword `mr`, still accepted
  alongside the new `zonefile` so existing private source lists keep working; the data-format names `EPS .img` and
  `EXP-3 text listing`, which describe the input formats the builder reads; "MR pointer" in the 68340 serial code,
  which is the chip's Mode Register pointer.
