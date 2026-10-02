# Held-out validation battery (see PREREGISTRATION.md). Usage: python3 validate.py <TOPO>
import sys, os, pickle, csv, time
sys.path.insert(0,'/home/claude/w')
import numpy as np
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix, identity
from fitlib import svf, onepole, peaks_frame, amp_at, SR, N, HOP, fq
from harm import stft
from an import load
TOPO=sys.argv[1]; OUT=f'/home/claude/w/validation/res_{TOPO}.pkl'
NAMES=['reslp_q000','reslp_q050','reslp_q075','reslp_q100','lp2lp2','lp3lp1']; FILES=NAMES[1:]
IDX=np.array([24,36,49])
rows=list(csv.DictReader(open('resonance_table.csv')))
A=np.array([float(r['regA_decoded']) for r in rows]); B=np.array([float(r['regB_decoded']) for r in rows])
cache='/home/claude/w/validation/data.pkl'
if os.path.exists(cache): D=pickle.load(open(cache,'rb'))
else:
    X={n:load(f'/home/claude/w/ds/{n}.wav')[1] for n in NAMES}; M={n:stft(X[n]) for n in NAMES}
    nT=M['reslp_q000'].shape[0]; data=[]
    for t in range(nT):
        m0=M['reslp_q000'][t]; top=20*np.log10(m0.max()+1e-12)
        if top < -80: continue
        pk=peaks_frame(m0,rel=65,floor=-100,fmax=6000); a0=amp_at(m0,pk)
        for fi,fn in enumerate(FILES):
            a=amp_at(M[fn][t],pk); ok=20*np.log10(a+1e-12)>-105
            for p in np.where(ok)[0]: data.append((t,fi,fq[pk[p]],20*np.log10(a[p]/a0[p]),top-20*np.log10(a0[p])))
    D=dict(data=np.array(data),nT=nT,q000=X['reslp_q000']); pickle.dump(D,open(cache,'wb'))
data=D['data']; nT=D['nT']; src=D['q000']
T=data[:,0].astype(int); F=data[:,1].astype(int); f=data[:,2]; y=data[:,3]; rel=data[:,4]
tsec=T*HOP/SR
db=lambda z: 20*np.log10(np.abs(z)+1e-30)
def Hres(ff,k1,k2,i):
    if TOPO=='ADOPT': return svf(ff,k1,B[i])*svf(ff,k2,B[i])
    if TOPO=='M2':    return svf(ff,k1,B[i])*svf(ff,k2,A[i])
    if TOPO=='M3':    return svf(ff,k1,B[i])*onepole(ff,k2)**2
    if TOPO=='M4':    return svf(ff,k1,B[i])
def Hnp(ff,k1,k2,mode): return onepole(ff,k1)**2*onepole(ff,k2)**2 if mode==2 else onepole(ff,k1)**3*onepole(ff,k2)
HASR = TOPO!='M4'
def shape(sel,k1,r):
    k2=np.minimum(k1*r,1.0); ff=f[sel]; Fs=F[sel]; out=np.empty(sel.sum())
    H0=db(Hres(ff,k1,k2,np.zeros(len(ff),int)))
    m=Fs<3
    if m.any(): out[m]=db(Hres(ff[m],k1[m],k2[m],IDX[Fs[m]]))
    m=Fs==3
    if m.any(): out[m]=db(Hnp(ff[m],k1[m],k2[m],2))
    m=Fs==4
    if m.any(): out[m]=db(Hnp(ff[m],k1[m],k2[m],3))
    return out-H0
GRID=np.exp(np.linspace(np.log(6e-4),np.log(0.99),70))
def soft(e): return np.minimum(np.abs(e),15)
def grid_k(sel,r,g=None):
    # per-frame grid; g=None: level-invariant (per frame & file median removed)
    frames=np.unique(T[sel]); best=np.full(nT,np.inf); lk=np.full(nT,np.nan)
    Ts=T[sel]; Fs=F[sel]
    for kk in GRID:
        e=shape(sel,np.full(sel.sum(),kk),r)-y[sel]
        if g is None:
            key=Ts*8+Fs; order=np.argsort(key); ks=key[order]; es=e[order]
            u,st=np.unique(ks,return_index=True); med=np.zeros_like(es)
            for a,b in zip(st,list(st[1:])+[len(es)]): med[a:b]=np.median(es[a:b])
            ee=np.empty_like(e); ee[order]=es-med
        else: ee=e+g[Fs]
        c=np.bincount(Ts,weights=soft(ee),minlength=nT)
        b=(c<best)&np.isin(np.arange(nT),frames); best[b]=c[b]; lk[b]=np.log(kk)
    return lk
def fit(files,frames_mask_fn):
    sel=np.isin(F,files)&frames_mask_fn(tsec); fr=np.unique(T[sel]); fmap=-np.ones(nT,int); fmap[fr]=np.arange(len(fr))
    r0=1.1 if HASR else 1.0
    lk0=grid_k(sel,r0)
    g0=np.zeros(5)
    for fi in files:
        m=sel&(F==fi); g0[fi]=np.median((y-0)[m]-shape(m,np.exp(lk0[T[m]]),r0))
    nf=len(fr); fl=list(files)
    def unpack(p):
        lk=p[:nf]; g=np.zeros(5); g[fl]=p[nf:nf+len(fl)]; r=np.exp(p[-1]) if HASR else 1.0; return lk,g,r
    def res(p):
        lk,g,r=unpack(p); k1=np.exp(lk[fmap[T[sel]]])
        return np.clip(g[F[sel]]+shape(sel,k1,r)-y[sel],-40,40)
    p0=np.concatenate([lk0[fr],g0[fl]]+([[np.log(r0)]] if HASR else []))
    lb=np.concatenate([np.full(nf,np.log(5e-4)),[-80]*len(fl)]+([[np.log(0.2)]] if HASR else []))
    ub=np.concatenate([np.full(nf,0.0),[40]*len(fl)]+([[np.log(5)]] if HASR else []))
    p0=np.clip(p0,lb+1e-9,ub-1e-9)
    J=lil_matrix((sel.sum(),len(p0)),dtype=int); rws=np.arange(sel.sum())
    J[rws,fmap[T[sel]]]=1; J[rws,nf+np.searchsorted(fl,F[sel])]=1
    if HASR: J[:,-1]=1
    sol=least_squares(res,p0,bounds=(lb,ub),loss='soft_l1',f_scale=3,max_nfev=120,jac_sparsity=J,x_scale='jac')
    lk_,g,r=unpack(sol.x); lk=np.full(nT,np.nan); lk[fr]=lk_
    return dict(lk=lk,g=g,r=r,sel=sel)
def estimate_k(files,frames_mask_fn,g,r):
    sel=np.isin(F,files)&frames_mask_fn(tsec); fr=np.unique(T[sel]); fmap=-np.ones(nT,int); fmap[fr]=np.arange(len(fr))
    lk0=grid_k(sel,r,g)
    def res(p): return np.clip(g[F[sel]]+shape(sel,np.exp(p[fmap[T[sel]]]),r)-y[sel],-40,40)
    J=lil_matrix((sel.sum(),len(fr)),dtype=int); J[np.arange(sel.sum()),fmap[T[sel]]]=1
    p0=np.clip(lk0[fr],np.log(5e-4)+1e-9,-1e-9)
    sol=least_squares(res,p0,bounds=(np.log(5e-4),0.0),loss='soft_l1',f_scale=3,max_nfev=60,jac_sparsity=J,x_scale='jac')
    lk=np.full(nT,np.nan); lk[fr]=sol.x; return lk
def residuals(file,frames_mask_fn,lk,g,r):
    sel=(F==file)&frames_mask_fn(tsec)&~np.isnan(lk[T])
    e=g[file]+shape(sel,np.exp(lk[T[sel]]),r)-y[sel]
    return np.where(sel)[0], e
def free_level(file,frames_mask_fn,lk,r):
    sel=(F==file)&frames_mask_fn(tsec)&~np.isnan(lk[T]); e=shape(sel,np.exp(lk[T[sel]]),r)-y[sel]
    sol=least_squares(lambda p: np.clip(p[0]+e,-40,40),[-np.median(e)],loss='soft_l1',f_scale=3); return sol.x[0]
def fill(lk):
    lk=lk.copy(); ok=np.where(~np.isnan(lk))[0]
    for t in np.where(np.isnan(lk))[0]: lk[t]=lk[ok[np.argmin(abs(ok-t))]]
    return lk
WIN=np.hanning(N); FB=np.arange(N//2+1)*SR/N; FB[0]=1.0
def control_level(mode,lk,r):
    lk=fill(lk); out=np.zeros(len(src)+N); ws=np.zeros(len(src)+N)
    for t in range(nT):
        seg=src[t*HOP:t*HOP+N]
        if len(seg)<N: break
        k1=np.exp(lk[t]); k2=min(k1*r,1.0)
        H=Hnp(FB,k1,k2,mode)/Hres(FB,k1,k2,0)
        z=np.fft.irfft(np.fft.rfft(seg*WIN)*H,N)*WIN; out[t*HOP:t*HOP+N]+=z; ws[t*HOP:t*HOP+N]+=WIN**2
    out=out[:len(src)]/np.maximum(ws[:len(src)],1e-3)
    return 20*np.log10(np.abs(src).max()/np.abs(out).max())
ALL=lambda s: np.ones_like(s,bool); BASS=lambda s: s<7.3; REST=lambda s: s>=7.3
R={}; t0=time.time(); log=lambda *a: print(f'[{TOPO} {time.time()-t0:6.0f}s]',*a,flush=True)
R['R0']=fit([0,1,2],ALL); log('R0',R['R0']['g'][:3],R['R0']['r'])
R['R0c']=fit([0,1,2,3,4],ALL); log('R0c',R['R0c']['g'],R['R0c']['r'])
for nm,fn in [('R0',[0,1,2]),('R0c',[0,1,2,3,4])]:
    R[nm]['res']={fi:residuals(fi,ALL,R[nm]['lk'],R[nm]['g'],R[nm]['r']) for fi in fn}
for split,train,test in [('T1',BASS,REST),('T2',REST,BASS)]:
    tr=fit([0,1,2],train); log(split,'train',tr['g'][:3],tr['r'])
    S={'train':tr,'held':{},'lk_used':{}}
    for fi in [0,1,2]:
        others=[x for x in [0,1,2] if x!=fi]
        lk=estimate_k(others,test,tr['g'],tr['r']); S['lk_used'][fi]=lk
        S['held'][fi]=residuals(fi,test,lk,tr['g'],tr['r'])
    lk3=estimate_k([0,1,2],test,tr['g'],tr['r']); lkfull=np.where(np.isnan(lk3),tr['lk'],lk3)
    for fi,mode in [(3,2),(4,3)]:
        g=tr['g'].copy(); g[fi]=control_level(mode,lkfull,tr['r'])
        S['held'][fi]=residuals(fi,test,lk3,g,tr['r']); S['lk_used'][fi]=lk3
        g2=g.copy(); g2[fi]=free_level(fi,test,lk3,tr['r']); S.setdefault('held_freelevel',{})[fi]=residuals(fi,test,lk3,g2,tr['r'])
        S.setdefault('ctrl_level',{})[fi]=(g[fi],g2[fi])
    R[split]=S; log(split,'done')
tr=fit([0,1],ALL); log('S train',tr['g'][:3],tr['r'])
S={'train':tr,'held':{},'lk_used':{}}
g=tr['g'].copy(); g[2]=free_level(2,ALL,tr['lk'],tr['r']); S['held'][2]=residuals(2,ALL,tr['lk'],g,tr['r']); S['lk_used'][2]=tr['lk']; S['q100_level']=g[2]
for fi,mode in [(3,2),(4,3)]:
    g=tr['g'].copy(); g[fi]=control_level(mode,tr['lk'],tr['r'])
    S['held'][fi]=residuals(fi,ALL,tr['lk'],g,tr['r']); S['lk_used'][fi]=tr['lk']
    g2=g.copy(); g2[fi]=free_level(fi,ALL,tr['lk'],tr['r']); S.setdefault('held_freelevel',{})[fi]=residuals(fi,ALL,tr['lk'],g2,tr['r'])
    S.setdefault('ctrl_level',{})[fi]=(g[fi],g2[fi])
R['S']=S; log('S done')
for k in ['R0','R0c']: R[k].pop('sel',None)
for k in ['T1','T2','S']: R[k]['train'].pop('sel',None)
pickle.dump(R,open(OUT,'wb')); log('saved')
