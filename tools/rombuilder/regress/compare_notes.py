#!/usr/bin/env python3
"""Per-note comparison of a key-sequence render with a hardware recording of the same notes.

usage: compare_notes.py REC.wav RENDER.wav FIRST,LAST,STEP SPACING_S OUT.csv
Both files hold one note per SPACING_S seconds, keys FIRST..LAST in steps of STEP (refplayer --keyseq).
Per note: loop-period fit of the pitch in each file (compare_recordings.find_period), cents render vs recording;
harmonic fingerprint similarity (cosine of harmonic magnitudes up to 19 kHz, max 40 harmonics); and RMS level.
Recordings are usually normalised per file, so levels are compared as a profile: each note's level difference minus
the median difference over all notes.
"""
import csv, math, sys
import numpy as np
from scipy.io import wavfile
import compare_recordings as cr

def load(p):
    sr, x = wavfile.read(p); x = x.astype(float)
    if x.ndim > 1: x = x.mean(axis=1)
    return sr, x

def note(x, sr, i, sp):
    return x[int((i * sp + 0.06) * sr): int((i * sp + 0.36) * sr)]

def fingerprint(seg, sr, f0, H):
    t = np.arange(len(seg)) / sr; w = np.hanning(len(seg)); k = np.arange(1, H + 1)[:, None]
    return np.abs(((seg * w)[None, :] * np.exp(-2j * np.pi * k * f0 * t[None, :])).sum(axis=1))

def main():
    rec, ren, ks, sp, out = sys.argv[1:6]
    a, b, c = (int(v) for v in ks.split(',')); sp = float(sp)
    srr, xr = load(rec); srn, xn = load(ren)
    rows = []
    for i, key in enumerate(range(a, b + 1, c)):
        sr_ = note(xr, srr, i, sp); sn_ = note(xn, srn, i, sp)
        if len(sr_) < 1000 or len(sn_) < 1000 or np.abs(sn_).max() == 0: continue
        f_guess = 440 * 2 ** ((key - 69) / 12)
        # pitch: render first (it defines the expected loop rate), then the recording around it
        fn = srn / cr.find_period(sn_ / np.abs(sn_).max(), srn, cr_guess(sn_, srn, f_guess))[0]
        fr = srr / cr.find_period(sr_ / np.abs(sr_).max(), srr, fn)[0]
        H = int(max(1, min(40, 19000 / fn, (min(srr, srn) / 2 - 300) / fn)))
        sim = cr.cosv(fingerprint(sr_, srr, fr, H), fingerprint(sn_, srn, fn, H))
        lr = 20 * math.log10(np.sqrt(np.mean(sr_ ** 2)) + 1e-12); ln = 20 * math.log10(np.sqrt(np.mean(sn_ ** 2)) + 1e-12)
        rows.append(dict(key=key, render_hz='%.4f' % fn, rec_hz='%.4f' % fr, cents_rec_vs_render='%+.2f' % (1200 * math.log2(fr / fn)),
                         harmonics=H, similarity='%.4f' % sim, rec_db=lr, render_db=ln))
    d = [r['rec_db'] - r['render_db'] for r in rows]; med = float(np.median(d))
    for r, v in zip(rows, d):
        r['level_dev_db'] = '%+.2f' % (v - med); r['rec_db'] = '%.2f' % r['rec_db']; r['render_db'] = '%.2f' % r['render_db']
    with open(out, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    c_ = [abs(float(r['cents_rec_vs_render'])) for r in rows]; s_ = [float(r['similarity']) for r in rows]; l_ = [abs(float(r['level_dev_db'])) for r in rows]
    print('%s: %d notes, |cents| max %.2f median %.2f; similarity min %.4f median %.4f; level profile max dev %.2f dB (offset %.2f dB)'
          % (out, len(rows), max(c_), float(np.median(c_)), min(s_), float(np.median(s_)), max(l_), med))

def cr_guess(seg, sr, f_guess):
    """coarse loop rate from the render itself (harmonic sum over +/-1 octave around the key's nominal pitch)"""
    w = seg * np.hanning(len(seg)); nfft = 1 << 19; X = np.abs(np.fft.rfft(w, nfft)); df = sr / nfft
    cand = f_guess * 2 ** (np.arange(-2400, 2401, 5) / 1200); best = (0, f_guess)
    for f in cand:
        h = np.arange(1, 9) * f; h = h[h < sr / 2 - 300]
        if not len(h): continue
        s = np.interp(h / df, np.arange(len(X)), X).sum() * (np.interp(f / df, np.arange(len(X)), X) > 0.05 * X.max())
        if s > best[0]: best = (s, f)
    return best[1]

if __name__ == '__main__':
    main()
