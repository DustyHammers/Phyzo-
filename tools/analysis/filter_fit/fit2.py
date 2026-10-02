import sys; sys.path.insert(0,'/home/claude/w')
from fitlib import *
import numpy as np, pickle, csv
from scipy.optimize import least_squares
frames,data=pickle.load(open('/home/claude/w/ratio_data.pkl','rb'))
tab=[r for r in csv.DictReader(open('resonance_table.csv'))]
A=np.array([float(r['regA_decoded']) for r in tab]); B=np.array([float(r['regB_decoded']) for r in tab])
fidx={t:i for i,t in enumerate(frames)}
T=np.array([fidx[int(t)] for t in data[:,0]]); F=data[:,1].astype(int); f=data[:,2]; y=data[:,3]
nT=len(frames)
def resp(model,f,k,i,c):
    if model=='M1':  # two SVF LP, damping c*B both
        return 20*np.log10(abs(svf(f,k,c[0]*B[i]))*abs(svf(f,k,c[0]*B[i])))
    if model=='M2':  # SVF(cB) -> SVF(c2 A)
        return 20*np.log10(abs(svf(f,k,c[0]*B[i]))*abs(svf(f,k,c[1]*A[i])))
    if model=='M3':  # SVF(cB) -> 2 onepoles
        return 20*np.log10(abs(svf(f,k,c[0]*B[i]))*abs(onepole(f,k))**2)
    if model=='M4':  # single SVF, damping cB
        return 20*np.log10(abs(svf(f,k,c[0]*B[i])))
def run(model,idx,nc,c0=None,ret=False):
    idx=np.array(idx)
    def res(p):
        lk=p[:nT]; g=p[nT:nT+3]; c=np.exp(p[nT+3:nT+3+nc])
        k=np.exp(lk[T])
        r=g[F]+resp(model,f,k,idx[F],c)-resp(model,f,k,np.zeros_like(F),c)-y
        return np.clip(r,-40,40)
    p0=np.concatenate([np.full(nT,np.log(0.05)),[-10,-15,-25],np.log(c0 if c0 else [1.0]*nc)])
    lb=np.concatenate([np.full(nT,np.log(1e-3)),[-80]*3,[np.log(0.2)]*nc]); ub=np.concatenate([np.full(nT,np.log(1.5)),[40]*3,[np.log(5)]*nc])
    from scipy.sparse import lil_matrix
    J=lil_matrix((len(y),len(p0)),dtype=int)
    J[np.arange(len(y)),T]=1; J[np.arange(len(y)),nT+F]=1
    for j in range(nc): J[:,nT+3+j]=1
    r=least_squares(res,p0,bounds=(lb,ub),loss='soft_l1',f_scale=3,max_nfev=200,jac_sparsity=J,x_scale='jac')
    rr=res(r.x)
    if ret: return r,rr
    return np.sqrt(np.mean(rr**2)), np.median(np.abs(rr)), r.x[nT:]
if __name__=='__main__':
    for model,nc in [('M1',1),('M2',2),('M3',1),('M4',1)]:
        out=run(model,[24,36,49],nc)
        print(model,'rms %.2f dB  med|r| %.2f  g,c'%(out[0],out[1]),np.round(out[2][:3],1),np.round(np.exp(out[2][3:]),3))
