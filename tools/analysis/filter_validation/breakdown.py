import pickle, numpy as np, json, sys
sys.argv=['x','ADOPT']
exec(open('/home/claude/w/validation/validate.py').read().split("ALL=lambda")[0])
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
NM=['q050','q075','q100','lp2lp2','lp3lp1']; TOP=['ADOPT','M2','M3','M4']
R={t:pickle.load(open(f'res_{t}.pkl','rb')) for t in TOP}
def fcof(lk): k=np.exp(lk); return SR/np.pi*np.arcsin(np.minimum(k,1.99)/2)
rows=[]
def bins(name,vals,edges,labels,e,sp,fn):
    for lo,hi,lab in zip(edges[:-1],edges[1:],labels):
        m=(vals>=lo)&(vals<hi)
        if m.sum()>=50: rows.append(dict(split=sp,file=fn,by=name,bin=lab,n=int(m.sum()),median_abs=float(np.median(abs(e[m]))),median_signed=float(np.median(e[m])),p90_abs=float(np.percentile(abs(e[m]),90))))
for sp in ['T1','T2','S']:
    for fi,(idx,e) in R['ADOPT'][sp]['held'].items():
        fn=NM[fi]; lk=R['ADOPT'][sp]['lk_used'][fi][T[idx]]; ratio=f[idx]/fcof(lk)
        bins('section',tsec[idx],[0,7.3,22,31],['0-7.3 s','7.3-22 s','22-30 s'],e,sp,fn)
        bins('f/fc',ratio,[0,0.5,0.8,1.25,2,1e9],['<0.5','0.5-0.8','0.8-1.25','1.25-2','>2'],e,sp,fn)
        bins('band',f[idx],[0,250,1000,4000,1e9],['<250 Hz','250-1000 Hz','1-4 kHz','>4 kHz'],e,sp,fn)
        bins('level below frame max',rel[idx],[0,20,40,65.01],['0-20 dB','20-40 dB','40-65 dB'],e,sp,fn)
json.dump(rows,open('breakdown.json','w'),indent=0)
import collections
for by in ['section','f/fc','band','level below frame max']:
    print('==',by)
    for sp in ['T1','T2','S']:
        for fn in NM:
            rr=[r for r in rows if r['split']==sp and r['file']==fn and r['by']==by]
            if rr: print(f' {sp} {fn:7s}',' | '.join(f"{r['bin']}: {r['median_abs']:.2f} ({r['median_signed']:+.2f}) n{r['n']}" for r in rr))
# share of total |error| by f/fc bin, q100 held-out S
idx,e=R['ADOPT']['S']['held'][2]; ratio=f[idx]/fcof(R['ADOPT']['S']['lk_used'][2][T[idx]])
for lo,hi in [(0,0.5),(0.5,0.8),(0.8,1.25),(1.25,2),(2,1e9)]:
    m=(ratio>=lo)&(ratio<hi); print('S q100 f/fc %s-%s: %.0f%% of points, %.0f%% of summed |err|, %.0f%% of points with |err|>3 dB'%(lo,hi,100*m.mean(),100*abs(e[m]).sum()/abs(e).sum(),100*(m&(abs(e)>3)).sum()/max((abs(e)>3).sum(),1)))
# ---- figures
fig,ax=plt.subplots(3,5,figsize=(20,10))
for i,sp in enumerate(['T1','T2','S']):
    for j,fn in enumerate(NM):
        a=ax[i,j]; fi=j
        if fi not in R['ADOPT'][sp]['held']: a.axis('off'); continue
        idx,e=R['ADOPT'][sp]['held'][fi]; ref='R0' if fi<3 else 'R0c'; ridx,re_=R['ADOPT'][ref]['res'][fi]; ins=re_[np.searchsorted(ridx,idx)]
        b=np.linspace(-15,15,121); a.hist(ins,b,histtype='step',label='in-sample',density=True); a.hist(e,b,histtype='step',label='held-out',density=True)
        a.set_yscale('log'); a.set_title(f'{sp} {fn}: med|e| {np.median(abs(ins)):.2f} / {np.median(abs(e)):.2f} dB',fontsize=9); a.legend(fontsize=7)
for a in ax[-1]: a.set_xlabel('error (dB, model - recording)')
plt.tight_layout(); plt.savefig('fig_error_histograms.png',dpi=60)
fig,ax=plt.subplots(2,3,figsize=(18,9))
cb=np.exp(np.linspace(np.log(0.1),np.log(20),30)); cc=np.sqrt(cb[1:]*cb[:-1])
for j,(sp,fi) in enumerate([('T1',0),('T1',1),('T1',2),('T2',2),('S',2),('S',3)]):
    a=ax.flat[j]
    for t in TOP:
        idx,e=R[t][sp]['held'][fi]; ratio=f[idx]/fcof(R[t][sp]['lk_used'][fi][T[idx]])
        md=[np.median(abs(e[(ratio>=lo)&(ratio<hi)])) if ((ratio>=lo)&(ratio<hi)).sum()>30 else np.nan for lo,hi in zip(cb[:-1],cb[1:])]
        sg=[np.median(e[(ratio>=lo)&(ratio<hi)]) if ((ratio>=lo)&(ratio<hi)).sum()>30 else np.nan for lo,hi in zip(cb[:-1],cb[1:])]
        a.plot(cc,md,label=f'{t} median |e|',lw=2 if t=='ADOPT' else 1)
        if t=='ADOPT': a.plot(cc,sg,'k--',label='ADOPT signed median')
    a.set_xscale('log'); a.axvline(1,color='grey',lw=0.5); a.axhline(0,color='grey',lw=0.5); a.set_title(f'{sp} held-out {NM[fi]}'); a.set_xlabel('f / fc (fitted cutoff)'); a.set_ylabel('dB'); a.legend(fontsize=7); a.set_ylim(-4,10)
plt.tight_layout(); plt.savefig('fig_error_vs_f_over_fc.png',dpi=60)
fig,ax=plt.subplots(1,1,figsize=(16,5))
tb=np.arange(0,30.5,0.5)
for sp,fi,c in [('T1',2,'C0'),('T2',2,'C1'),('S',2,'C2'),('T1',0,'C3'),('T2',0,'C4')]:
    idx,e=R['ADOPT'][sp]['held'][fi]; ts=tsec[idx]
    md=[np.median(abs(e[(ts>=a)&(ts<b)])) if ((ts>=a)&(ts<b)).sum()>30 else np.nan for a,b in zip(tb[:-1],tb[1:])]
    ax.step(tb[:-1],md,where='post',label=f'{sp} {NM[fi]}',color=c)
ax.axvline(7.3,color='k',lw=0.6); ax.set_xlabel('s'); ax.set_ylabel('median |error| dB (held-out)'); ax.legend(); plt.tight_layout(); plt.savefig('fig_error_vs_time.png',dpi=60)
print('figures ok')
