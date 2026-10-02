import sys; sys.path.insert(0,'/home/claude/w')
from fitlib import *
from harm import stft
from an import load
import numpy as np, pickle, csv
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix
names=['reslp_q000','reslp_q050','reslp_q075','reslp_q100','lp2lp2','lp3lp1']
X={n:load(f'/home/claude/w/ds/{n}.wav')[1] for n in names}
M={n:stft(X[n]) for n in names}
tab=[r for r in csv.DictReader(open('resonance_table.csv'))]
A=np.array([float(r['regA_decoded']) for r in tab]); B=np.array([float(r['regB_decoded']) for r in tab])
ref='reslp_q000'; files=names[1:]; idx=np.array([24,36,49])
nT=M[ref].shape[0]
data=[]
for t in range(nT):
    if 20*np.log10(M[ref][t].max()+1e-12) < -80: continue
    pk=peaks_frame(M[ref][t],rel=65,floor=-100,fmax=6000); a0=amp_at(M[ref][t],pk)
    for fi,fn in enumerate(files):
        a=amp_at(M[fn][t],pk); ok=(20*np.log10(a+1e-12)>-105)
        for p in np.where(ok)[0]: data.append((t,fi,fq[pk[p]],20*np.log10(a[p]/a0[p])))
data=np.array(data); T=data[:,0].astype(int); F=data[:,1].astype(int); f=data[:,2]; y=data[:,3]
def db(x): return 20*np.log10(np.abs(x)+1e-30)
def Hres(ff,k1,k2,i,c=1.0,bp=False):
    o='bp' if bp else 'lp'
    return svf(ff,k1,c*B[i],o)*svf(ff,k2,c*B[i],o)
def Hnp(ff,k1,k2,mode):
    return onepole(ff,k1)**2*onepole(ff,k2)**2 if mode==2 else onepole(ff,k1)**3*onepole(ff,k2)
def model(p):
    lk=p[:nT]; g=p[nT:nT+5]; r,c=np.exp(p[nT+5:nT+7])
    k1=np.exp(lk[T]); k2=np.minimum(k1*r,1.0)
    H0=db(Hres(f,k1,k2,0,c)); out=np.empty_like(y)
    m=F<3; out[m]=db(Hres(f,k1,k2,idx[np.minimum(F,2)],c))[m]
    m=F==3; out[m]=db(Hnp(f,k1,k2,2))[m]; m=F==4; out[m]=db(Hnp(f,k1,k2,3))[m]
    return g[F]+out-H0
def res(p): return np.clip(model(p)-y,-40,40)
g0=np.array([-7.1,-12.9,-25.4,1.5,2.2]); r0,c0=1.13,1.025
grid=np.exp(np.linspace(np.log(6e-4),np.log(0.99),70))
lk0=np.full(nT,np.log(0.03))
best=np.full(nT,np.inf)
for kk in grid:
    k1=np.full(len(y),kk); k2=np.minimum(k1*r0,1.0)
    H0=db(Hres(f,k1,k2,0,c0)); out=np.empty_like(y)
    m=F<3; out[m]=db(Hres(f,k1,k2,idx[np.minimum(F,2)],c0))[m]
    m=F==3; out[m]=db(Hnp(f,k1,k2,2))[m]; m=F==4; out[m]=db(Hnp(f,k1,k2,3))[m]
    e=np.minimum(np.abs(g0[F]+out-H0-y),15)
    cost=np.bincount(T,weights=e,minlength=nT)
    better=cost<best; best[better]=cost[better]; lk0[better]=np.log(kk)
p0=np.concatenate([lk0,g0,[np.log(r0),np.log(c0)]])
lb=np.concatenate([np.full(nT,np.log(5e-4)),[-80]*5,[np.log(0.2)]*2]); ub=np.concatenate([np.full(nT,np.log(1.0)),[40]*5,[np.log(5)]*2])
J=lil_matrix((len(y),len(p0)),dtype=int); rows=np.arange(len(y)); J[rows,T]=1; J[rows,nT+F]=1; J[:,nT+5:]=1
r=least_squares(res,p0,bounds=(lb,ub),loss='soft_l1',f_scale=3,max_nfev=120,jac_sparsity=J,x_scale='jac')
rr=res(r.x)
print('points',len(y),'frames used',len(set(T)))
print('g',np.round(r.x[nT:nT+5],2),'K2/K1, damping scale',np.round(np.exp(r.x[nT+5:]),3))
stats=[]
for fi,fn in enumerate(files):
    m=F==fi; e=rr[m]
    stats.append((fn,len(e),np.median(abs(e)),np.sqrt(np.mean(e**2)),np.percentile(abs(e),90)))
    print('%-11s n %5d  median|err| %.2f dB  rms %.2f dB  p90 %.2f dB'%stats[-1])
# section split: bass 0-7.3 s, rest
tsec=T*HOP/SR
for fi,fn in enumerate(files[:3]):
    for lo,hi in [(0,7.3),(7.3,22),(22,31)]:
        m=(F==fi)&(tsec>=lo)&(tsec<hi); print('  %s %.1f-%.1f s median %.2f rms %.2f'%(fn,lo,hi,np.median(abs(rr[m])),np.sqrt(np.mean(rr[m]**2))))
used=sorted(set(T))
pickle.dump(dict(x=r.x,nT=nT,used=used,stats=stats,g=r.x[nT:nT+5],rc=np.exp(r.x[nT+5:])),open('/home/claude/w/fitfull.pkl','wb'))
