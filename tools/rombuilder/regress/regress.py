#!/usr/bin/env python3
"""Wave ROM regression: measures the reference-player renders and compares them with targets.

usage: regress.py ROM.bin RENDER_DIR TARGETS.csv OUT_DIR
  RENDER_DIR  : output of refplayer (render_log.csv + WAVs)
  TARGETS.csv : wave_select,name,zone,target_hz_key60[,record_hw_recording_c,record_hw_step_similarity]
                (private data: derived from the build record; this script holds no wave data)
Pitch: harmonic comb (60 harmonics, 0.25 c grid, +/-200 c) on 0.2-1.4 s of the C4 render.
Steps: per sweep step, harmonic fingerprint of the rendered step vs every source frame/window in the ROM;
       similarity to the frame the firmware law expects, best-matching frame, and frame-order slope.
"""
import csv, math, os, struct, sys, zlib
import numpy as np

PITCH_TOL_C, STEP_TOL = 3.0, 0.97

def load_rom(path):
    b = open(path, 'rb').read()
    assert b[:8] == b'FZWAVROM', 'not a wave ROM'
    fs, hs, wc, do, des, zc, zo, zs, po, ps, crc, fl = struct.unpack_from('<12I', b, 40)
    bb = bytearray(b); bb[80:84] = b'\0\0\0\0'
    assert zlib.crc32(bytes(bb)) == crc, 'ROM CRC32 mismatch'
    pcm = np.frombuffer(b, dtype='<i2', count=ps, offset=po).astype(float)
    zones = []
    for i in range(zc):
        z = struct.unpack_from('<HBBBBBBiIIIIIIIIIIhh', b, zo + i * zs)
        zones.append(dict(ws=z[0], zone=z[1], lo=z[2], hi=z[3], loop=z[4], sweep=z[5], pitch=z[7], off=z[9], n=z[10],
                          play=z[11], ls=z[12], le=z[13], W=z[14], N=z[15]))
    return pcm, zones

def read_wav(path):
    b = open(path, 'rb').read(); pos = 12; rate = 44100; data = None
    while pos + 8 <= len(b):
        cid = b[pos:pos + 4]; sz = struct.unpack_from('<I', b, pos + 4)[0]
        if cid == b'fmt ': rate = struct.unpack_from('<I', b, pos + 12)[0]
        if cid == b'data': data = np.frombuffer(b[pos + 8:pos + 8 + sz], dtype='<i2').astype(float)
        pos += 8 + sz + (sz & 1)
    return rate, data

def comb_pitch(x, sr, f_exp):
    seg = x[int(0.2 * sr):int(1.4 * sr)]
    seg = seg * np.hanning(len(seg))
    nfft = 1 << 22
    X = np.abs(np.fft.rfft(seg, nfft)); df = sr / nfft
    cents = np.arange(-200, 200.25, 0.25)
    best = (-1, 0)
    for c in cents:
        f = f_exp * 2 ** (c / 1200)
        h = np.arange(1, 61) * f; h = h[h < min(20000, sr / 2 - 100)]
        if len(h) == 0: continue
        idx = h / df; i0 = np.floor(idx).astype(int); fr = idx - i0
        s = (X[i0] * (1 - fr) + X[i0 + 1] * fr).sum()
        if s > best[0]: best = (s, c)
    return f_exp * 2 ** (best[1] / 1200)

def fingerprint_seq(seq, H):
    """|DFT| of one period at harmonics 1..H (seq = one period, any length)."""
    n = len(seq); k = np.arange(1, H + 1)[:, None]; j = np.arange(n)[None, :]
    return np.abs((seq[None, :] * np.exp(-2j * np.pi * k * j / n)).sum(axis=1))

def fingerprint_window(cycle, P, H):
    """Stretch window: zeros then `cycle` ending at the window end; window length P (may be fractional)."""
    L = len(cycle); pos = P - L + np.arange(L)
    k = np.arange(1, H + 1)[:, None]
    return np.abs((cycle[None, :] * np.exp(-2j * np.pi * k * pos[None, :] / P)).sum(axis=1))

def fingerprint_render(seg, sr, f0, H):
    t = np.arange(len(seg)) / sr; w = np.hanning(len(seg))
    k = np.arange(1, H + 1)[:, None]
    return np.abs(((seg * w)[None, :] * np.exp(-2j * np.pi * k * f0 * t[None, :])).sum(axis=1))

def cos(a, b):
    na, nb = np.linalg.norm(a), np.linalg.norm(b)
    return float(a @ b / (na * nb)) if na > 0 and nb > 0 else 0.0

def main():
    rom, rdir, tpath, odir = sys.argv[1:5]
    pcm, zones = load_rom(rom)
    log = list(csv.DictReader(open(os.path.join(rdir, 'render_log.csv'))))
    targets = {r['wave_select']: r for r in csv.DictReader(open(tpath))}
    switches = {}
    sp = os.path.join(rdir, 'sweep_switches.csv')
    if os.path.exists(sp):
        for q in csv.DictReader(open(sp)): switches.setdefault(q['sweep_file'], {})[int(q['index'])] = int(q['applied_sample'])
    rows = []
    for r in log:
        if r['loop_type'] == '0' or float(r['expected_loop_hz']) <= 0:   # one-shots have no loop rate to measure
            print(r['wave_select'], r['name'], 'one-shot: skipped', flush=True); continue
        ws = int(r['wave_select']); z = [z for z in zones if z['ws'] == ws and z['zone'] == int(r['zone'])][0]
        sr, x = read_wav(os.path.join(rdir, r['c4_file']))
        f_exp = float(r['expected_loop_hz'])
        f_meas = comb_pitch(x, sr, f_exp)
        t = targets.get(str(ws), {}); tgt = float(t['target_hz_key60']) if t.get('target_hz_key60') else None
        c_tgt = 1200 * math.log2(f_meas / tgt) if tgt else None
        row = dict(wave_select=ws, name=r['name'], zone=r['zone'], keys='%s-%s' % (r['low_key'], r['high_key']),
                   expected_hz='%.4f' % f_exp, measured_hz='%.4f' % f_meas, target_hz=('%.4f' % tgt) if tgt else '',
                   cents_vs_target=('%+.2f' % c_tgt) if c_tgt is not None else '',
                   record_hw_recording_c=t.get('record_hw_recording_c', ''), record_hw_step_similarity=t.get('record_hw_step_similarity', ''))
        flags = []
        if c_tgt is not None and abs(c_tgt) > PITCH_TOL_C: flags.append('pitch')
        if r['sweep_file']:
            sr, s = read_wav(os.path.join(rdir, r['sweep_file'])); n = len(s) // 128
            sw = switches.get(r['sweep_file'], {})
            bounds = [0] + [sw.get(i, i * n) for i in range(1, 128)] + [len(s)]
            # analyse each step after its window actually took effect (index changes are latched to the loop wrap)
            seg_sl = lambda i: s[bounds[i] + int(0.25 * (bounds[i + 1] - bounds[i])): bounds[i + 1] - int(0.02 * n)]
            Hmax = int(min(48, (sr / 2 - 200) / f_exp))
            cyc = pcm[z['off']:z['off'] + z['n']]
            if z['sweep'] == 1:  # table
                frames = []
                for f in range(z['N']):
                    fr = cyc[z['ls'] + f * z['W']: z['ls'] + (f + 1) * z['W']]
                    if z['loop'] == 2: fr = np.concatenate([fr, fr[::-1]])
                    frames.append(fingerprint_seq(fr, min(Hmax, len(fr) // 2)))
                expect = [i * z['N'] // 128 for i in range(128)]
            else:  # stretch: candidate windows are the 128 index positions
                key = 60; smax = 0x1800 if key <= 66 else max(0, 0x5A00 - 256 * key)
                L = z['le'] - z['ls']
                frames = [fingerprint_window(cyc[z['ls']:z['le']], L * 2 ** ((i * smax // 128) / 3072), min(Hmax, L // 2)) for i in range(128)]
                expect = list(range(128))
            H = len(frames[0])
            sims, best_idx, best_sims = [], [], []
            for i in range(128):
                fp = fingerprint_render(seg_sl(i), sr, f_exp, H)
                allc = [cos(fp, fr) for fr in frames]
                sims.append(allc[expect[i]]); b = int(np.argmax(allc)); best_idx.append(b); best_sims.append(allc[b])
            slope = np.polyfit(np.arange(128), np.array(best_idx, float), 1)[0] * 128
            hits = sum(1 for i in range(128) if abs(best_idx[i] - expect[i]) <= 0 or best_sims[i] - sims[i] < 1e-3)
            row.update(step_sim_expected='%.4f' % np.mean(sims), step_sim_min='%.4f' % np.min(sims), step_best_match_rate='%d/128' % hits,
                       frames=len(frames) if z['sweep'] == 1 else '128 windows', slope_over_128='%+.1f' % slope)
            if np.mean(sims) < STEP_TOL: flags.append('steps')
        row['flags'] = ' '.join(flags)
        rows.append(row); print(row['wave_select'], row['name'], row['cents_vs_target'], row.get('step_sim_expected', ''), row['flags'], flush=True)
    cols = ['wave_select', 'name', 'zone', 'keys', 'expected_hz', 'measured_hz', 'target_hz', 'cents_vs_target', 'step_sim_expected', 'step_sim_min',
            'step_best_match_rate', 'frames', 'slope_over_128', 'record_hw_recording_c', 'record_hw_step_similarity', 'flags']
    with open(os.path.join(odir, 'regression.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore'); w.writeheader(); w.writerows(rows)
    bad = [r for r in rows if r['flags']]
    print('waves checked: %d; flagged: %d' % (len(rows), len(bad)))
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
