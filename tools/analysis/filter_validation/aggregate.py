import pickle, numpy as np, json, sys
sys.argv=['x','ADOPT']
exec(open('/home/claude/w/validation/validate.py').read().split("ALL=lambda")[0])   # data, model fns (no fitting)
NM=['q050','q075','q100','lp2lp2','lp3lp1']
TOP=['ADOPT','M2','M3','M4']
R={t:pickle.load(open(f'/home/claude/w/validation/res_{t}.pkl','rb')) for t in TOP}
def med(e): return float(np.median(np.abs(e)))
out={}
for t in TOP:
    r=R[t]; out[t]={}
    for sp in ['T1','T2','S']:
        out[t][sp]={}
        for fi,(idx,e) in r[sp]['held'].items():
            ref='R0' if fi<3 else 'R0c'
            ridx,re_=r[ref]['res'][fi]; pos=np.searchsorted(ridx,idx); assert np.all(ridx[pos]==idx)
            ins=re_[pos]
            d=dict(n=len(e),held=med(e),ins=med(ins),delta=med(e)-med(ins),held_signed=float(np.median(e)),ins_signed=float(np.median(ins)))
            if fi>=3:
                i2,e2=r[sp]['held_freelevel'][fi]; d['held_freelevel']=med(e2); d['level_pred'],d['level_free']=map(float,r[sp]['ctrl_level'][fi])
            out[t][sp][NM[fi]]=d
    out[t]['train_r']={sp:float(r[sp]['train']['r']) for sp in ['T1','T2','S']}; out[t]['R0_r']=float(r['R0']['r'])
json.dump(out,open('/home/claude/w/validation/summary.json','w'),indent=1)
# criterion 1 (adopted), criterion 2
c1=[]; 
for sp in ['T1','T2','S']:
    for fn,d in out['ADOPT'][sp].items(): c1.append((sp,fn,d['n'],round(d['ins'],2),round(d['held'],2),round(d['delta'],2),d['delta']<=0.5,abs(d['delta'])<=0.5))
print('CRITERION 1 (adopted): split,file,n,in-sample median,held-out median,delta,pass(one-sided),pass(two-sided)')
for x in c1: print(*x,sep=', ')
print('C1 overall:', 'PASS' if all(x[6] for x in c1) else 'FAIL')
print('\nCRITERION 2: mean held-out median over resonant files (and incl. controls)')
c2=True
for sp in ['T1','T2','S']:
    files=['q050','q075','q100'] if sp!='S' else ['q100']
    sc={t:np.mean([out[t][sp][f]['held'] for f in files]) for t in TOP}
    scc={t:np.mean([out[t][sp][f]['held'] for f in out[t][sp]]) for t in TOP}
    rank=sorted(TOP,key=lambda t:sc[t]); c2&=rank[0]=='ADOPT' and sc['ADOPT']<min(sc[t] for t in TOP if t!='ADOPT')
    print(sp,' '.join(f'{t}:{sc[t]:.2f}' for t in rank),' | incl. controls:',' '.join(f'{t}:{scc[t]:.2f}' for t in sorted(TOP,key=lambda t:scc[t])))
print('C2 overall:','PASS' if c2 else 'FAIL')
print('\nAll topologies, per split/file held-out (in-sample):')
for t in TOP:
    print(t, '; '.join(f"{sp}/{fn} {d['held']:.2f} ({d['ins']:.2f})" for sp in ['T1','T2','S'] for fn,d in out[t][sp].items()))
print('\ncontrol levels predicted vs free; free-level held-out medians:')
for sp in ['T1','T2','S']:
    for fn in ['lp2lp2','lp3lp1']:
        d=out['ADOPT'][sp][fn]; print(sp,fn,'pred %.2f free %.2f  held(pred lvl) %.2f held(free lvl) %.2f'%(d['level_pred'],d['level_free'],d['held'],d['held_freelevel']))
