#!/usr/bin/env python3
"""Timing/onset analysis for VGM-player A/B comparisons (numpy only, stdlib wave).

Reads mono/stereo PCM-16 WAV files and reports, per file:
  * onset times (spectral-flux peak-pick) in a chosen window
  * inter-onset-interval median -> implied tempo
  * the onset grid across a "suspect" timestamp, to see if it stays regular

With two files it also cross-correlates their onset envelopes in sliding
windows and prints lag(t) in ms -- a constant lag = the two parts are locked;
a lag that jumps at some point = a real desync there.

Usage:
  analyze.py A.wav [B.wav] [--win LO HI] [--suspect 9.6] [--label A B]
Only WAV/PCM in; convert other formats with Audacity -> Export as WAV first.
"""
import sys, wave, argparse
import numpy as np

def load(path):
    w = wave.open(path, 'rb')
    sr = w.getframerate(); nch = w.getnchannels(); sw = w.getsampwidth()
    raw = w.readframes(w.getnframes()); w.close()
    if sw != 2:
        raise SystemExit(f"{path}: need 16-bit PCM, got {sw*8}-bit")
    x = np.frombuffer(raw, dtype='<i2').astype(np.float64)
    if nch > 1:
        x = x.reshape(-1, nch).mean(axis=1)
    return x / 32768.0, sr

def onset_env(x, sr, hop=256, nfft=1024):
    win = np.hanning(nfft)
    n = 1 + (len(x) - nfft) // hop
    if n <= 1:
        return np.zeros(1), hop / sr
    S = np.empty((n, nfft // 2 + 1))
    for i in range(n):
        seg = x[i*hop:i*hop+nfft] * win
        S[i] = np.abs(np.fft.rfft(seg))
    D = np.diff(S, axis=0)
    flux = np.sqrt(np.maximum(D, 0.0)**2 @ np.ones(S.shape[1]))
    flux = np.concatenate([[0.0], flux])
    if flux.max() > 0:
        flux /= flux.max()
    return flux, hop / sr

def pick_onsets(env, dt, thresh=0.18, min_gap_s=0.05):
    mg = max(1, int(min_gap_s / dt))
    out, last = [], -10**9
    for i in range(1, len(env)-1):
        if env[i] > thresh and env[i] >= env[i-1] and env[i] > env[i+1] and i - last >= mg:
            out.append(i*dt); last = i
    return np.array(out)

def report_one(label, x, sr, lo, hi, suspect):
    env, dt = onset_env(x, sr)
    ons = pick_onsets(env, dt)
    w = ons[(ons >= lo) & (ons <= hi)]
    print(f"\n=== {label} ===  {len(x)/sr:.2f}s @ {sr}Hz, {len(ons)} onsets total")
    if len(w) >= 2:
        ioi = np.diff(w)
        med = np.median(ioi)
        print(f"  window {lo}-{hi}s: {len(w)} onsets, median IOI {med*1000:.1f} ms"
              f"  (~{60/med:.1f} bpm if quarter, ~{60/(4*med):.1f} bpm if 16th)")
        print(f"  IOI ms: " + " ".join(f"{v*1000:.0f}" for v in ioi))
    print(f"  onsets in window: " + " ".join(f"{t:.3f}" for t in w))
    if suspect is not None:
        near = ons[(ons >= suspect-1.0) & (ons <= suspect+1.0)]
        print(f"  around suspect {suspect}s (+/-1s): " + " ".join(f"{t:.3f}" for t in near))
    return env, dt

def xcorr_lag(ea, eb, dt, win_s=2.0, hop_s=0.5, max_lag_s=0.25):
    n = min(len(ea), len(eb))
    ea, eb = ea[:n], eb[:n]
    W = int(win_s/dt); H = int(hop_s/dt); M = int(max_lag_s/dt)
    print(f"\n=== lag(t): B relative to A, + = B later ===")
    t = 0
    while t + W < n:
        a = ea[t:t+W] - ea[t:t+W].mean()
        b = eb[t:t+W] - eb[t:t+W].mean()
        best, bl = -1e9, 0
        for L in range(-M, M+1):
            if L >= 0:
                c = np.dot(a[L:], b[:W-L])
            else:
                c = np.dot(a[:W+L], b[-L:])
            if c > best:
                best, bl = c, L
        na = np.linalg.norm(a); nb = np.linalg.norm(b)
        q = best/(na*nb) if na>0 and nb>0 else 0.0
        print(f"  t={t*dt:5.1f}s  lag={bl*dt*1000:+6.1f} ms  (corr {q:.2f})")
        t += H

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wavs', nargs='+')
    ap.add_argument('--win', nargs=2, type=float, default=[6.0, 14.0])
    ap.add_argument('--suspect', type=float, default=9.6)
    ap.add_argument('--label', nargs='*', default=None)
    a = ap.parse_args()
    lo, hi = a.win
    labels = a.label or [p.rsplit('/',1)[-1] for p in a.wavs]
    envs = []
    for p, lab in zip(a.wavs, labels):
        x, sr = load(p)
        env, dt = report_one(lab, x, sr, lo, hi, a.suspect)
        envs.append((env, dt))
    if len(envs) == 2:
        (ea, dta), (eb, _) = envs
        xcorr_lag(ea, eb, dta)

if __name__ == '__main__':
    main()
