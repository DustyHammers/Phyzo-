# Panel analog controls (F-03)

The panel reports its 26 analog controls as Bx cc vv: control number = cc & 0x1F, raw 10-bit value = vv + ((x & 7) << 7) (0–1023). The OS requests all 26 with F4 at boot (see F-02) and otherwise receives a single message whenever a control moves (absolute raw value).

| cc | Control ID | Control | Range the OS uses | Path inside the OS |
|---|---|---|---|---|
| 0 | 57 | Mod Speed (LFO or Noise rate) | −11 … 99 | panel |
| 1 | 50 | Pan | −63 … 64 | panel |
| 2 | 49 | Level | −30 … 14 | panel |
| 3 | 63 | perf5 (System Controller 4) | 0 … 127 | panel |
| 4 | 56 | Filter Amount | −127 … 127 | panel |
| 5 | 62 | perf4 (detune, CC 79) | 0 … 127 | panel |
| 6 | 61 | perf3 (filter cutoff, CC 74) | 0 … 127 | panel |
| 7 | 53 | Wave Amount | −127 … 127 | panel |
| 8 | 55 | Resonance | 0 … 49 | panel |
| 9 | 59 | perf1 (System Controller 3) | 0 … 127 | panel |
| 10 | 52 | Wave Select | 0 … 57 | panel |
| 11 | 41 | Volume | 0 … 127 | direct (always applied) |
| 12 | 47 | Sustain | 0 … 127 | panel |
| 13 | 60 | perf2 (table sweep, CC 71) | 0 … 127 | panel |
| 14 | 58 | Arp Value | 0 … 127 | panel |
| 15 | 51 | Mix | 0 … 127 | panel |
| 16 | 46 | Decay | 0 … 99 | panel |
| 17 | 45 | Attack | 0 … 99 | panel |
| 18 | 43 | Pitch Amount | −127 … 127 | panel |
| 19 | 44 | Glide Time | 0 … 100 | panel |
| 20 | 42 | Pitch Tune | −24 … 24 | panel |
| 21 | 48 | Release | 0 … 99 | panel |
| 22 | 54 | Cutoff | 0 … 127 | panel |
| 23 | 64 | Pitch wheel | 0 … 16383 | MIDI (pitch bend) |
| 24 | 65 | Mod wheel | 0 … 127 | MIDI (CC 1) |
| 25 | 66 | Pressure (channel pressure) | 0 … 127 | MIDI (channel pressure) |

Control IDs 42–63 match the front panel control map (perf1–perf5 are IDs 59–63). Mod Speed's −11 … −1 are probably tempo-sync divisions (not confirmed).

## Behaviour that matters for the plugin

Ordinary knobs: the raw 0–1023 value is scaled linearly onto the range above. Knob moves are acted on only after boot has completed; before that the value is just stored.
Volume is applied every time, including the F4 answer. Raw 1023 = full volume; raw 512 ≈ −6 dB.
Pitch wheel: the first report after boot sets the wheel's centre (clamped to raw 448–576). Dead band of ±20 counts around the centre; full bend at raw 924 (up) and raw 100 (down). The F4 answer must therefore be 512.
Mod wheel: 0 up to raw 80, then rising to 127 at about raw 917.
Pressure: the first report sets the rest threshold (raw + 16); values below it read as 0. Answer 0 at boot.
Jitter filter: a change that reverses direction is ignored if smaller than 4 counts (6 for the wheels and pressure).

## Fresh-instance F4 answer (raw values, in cc order)

0: 102 · 1: 508 · 2: 698 · 3: 2 · 4: 512 · 5: 2 · 6: 516 · 7: 512 · 8: 5 · 9: 2 · 10: 4 · 11: 1023 · 12: 2 · 13: 516 · 14: 2 · 15: 516 · 16: 3 · 17: 3 · 18: 512 · 19: 3 · 20: 512 · 21: 3 · 22: 516 · 23: 512 · 24: 0 · 25: 0
