# Phyzo: rules for working in this repository

Phyzo is a VST3/AU plugin for personal use that runs a 1998 hardware synthesizer's original OS on emulated hardware
(`phyzo-emu/`). The OS provides all synth behaviour; our code is the hardware. The owner has no local build machine:
GitHub Actions builds everything, and the owner tests the plugin in REAPER on an Apple Silicon Mac. Guide the
owner step by step through anything they must do by hand on github.com or on the Mac.

## Rules (from the owner; do not relax them)

1. **Never commit ROM files or other proprietary data**: no OS image, wave data, tables, recordings, renders,
   trace output or SysEx dumps. CI rejects .bin/.img/.wav/.mid/.syx/.mp3 files and any file over 1 MB.
2. **No synth parameters added or removed** without the owner's explicit instruction.
3. **Mockup before any UI.** Show a mockup and wait for approval before building any user interface.
4. **Stop after each phase**: summarise what changed and say what to test.
5. **THIRD_PARTY_NOTICES.md** (`phyzo-emu/THIRD_PARTY_NOTICES.md`): never change its existing entries. Complete the
   "Planned dependencies" entry for each dependency (full licence text and version) when it is added.
6. **Identify ROMs by checksum, never by file name** (`phyzo-emu/src/rom_id.*`):
   OS image MD5 `8de06f48bfb0d847cacab05b763e22c5`, native wave image MD5 `42a974e31e48b05815d07554d0063171`.

## Naming and identity

- No product or maker names of the original instrument outside THIRD_PARTY_NOTICES.md. Deliberately kept names are
  listed in PACKAGE_CONTENTS.md. Chip part numbers MC68340, ES5506 and ESP2 stay. Say "table sweep", not the
  original technique's brand-like name.
- The five realtime performance knobs are named by function in code: perf1 (System Controller 3, effect modulation),
  perf2 (table-sweep position, CC 71), perf3 (filter cutoff, CC 74), perf4 (detune, CC 79), perf5
  (System Controller 4). Visible labels come from the skin.
- Keep the MIDI/SysEx identity (manufacturer code, model number, message formats) exactly as the OS uses it.
- Plugin identity, fixed forever (hosts use it to find the plugin in saved projects): manufacturer "DHammers",
  manufacturer code "DHmr", plugin name "Phyzo", plugin code "Phyz", bundle ID com.dhammers.phyzo, plus
  `JUCE_VST3_CAN_REPLACE_VST2=0`. Resulting VST3 class ID `ABCDEF019182FAEB44486D725068797A`, AU `aumu Phyz DHmr`.
  CI checks all of these (`scripts/check_plugin_identity.py`).

## Other constraints

- No CI job may ever need the ROM files.
- Do not copy Gearmulator code (GPL-3); using its design as a reference is fine.
- Speed work must keep the emulator's output byte-identical to the current version.

## Build and CI

- Dependencies are pinned in `scripts/deps.env` and fetched by `scripts/fetch_deps.sh` into `work/deps/` (ignored).
- `.github/workflows/ci.yml`: data-file guard over every commit (`scripts/check_no_data.py`), ROM-free tests on
  Linux (`ctest` in `phyzo-emu` and `tools/rombuilder`), and the macOS plugin build with identity check, auval and
  the downloadable zip. The ESP2 and voice-core tests hold golden hashes of the current output: a speed change
  that alters them has changed the sound.
- Owner's install guide: `docs/INSTALL_MAC.md`.
