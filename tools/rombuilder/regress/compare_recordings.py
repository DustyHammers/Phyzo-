#!/usr/bin/env python3
"""Step-boundary analysis of Start Index sweeps: hardware recordings vs reference-player renders.

usage: compare_recordings.py ROM.bin RENDER_LOG.csv OUT_DIR LABEL=DIR[:PATTERN] [LABEL=DIR[:PATTERN] ...]
  Each LABEL=DIR names a set of sweep files (WAV, any rate). PATTERN maps a wave to its file:
  '{sel:02d}' and '{file}' (render_log sweep file stem) are substituted; the first file in DIR whose name contains
  the pattern is used. Example: rec=recs:'<recording prefix> {sel:02d} '   new=renders_mp3:'{file}'

Per sweep:
  1. Loop period P: residual energy of x[n] - x[n-P] minimised (fractional delay by FFT) around the expected loop rate,
     coarse +/-160 c on 0.5 s, then golden-section on the whole file.
  2. Window switches: bursts of the residual (x[n]-x[n-P] stays ~0 while the loop window is unchanged). Each switch
     instant t is located to the sample by minimising sum_{n<t}(x[n]-x[n-P])^2 + sum_{n>=t}(x[n]-x[n+P])^2.
  3. Wrap alignment: phases (t mod P)/P; mean resultant length R (1 = every switch at the same loop phase, i.e. at the
     loop wrap; ~0 = switches anywhere in the cycle) and the circular spread in samples.
  4. Clicks at each switch, same criteria as clicks.py: jump (|1st difference| above the steady maximum within
     +/-0.1 s) and slope (|2nd difference| above the steady maximum).
  5. Frame mapping: harmonic fingerprint of the steady signal before the first and after the last switch vs every
     source frame of the zone that plays C4 (table zones), giving the first/last frame the sweep plays.
"""
import csv, math, os, struct, sys
import numpy as np
from scipy.io import wavfile

def load_rom(path):
    b = open(path, 'rb').read()
    zc, zo, zs, po, ps = struct.unpack_from('<5I', b, 60)
    pcm = np.frombuffer(b, dtype='<i2', count=ps, offset=po).astype(float)
    zones = {}
    for i in range(zc):
        z = struct.unpack_from('<HBBBBBBiIIIIIIIIIIhh', b, zo + i * zs)
        zones[(z[0], z[1])] = dict(loop=z[4], sweep=z[5], off=z[9], n=z[10], ls=z[12], le=z[13], W=z[14], N=z[15])
    return pcm, zones

def read(path):
    sr, x = wavfile.read(path)
    x = x.astype(float)
    if x.ndim > 1: x = x.mean(axis=1)
    m = np.abs(x).max()
    return sr, (x / m if m > 0 else x)

def delayer(x):
    n = len(x); nf = 1 << int(math.ceil(math.log2(n + 1)))
    X = np.fft.rfft(x, nf); f = np.fft.rfftfreq(nf)
    return lambda d: np.fft.irfft(X * np.exp(-2j * np.pi * f * d), nf)[:n]

def resid_energy(dl, x, P, lo, hi):
    y = dl(P); return float(np.sum((x[lo:hi] - y[lo:hi]) ** 2))

def find_period(x, sr, f_exp):
    P0 = sr / f_exp
    mid = len(x) // 2; seg = x[max(0, mid - sr // 4): mid + sr // 4]
    ds = delayer(seg); lo, hi = int(P0 * 2) + 2, len(seg)
    cs = np.arange(-160, 160.1, 2.0)
    e = [resid_energy(ds, seg, P0 * 2 ** (-c / 1200), lo, hi) for c in cs]
    c0 = cs[int(np.argmin(e))]
    dl = delayer(x); lo, hi = int(P0 * 2) + 2, len(x)
    a, b = P0 * 2 ** (-(c0 + 2.5) / 1200), P0 * 2 ** (-(c0 - 2.5) / 1200)
    g = (math.sqrt(5) - 1) / 2
    c, d = b - g * (b - a), a + g * (b - a); fc, fd = resid_energy(dl, x, c, lo, hi), resid_energy(dl, x, d, lo, hi)
    for _ in range(28):
        if fc < fd: b, d, fd = d, c, fc; c = b - g * (b - a); fc = resid_energy(dl, x, c, lo, hi)
        else: a, c, fc = c, d, fd; d = a + g * (b - a); fd = resid_energy(dl, x, d, lo, hi)
    P = (a + b) / 2
    return P, dl, abs(c0) >= 158, 1200 * math.log2(P0 / P)

def body_range(x):
    rms = np.sqrt(np.convolve(x ** 2, np.ones(480) / 480, mode='same'))
    act = rms > 1e-4
    if not act.any(): return 0, len(x) - 1
    idx = np.nonzero(rms > 0.1 * np.median(rms[act]))[0]
    return int(idx[0]), int(idx[-1])

def switches(x, P, dl):
    back = x - dl(P); fwd = x - dl(-P)
    n = len(x); back[:int(P) + 2] = 0; fwd[n - int(P) - 2:] = 0
    w = max(4, int(P / 8)); k = np.ones(w) / w
    sm = np.sqrt(np.convolve(back ** 2, k, mode='same'))
    act = x ** 2 > 1e-8
    floor = np.median(sm[act]) if act.any() else 1e-9
    thr = max(8 * floor, 0.05 * np.percentile(sm, 99.9))
    # sustained body of the note: exclude attack and release, whose envelopes also leave residual
    a0, a1 = body_range(x)
    b0, b1 = a0 + 3 * int(P) + 960, a1 - 3 * int(P) - 960
    sm[:max(0, b0)] = 0; sm[max(0, b1):] = 0
    above = np.nonzero(sm > thr)[0]
    if len(above) == 0: return [], thr
    bursts = []; s0 = above[0]; prev = above[0]
    for i in above[1:]:
        if i - prev > P / 2: bursts.append((s0, prev)); s0 = i
        prev = i
    bursts.append((s0, prev))
    a2, b2 = back ** 2, fwd ** 2
    out = []
    for bs, be in bursts:
        if be - bs < P / 4: continue
        lo, hi = max(1, int(bs - 1.5 * P)), min(n - 1, int(bs + 1.5 * P))
        ca = np.concatenate([[0], np.cumsum(a2[lo:hi])]); cb = np.concatenate([[0], np.cumsum(b2[lo:hi])])
        cand = np.arange(max(lo, int(bs - P / 2)), min(hi, int(bs + P / 2)))
        cost = ca[cand - lo] + (cb[-1] - cb[cand - lo])
        out.append(int(cand[int(np.argmin(cost))]))
    return out, thr

def clicks(x, ts, sr, margins=(1.0, 1.25, 1.5)):
    """Counts of jump/slope clicks at margins (boundary value > margin x steady maximum), worst ratios, and the
    switch numbers (1-based, in sweep order) flagged at margin 1.0."""
    d1 = np.concatenate([[0], np.abs(np.diff(x))]); d2 = np.concatenate([[0], np.abs(np.diff(x, 2)), [0]])
    nb = int(0.1 * sr); rj, rs = [], []
    for k, t in enumerate(ts):
        lo = max(ts[k - 1] + 3 if k else 3, t - nb); hi = min(ts[k + 1] - 3 if k + 1 < len(ts) else len(x) - 3, t + nb)
        def ref(d): return max(d[lo:t - 2].max() if t - 2 > lo else 0, d[t + 3:hi].max() if hi > t + 3 else 0)
        Rj, Rs = ref(d1), ref(d2)
        rj.append(d1[t - 1:t + 2].max() / Rj if Rj > 0 else 0.0); rs.append(d2[t - 1:t + 2].max() / Rs if Rs > 0 else 0.0)
    out = {}
    for m in margins:
        out['jump>%.2f' % m] = sum(r > m for r in rj); out['slope>%.2f' % m] = sum(r > m for r in rs)
    out['jump_worst'] = '%.2f' % max(rj, default=0); out['slope_worst'] = '%.2f' % max(rs, default=0)
    out['jump_at'] = ' '.join(str(k + 1) for k, r in enumerate(rj) if r > 1.0)
    out['slope_at'] = ' '.join(str(k + 1) for k, r in enumerate(rs) if r > 1.0)
    return out

def fp_signal(seg, sr, f0, H):
    t = np.arange(len(seg)) / sr; w = np.hanning(len(seg)); k = np.arange(1, H + 1)[:, None]
    return np.abs(((seg * w)[None, :] * np.exp(-2j * np.pi * k * f0 * t[None, :])).sum(axis=1))

def fp_frame(fr, H):
    n = len(fr); k = np.arange(1, H + 1)[:, None]; j = np.arange(n)[None, :]
    return np.abs((fr[None, :] * np.exp(-2j * np.pi * k * j / n)).sum(axis=1))

def cosv(a, b):
    na, nb = np.linalg.norm(a), np.linalg.norm(b); return float(a @ b / (na * nb)) if na > 0 and nb > 0 else 0.0

def frame_match(x, sr, P, ts, z, pcm):
    if z['sweep'] != 1: return '', ''
    f0 = sr / P; H = int(min(32, (sr / 2 - 200) / f0, z['W'] // 2))
    frames = []
    for f in range(z['N']):
        fr = pcm[z['off'] + z['ls'] + f * z['W']: z['off'] + z['ls'] + (f + 1) * z['W']]
        if z['loop'] == 2: fr = np.concatenate([fr, fr[::-1]])
        frames.append(fp_frame(fr, H))
    a0, a1 = body_range(x)
    f_end = min(ts[0] - 5, a0 + int(0.17 * sr)) if ts else a0 + int(0.17 * sr)
    first = x[a0 + int(0.02 * sr): f_end]
    l_start = max(ts[-1] + int(P) + 5, a1 - int(0.2 * sr)) if ts else a1 - int(0.2 * sr)
    last = x[l_start: a1 - int(0.05 * sr)]
    res = []
    for seg in (first, last):
        if len(seg) < 4 * P: res.append('?'); continue
        fp = fp_signal(seg, sr, f0, H)
        c = [cosv(fp, fr) for fr in frames]; b = int(np.argmax(c)); res.append('%d (%.3f)' % (b, c[b]))
    return res[0], res[1]

def main():
    rom, logp, outd = sys.argv[1:4]
    sets = []
    for a in sys.argv[4:]:
        lab, rest = a.split('=', 1); d, _, pat = rest.partition(':'); sets.append((lab, d, pat or '{file}'))
    pcm, zones = load_rom(rom)
    log = [r for r in csv.DictReader(open(logp)) if r['sweep_file']]
    only = os.environ.get('ONLY')  # optional comma list of wave_select numbers
    if only: log = [r for r in log if r['wave_select'] in only.split(',')]
    rows = []
    for r in log:
        sel = int(r['wave_select']); z = zones[(sel, int(r['zone']))]
        stem = r['sweep_file'][:-4]
        row = dict(wave_select=sel, name=r['name'], sweep='table' if z['sweep'] == 1 else 'stretch', frames=z['N'] if z['sweep'] == 1 else 128)
        for lab, d, pat in sets:
            key = pat.format(sel=sel, file=stem)
            fs = sorted(f for f in os.listdir(d) if key in f and f.lower().endswith('.wav'))
            if not fs: row[lab + '_file'] = 'missing'; continue
            sr, x = read(os.path.join(d, fs[0]))
            P, dl, edge, dc = find_period(x, sr, float(r['expected_loop_hz']))
            ts, thr = switches(x, P, dl)
            if len(ts) >= 2:
                ph = np.exp(2j * np.pi * (np.array(ts) % P) / P); R = abs(ph.mean())
                spread = math.sqrt(max(0.0, -2 * math.log(max(R, 1e-12)))) * P / (2 * math.pi)
            else: R, spread = float('nan'), float('nan')
            ck = clicks(x, ts, sr)
            f_first, f_last = frame_match(x, sr, P, ts, z, pcm)
            a0, _ = body_range(x)
            row.update({lab + '_cents': '%+.1f' % dc + (' EDGE' if edge else ''), lab + '_period': '%.3f' % P, lab + '_switches': len(ts),
                        lab + '_wrapR': '%.3f' % R, lab + '_spread_smp': '%.2f' % spread, lab + '_lead_silence_s': '%.3f' % (a0 / sr),
                        lab + '_first_frame': f_first, lab + '_last_frame': f_last})
            row.update({lab + '_' + k: v for k, v in ck.items()})
        rows.append(row); print(sel, r['name'], flush=True)
    keys = list(dict.fromkeys(k for r in rows for k in r))
    with open(os.path.join(outd, 'recording_comparison.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=keys); w.writeheader(); w.writerows(rows)

if __name__ == '__main__':
    main()
