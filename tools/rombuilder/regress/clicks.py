#!/usr/bin/env python3
"""Step-boundary click count for refplayer sweep renders.

usage: clicks.py RENDER_DIR [OUT.csv]
For every Start Index change that actually changes the loop window (table: frame changes; stretch: always),
take the point where the new window took effect (sweep_switches.csv; if absent, the nominal step boundary).
D = largest |second difference| within +/-1 sample of that point.
R = largest |second difference| in the surrounding steady signal (up to 0.1 s each side, excluding +/-2 samples),
    which already contains the ordinary loop wraps of both the old and the new frame.
Two criteria are reported:
  jump  (amplitude click): |first difference| at the boundary exceeds the largest |first difference| in the
        surrounding steady signal - a sample step bigger than anything the steady loops (including their wraps) make.
  slope (strict): |second difference| at the boundary exceeds the steady maximum - also catches a waveform
        stopping or bending at the boundary without any amplitude step (e.g. a cycle ending at a zero crossing
        into a silent frame).
"""
import csv, os, struct, sys
import numpy as np

def read_wav(path):
    b = open(path, 'rb').read(); pos = 12
    while pos + 8 <= len(b):
        cid = b[pos:pos + 4]; sz = struct.unpack_from('<I', b, pos + 4)[0]
        if cid == b'data': return np.frombuffer(b[pos + 8:pos + 8 + sz], dtype='<i2').astype(float)
        pos += 8 + sz + (sz & 1)

def analyse(rdir):
    log = [r for r in csv.DictReader(open(os.path.join(rdir, 'render_log.csv'))) if r['sweep_file']]
    sw = {}
    p = os.path.join(rdir, 'sweep_switches.csv')
    if os.path.exists(p):
        for r in csv.DictReader(open(p)): sw.setdefault(r['sweep_file'], {})[int(r['index'])] = int(r['applied_sample'])
    out = []
    for r in log:
        x = read_wav(os.path.join(rdir, r['sweep_file'])); n = len(x) // 128
        N = int(r['frame_count']); table = r['sweep_type'] == '1'
        d2 = np.abs(np.diff(x, 2)); d2 = np.concatenate([[0], d2, [0]])  # d2[k] = |x[k+1]-2x[k]+x[k-1]|
        d1 = np.abs(np.diff(x)); d1 = np.concatenate([[0], d1])           # d1[k] = |x[k]-x[k-1]|
        pts = [(i, sw.get(r['sweep_file'], {}).get(i, i * n)) for i in range(1, 128)]
        changed, clicks, worst, jumps, worstj = 0, [], 0.0, [], 0.0
        for k, (i, s) in enumerate(pts):
            if table and (i * N) // 128 == ((i - 1) * N) // 128: continue
            changed += 1
            lo = max(pts[k - 1][1] + 3 if k else 3, s - 4410); hi = min(pts[k + 1][1] - 3 if k + 1 < len(pts) else len(x) - 3, s + 4410)
            D = d2[s - 1:s + 2].max()
            R = max(d2[lo:s - 2].max() if s - 2 > lo else 0, d2[s + 3:hi].max() if hi > s + 3 else 0)
            ratio = D / R if R > 0 else (np.inf if D > 0 else 0.0)
            worst = max(worst, ratio)
            if D > R: clicks.append(i)
            J = d1[s - 1:s + 2].max()
            RJ = max(d1[lo:s - 2].max() if s - 2 > lo else 0, d1[s + 3:hi].max() if hi > s + 3 else 0)
            worstj = max(worstj, J / RJ if RJ > 0 else (np.inf if J > 0 else 0.0))
            if J > RJ: jumps.append(i)
        out.append(dict(wave_select=r['wave_select'], name=r['name'], sweep_type='table' if table else 'stretch', frames=N if table else 128,
                        window_changes=changed, jump_clicks=len(jumps), jump_indices=' '.join(map(str, jumps)), jump_worst_ratio='%.2f' % worstj,
                        slope_clicks=len(clicks), slope_indices=' '.join(map(str, clicks)), slope_worst_ratio='%.2f' % worst))
    return out

if __name__ == '__main__':
    rows = analyse(sys.argv[1])
    if len(sys.argv) > 2:
        with open(sys.argv[2], 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    print('sweeps %d, window changes %d, jump clicks %d, slope clicks %d' % (len(rows), sum(r['window_changes'] for r in rows),
          sum(r['jump_clicks'] for r in rows), sum(r['slope_clicks'] for r in rows)))
