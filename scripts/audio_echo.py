"""Look for duplicated audio in a PS2X_SPU2_DUMP raw file (48 kHz s16 stereo): per-second RMS + autocorrelation peaks."""
import sys
import numpy as np
raw = np.fromfile(sys.argv[1], dtype=np.int16).reshape(-1, 2).astype(np.float32)
sr = 48000
print("seconds:", len(raw) / sr)
mono = raw.mean(axis=1)
rms = [float(np.sqrt((mono[i*sr:(i+1)*sr]**2).mean())) for i in range(len(mono)//sr)]
print("rms/sec:", " ".join("%d" % r for r in rms))
lr = np.corrcoef(raw[:,0][raw[:,0]!=0][:200000], raw[:,1][raw[:,0]!=0][:200000])[0,1] if (raw[:,0]!=0).any() else 0
print("L/R corr:", lr)
# autocorrelation echo search over 2-second windows with energy
for start in range(0, len(mono)//sr - 2, 6):
    w = mono[start*sr:(start+2)*sr]
    if np.sqrt((w**2).mean()) < 200: continue
    n = len(w)
    f = np.fft.rfft(w, 2*n)
    ac = np.fft.irfft(f*np.conj(f))[:n]
    ac /= ac[0]
    lo, hi = int(0.01*sr), int(1.0*sr)
    k = lo + int(np.argmax(ac[lo:hi]))
    print("t=%3ds rms=%6.0f best echo lag=%6.1f ms corr=%.3f" % (start, np.sqrt((w**2).mean()), k*1000/sr, ac[k]))
