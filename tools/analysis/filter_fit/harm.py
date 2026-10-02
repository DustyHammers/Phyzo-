import sys; sys.path.insert(0,'/home/claude/w')
from an import load
import numpy as np, scipy.signal as ss
SR=44100; N=8192; HOP=1024
win=np.hanning(N)
def stft(x):
    nf=(len(x)-N)//HOP
    idx=np.arange(N)[None,:]+HOP*np.arange(nf)[:,None]
    return np.abs(np.fft.rfft(x[idx]*win,axis=1))/ (win.sum()/2)
def f0_track(M, fmin=27, fmax=220, H=12):
    # harmonic sieve on log magnitude
    bins=np.arange(M.shape[1])*SR/N
    cands=np.exp(np.linspace(np.log(fmin),np.log(fmax),600))
    L=np.log(M+1e-7)
    out=[]
    for fr in range(M.shape[0]):
        best=-1e9;bf=0
        for f in cands:
            hs=np.arange(1,H+1)*f
            b=np.round(hs*N/SR).astype(int)
            v=np.max(np.stack([L[fr,b-1],L[fr,b],L[fr,b+1]]),0).mean()
            if v>best: best=v;bf=f
        out.append(bf)
    return np.array(out)
def refine(M,fr,f0):
    # parabolic refine on fundamental using several harmonics
    est=[]
    for h in range(1,6):
        b=int(round(h*f0*N/SR)); seg=M[fr,b-3:b+4]; i=np.argmax(seg)+b-3
        a,bv,c=np.log(M[fr,i-1:i+2]+1e-9); p=0.5*(a-c)/(a-2*bv+c) if (a-2*bv+c)!=0 else 0
        est.append((i+p)*SR/N/h)
    return np.median(est)
def harm_amps(M,fr,f0,fmax=8000):
    hs=np.arange(1,int(fmax/f0)+1)
    out=[]
    for h in hs:
        b=h*f0*N/SR; i0=int(np.floor(b))
        seg=M[fr,i0-1:i0+3]; out.append(seg.max())
    return hs*f0,np.array(out)
