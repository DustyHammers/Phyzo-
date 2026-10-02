import sys; sys.path.insert(0,'/home/claude/w')
from fitlib import *
import numpy as np, pickle, csv
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix
M,_=pickle.load(open('/home/claude/w/stft.pkl','rb'))
tab=[r for r in csv.DictReader(open('resonance_table.csv'))]
A=np.array([float(r['regA_decoded']) for r in tab]); B=np.array([float(r['regB_decoded']) for r in tab])
ref='reslp_q000'
files=['reslp_q050','reslp_q075','reslp_q100','lp2lp2','lp3lp1']
nfr=int(7.3*SR/HOP)
frames=list(range(nfr))
data=[]
for t in frames:
    pk=peaks_frame(M[ref][t],rel=65,floor=-100,fmax=5000)
    a0=amp_at(M[ref][t],pk)
    for fi,fn in enumerate(files):
        a=amp_at(M[fn][t],pk); ok=(20*np.log10(a+1e-12)>-105)
        for p in np.where(ok)[0]: data.append((t,fi,fq[pk[p]],20*np.log10(a[p]/a0[p])))
data=np.array(data)
T=data[:,0].astype(int); F=data[:,1].astype(int); f=data[:,2]; y=data[:,3]; nT=nfr
idx=np.array([24,36,49])
def db(x): return 20*np.log10(np.abs(x)+1e-30)
def model(p):
    lk=p[:nT]; g=p[nT:nT+5]; s,r,c=np.exp(p[nT+5:nT+8])
    k1=np.exp(lk[T]); k2=np.minimum(k1*r,1.0)
    ks1=np.minimum(s*k1,1.2); ks2=np.minimum(s*k2,1.2)
    def res_mode(i): return db(svf(f,ks1,c*B[i]))+db(svf(f,ks2,c*B[i]))
    H0=res_mode(np.zeros_like(F))
    out=np.empty_like(y)
    m=F<3; out[m]=(res_mode(idx[np.minimum(F,2)]))[m]
    m=F==3; out[m]=(2*db(onepole(f,k1))+2*db(onepole(f,k2)))[m]   # 2LP/2LP: K1,K1,K2,K2
    m=F==4; out[m]=(3*db(onepole(f,k1))+db(onepole(f,k2)))[m]     # 3LP/1LP: K1 x3, K2
    return g[F]+out-H0
def fit(fix=None,files_used=(0,1,2,3,4)):
    use=np.isin(F,files_used)
    def res(p):
        if fix:
            for j,v in fix.items(): p=p.copy(); p[nT+5+j]=v
        return np.clip((model(p)-y)[use],-40,40)
    p0=np.concatenate([np.full(nT,np.log(0.03)),[-7,-13,-25,0,0],[0,0,0]])
    lb=np.concatenate([np.full(nT,np.log(5e-4)),[-80]*5,[np.log(0.2)]*3]); ub=np.concatenate([np.full(nT,np.log(1.0)),[40]*5,[np.log(5)]*3])
    J=lil_matrix((use.sum(),len(p0)),dtype=int); rows=np.arange(use.sum())
    J[rows,T[use]]=1; J[rows,nT+F[use]]=1; J[:,nT+5:nT+8]=1
    r=least_squares(res,p0,bounds=(lb,ub),loss='soft_l1',f_scale=3,max_nfev=300,jac_sparsity=J,x_scale='jac')
    rr=res(r.x)
    return r.x,rr
if __name__=='__main__':
    x,rr=fit()
    print('all: rms %.2f med %.2f'%(np.sqrt(np.mean(rr**2)),np.median(abs(rr))),'g',np.round(x[nT:nT+5],1),'s,r,c',np.round(np.exp(x[nT+5:]),3))
    use=np.isin(F,(0,1,2,3,4))
    for fi in range(5):
        m=F[use]==fi; print(' file',fi,'rms %.2f med %.2f n %d'%(np.sqrt(np.mean(rr[m]**2)),np.median(abs(rr[m])),m.sum()))
    pickle.dump((x,rr,data),open('/home/claude/w/fit3_res.pkl','wb'))
