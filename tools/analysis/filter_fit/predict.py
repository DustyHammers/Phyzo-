import sys; sys.path.insert(0,'/home/claude/w')
from fitlib import *
from an import load
import numpy as np, pickle, csv, scipy.io.wavfile as wf
d=pickle.load(open('/home/claude/w/fitfull.pkl','rb')); x=d['x']; nT=d['nT']; used=d['used']
tab=[r for r in csv.DictReader(open('resonance_table.csv'))]
B=np.array([float(r['regB_decoded']) for r in tab])
r_,c_=d['rc']; g=d['g']
lk=x[:nT].copy()
# fill unused frames from nearest used
uu=np.array(used); 
for t in range(nT):
    if t not in set(used): lk[t]=lk[uu[np.argmin(abs(uu-t))]]
src=load('/home/claude/w/ds/reslp_q000.wav')[1]
win=np.hanning(N); fbin=np.arange(N//2+1)*SR/N; fbin[0]=1.0
def Hres(ff,k,i):
    k2=min(k*r_,1.0); return svf(ff,k,c_*B[i])*svf(ff,k2,c_*B[i])
def Hnp(ff,k,mode):
    k2=min(k*r_,1.0); return onepole(ff,k)**2*onepole(ff,k2)**2 if mode==2 else onepole(ff,k)**3*onepole(ff,k2)
def predict(kind,arg,gain_db):
    out=np.zeros(len(src)+N); wsum=np.zeros(len(src)+N)
    for t in range(nT):
        seg=src[t*HOP:t*HOP+N]
        if len(seg)<N: break
        S=np.fft.rfft(seg*win); k=np.exp(lk[t])
        H=(Hres(fbin,k,arg) if kind=='res' else Hnp(fbin,k,arg))/Hres(fbin,k,0)
        y=np.fft.irfft(S*H,N)*win
        out[t*HOP:t*HOP+N]+=y; wsum[t*HOP:t*HOP+N]+=win**2
    out=out[:len(src)]/np.maximum(wsum[:len(src)],1e-3)
    return out*10**(gain_db/20)
P={}
for name,kind,arg,gi in [('reslp_q050','res',24,0),('reslp_q075','res',36,1),('reslp_q100','res',49,2),('lp2lp2','np',2,3),('lp3lp1','np',3,4)]:
    raw=predict(kind,arg,0)
    gpred=20*np.log10(np.abs(src).max()/np.abs(raw).max())
    P[name]=raw*10**(g[gi]/20)
    rec=load(f'/home/claude/w/ds/{name}.wav')[1]
    n=min(len(rec),len(P[name]))
    # log-spectral distance per frame (dB), frames with energy
    def spec(z):
        idx=np.arange(N)[None,:]+HOP*np.arange((n-N)//HOP)[:,None]; return 20*np.log10(np.abs(np.fft.rfft(z[idx]*win,axis=1))+1e-7)
    Sr,Sp=spec(rec[:n]),spec(P[name][:n]); m=Sr>Sr.max(1,keepdims=True)-60
    lsd=np.sqrt(np.mean(np.where(m,(Sr-Sp)**2,0),1)*m.shape[1]/np.maximum(m.sum(1),1))
    print(f'{name:11s} fitted level offset {g[gi]:6.2f} dB  predicted from peak normalisation {gpred:6.2f} dB  log-spectral distance median {np.median(lsd):.2f} dB')
    wf.write(f'/home/claude/w/pred_{name}.wav',SR,P[name].astype(np.float32))
pickle.dump(P,open('/home/claude/w/pred.pkl','wb'))
