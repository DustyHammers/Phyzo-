import numpy as np, scipy.io.wavfile as wf, scipy.signal as ss
def load(p):
    sr,x=wf.read(p); x=x.astype(np.float64)
    if x.dtype!=np.float64: pass
    if x.ndim>1: x=x.mean(1)
    if np.abs(x).max()>2: x/=32768.0
    return sr,x
