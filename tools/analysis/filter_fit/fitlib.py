import numpy as np, pickle
SR=44100; N=8192; HOP=1024
fq=np.arange(N//2+1)*SR/N
def peaks_frame(m, rel=60, floor=-100, fmax=6000):
    L=20*np.log10(m+1e-12)
    top=L.max()
    i=np.arange(2,len(L)-2)
    pk=i[(L[i]>L[i-1])&(L[i]>=L[i+1])&(L[i]>L[i-2])&(L[i]>=L[i+2])]
    pk=pk[(L[pk]>top-rel)&(L[pk]>floor)&(fq[pk]<fmax)&(fq[pk]>20)]
    return pk
def amp_at(m,b):
    return np.array([m[max(x-1,0):x+2].max() for x in b])
# --- filter responses (digital) ---
def z(f): return np.exp(1j*2*np.pi*np.asarray(f)/SR)
def onepole(f,k):  # y += k (x - y)
    zz=z(f); return k/(1-(1-k)/zz)
def svf(f,k,q,out='lp'):
    # Chamberlin: lp[n]=lp[n-1]+k*bp[n-1]; hp=x-lp[n]-q*bp[n-1]; bp[n]=bp[n-1]+k*hp
    zz=z(f); zi=1/zz
    # derive transfer: from state eqs
    # LP(z)=zi*LP + k*zi*BP ; HP = X - LP - q*zi*BP ; BP = zi*BP + k*HP
    # => LP(1-zi)=k zi BP ; BP(1-zi+k q zi)=k(X-LP)
    # LP = k zi BP/(1-zi); BP(1-zi+kq zi) = kX - k^2 zi BP/(1-zi)
    den=(1-zi+k*q*zi)+k*k*zi/(1-zi)
    BP=k/den
    LP=k*zi*BP/(1-zi)
    return LP if out=='lp' else BP
