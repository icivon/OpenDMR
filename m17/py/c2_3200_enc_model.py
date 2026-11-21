"""Float model of the Codec 2 3200 encoder as described in m17/codec2-3200.md section 9 (our own code: for checking
our understanding against the reference encoder's bits, and src/services/c2enc.c against this model).

  python m17/py/c2_3200_enc_model.py in.raw out.c2 [ref.c2]
  python m17/py/c2_3200_enc_model.py --cmp ours.c2 ref.c2

in.raw: 8 kHz 16-bit mono; out.c2: 8 bytes per 20 ms. With ref.c2 the fields are compared one by one.
"""
import sys

import numpy as np

f32 = np.float32
N = 80            # samples per 10 ms
M = 320           # pitch analysis window (40 ms)
NW = 279          # analysis window length
NFFT = 512
R = 2 * np.pi / NFFT
P_MIN, P_MAX = 20, 160
WO_MIN, WO_MAX = 2 * np.pi / P_MAX, 2 * np.pi / P_MIN
DEC = 5

NLP_FIR = np.array([
    -1.0818124e-03, -1.1008344e-03, -9.2768838e-04, -4.2289438e-04, 5.5034190e-04, 2.0029849e-03,
    3.7058509e-03, 5.1449415e-03, 5.5924666e-03, 4.3036754e-03, 8.0284511e-04, -4.8204610e-03,
    -1.1705810e-02, -1.8199275e-02, -2.2065282e-02, -2.0920610e-02, -1.2808831e-02, 3.2204775e-03,
    2.6683811e-02, 5.5520624e-02, 8.6305944e-02, 1.1480192e-01, 1.3674206e-01, 1.4867556e-01])
NLP_FIR = np.concatenate([NLP_FIR, NLP_FIR[::-1]])  # 48 taps, symmetric 600 Hz low pass


def lspd_codebook(i):
    if 3 <= i <= 5:
        return np.array([25.0 * (k + 1) for k in range(8)] + [250.0 + 50 * (k - 8) for k in range(8, 32)])
    return np.array([25.0 * (k + 1) for k in range(32)])


def analysis_window():
    w = np.zeros(M)
    half = NW // 2  # 139
    j = np.arange(2 * half)
    w[M // 2 - half:M // 2 + half] = 0.5 - 0.5 * np.cos(2 * np.pi * j / (NW - 1))
    w /= np.sqrt(np.sum(w * w) * NFFT)
    shifted = np.zeros(NFFT)
    shifted[:half] = w[M // 2:M // 2 + half]
    shifted[NFFT - half:] = w[M // 2 - half:M // 2]
    t = np.fft.fft(shifted).real
    W = np.concatenate([t[NFFT // 2:], t[:NFFT // 2]])  # centred on bin 256
    return w, W


def cheb(coef, x):
    # sum coef[5-i] T_i(x), i = 0..5
    t = [1.0, x]
    for _ in range(2, 6):
        t.append(2 * x * t[-1] - t[-2])
    return sum(coef[5 - i] * t[i] for i in range(6))


def lpc_to_lsp(a, nb=5, delta=0.01):
    P, Q = [1.0], [1.0]
    for i in range(1, 6):
        P.append(a[i] + a[11 - i] - P[i - 1])
        Q.append(a[i] - a[11 - i] + Q[i - 1])
    for i in range(5):
        P[i] *= 2
        Q[i] *= 2
    freq = [0.0] * 10
    roots = 0
    xl, xr, xm = 1.0, 0.0, 0.0
    for j in range(10):
        pt = Q if (j & 1) else P
        psuml = cheb(pt, xl)
        flag = True
        while flag and xr >= -1.0:
            xr = xl - delta
            psumr = cheb(pt, xr)
            if psumr * psuml < 0.0 or psumr == 0.0:
                roots += 1
                for _ in range(nb + 1):
                    xm = (xl + xr) / 2
                    psumm = cheb(pt, xm)
                    if psumm * psuml > 0.0:
                        psuml, xl = psumm, xm
                    else:
                        psumr, xr = psumm, xm
                freq[j] = xm
                xl = xm
                flag = False
            else:
                psuml, xl = psumr, xr
    return roots, np.arccos(np.clip(freq, -1, 1))


def levinson(Rr, order=10):
    a = np.zeros((order + 1, order + 1))
    e = Rr[0]
    for i in range(1, order + 1):
        s = sum(a[i - 1][j] * Rr[i - j] for j in range(1, i))
        k = -(Rr[i] + s) / e
        if abs(k) > 1.0:
            k = 0.0
        a[i][i] = k
        for j in range(1, i):
            a[i][j] = a[i - 1][j] + k * a[i - 1][i - j]
        e *= (1 - k * k)
    out = np.zeros(order + 1)
    out[0] = 1.0
    out[1:] = a[order][1:]
    return out


class Encoder:
    def __init__(self):
        self.w, self.W = analysis_window()
        self.sn = np.ones(M)
        self.sq = np.zeros(M)
        self.mem_x = self.mem_y = 0.0
        self.mem_fir = np.zeros(48)
        self.nlp_w = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(M // DEC) / (M // DEC - 1))
        self.prev_f0 = 1 / 0.02
        self.trace = []

    def nlp(self):
        for i in range(M - N, M):
            x = self.sn[i] * self.sn[i]
            notch = x - self.mem_x + 0.95 * self.mem_y
            self.mem_x, self.mem_y = x, notch
            self.mem_fir = np.roll(self.mem_fir, -1)
            self.mem_fir[-1] = notch + 1.0
            self.sq[i] = np.dot(self.mem_fir, NLP_FIR)
        fw = np.zeros(NFFT)
        fw[:M // DEC] = self.sq[0:M:DEC] * self.nlp_w
        F = np.abs(np.fft.fft(fw)) ** 2
        lo, hi = NFFT * DEC // P_MAX, NFFT * DEC // P_MIN  # 16 .. 128
        gmax_bin = lo + int(np.argmax(F[lo:hi + 1]))
        gmax = F[gmax_bin]
        # sub-multiples: the lowest strong local peak near gmax_bin / mult
        cmax_bin = gmax_bin
        prev_bin = int(self.prev_f0 * NFFT * DEC / 8000)
        mult = 2
        while gmax_bin // mult >= lo:
            b = gmax_bin // mult
            bmin, bmax = int(0.8 * b), int(1.2 * b)
            bmin = max(bmin, lo)
            thresh = 0.3 * gmax * (0.5 if bmin < prev_bin < bmax else 1.0)
            lmax, lbin = 0.0, bmin
            for bb in range(bmin, bmax + 1):
                if F[bb] > lmax:
                    lmax, lbin = F[bb], bb
            if lmax > thresh and lmax > F[lbin - 1] and lmax > F[lbin + 1]:
                cmax_bin = lbin
            mult += 1
        f0 = cmax_bin * 8000 / (NFFT * DEC)
        self.sq[:M - N] = self.sq[N:]
        self.prev_f0 = f0
        return 8000 / f0

    @staticmethod
    def refine(Sw2, wo, pmin, pmax, pstep):
        L = int(np.pi / wo)
        best_wo, best_e = wo, 0.0
        p = f32(pmin)
        while p <= f32(pmax):
            w = f32(2 * np.pi) / p
            bf = f32(w * f32(1 / R))
            cur = bf
            e = 0.0
            for _ in range(L):
                e += Sw2[int(cur + f32(0.5))]
                cur = f32(cur + bf)
            if e > best_e:
                best_e, best_wo = e, float(w)
            p = f32(p + f32(pstep))
        return best_wo

    def analyse(self, speech):
        self.sn[:M - N] = self.sn[N:]
        self.sn[M - N:] = speech
        half = NW // 2
        x = np.zeros(NFFT)
        x[:half] = self.sn[M // 2:M // 2 + half] * self.w[M // 2:M // 2 + half]
        x[NFFT - half:] = self.sn[M // 2 - half:M // 2] * self.w[M // 2 - half:M // 2]
        Sw = np.fft.fft(x)
        Sw2 = np.abs(Sw) ** 2
        pitch = self.nlp()
        wo = 2 * np.pi / pitch
        wo = self.refine(Sw2, wo, 2 * np.pi / wo - 5, 2 * np.pi / wo + 5, 1.0)
        wo = self.refine(Sw2, wo, 2 * np.pi / wo - 1, 2 * np.pi / wo + 1, 0.25)
        wo = min(max(wo, WO_MIN), WO_MAX)
        L = int(np.floor(np.pi / wo))
        if wo * L >= 0.95 * np.pi:
            L -= 1
        A = np.zeros(L + 1)
        for m in range(1, L + 1):
            am, bm = int((m - 0.5) * wo / R + 0.5), int((m + 0.5) * wo / R + 0.5)
            A[m] = np.sqrt(np.sum(Sw2[am:bm]))
        voiced = self.voicing(Sw, wo, L, A)
        return wo, voiced

    def voicing(self, Sw, wo, L, A):
        W = self.W
        l1k = int(L * 1000.0 / 4000)
        sig = 1e-4 + np.sum(A[1:l1k + 1] ** 2)
        err = 1e-4
        for l in range(1, l1k + 1):
            al = int(np.ceil((l - 0.5) * wo * NFFT / (2 * np.pi)))
            bl = int(np.ceil((l + 0.5) * wo * NFFT / (2 * np.pi)))
            off = int(NFFT / 2 - l * wo * NFFT / (2 * np.pi) + 0.5)
            ww = W[off + al:off + bl]
            s = Sw[al:bl]
            amp = np.sum(s * ww) / np.sum(ww * ww)
            err += np.sum(np.abs(s - amp * ww) ** 2)
        snr = 10 * np.log10(sig / err)
        voiced = snr > 6.0
        l2k, l4k = int(L * 2000.0 / 4000), int(L * 4000.0 / 4000)
        elow = 1e-4 + np.sum(A[1:l2k + 1] ** 2)
        ehigh = 1e-4 + np.sum(A[l2k:l4k + 1] ** 2)
        eratio = 10 * np.log10(elow / ehigh)
        if not voiced and eratio > 10.0:
            voiced = True
        if voiced:
            if eratio < -10.0:
                voiced = False
            if eratio < -4.0 and wo <= 60.0 * 2 * np.pi / 8000:
                voiced = False
        self.trace.append((snr, eratio))
        return int(voiced)

    def lsps(self):
        wn = self.sn * self.w
        if not np.any(wn):
            return 0.0, np.arange(10) * np.pi / 10
        Rr = np.array([np.dot(wn[:M - k], wn[k:]) for k in range(11)])
        a = levinson(Rr)
        E = float(np.dot(a, Rr))
        a = a * 0.994 ** np.arange(11)
        roots, lsp = lpc_to_lsp(a)
        if roots != 10:
            lsp = np.arange(10) * np.pi / 10
        return E, lsp

    def encode(self, speech):
        """160 samples -> (fields, 8 bytes)"""
        _, v0 = self.analyse(speech[:N])
        wo, v1 = self.analyse(speech[N:])
        wi = int(np.floor(128 * (wo - WO_MIN) / (WO_MAX - WO_MIN) + 0.5))
        wi = min(max(wi, 0), 127)
        E, lsp = self.lsps()
        ei = int(np.floor(32 * (10 * np.log10(E) + 10) / 50 + 0.5)) if E > 0 else 0
        ei = min(max(ei, 0), 31)
        hz = 4000 / np.pi * lsp
        idx, q = [], 0.0
        for i in range(10):
            d = hz[i] - (q if i else 0.0)
            cb = lspd_codebook(i)
            k = int(np.argmin(np.abs(cb - d)))
            idx.append(k)
            q = (q if i else 0.0) + cb[k]
        fields = [v0, v1, wi, ei] + idx
        return fields, pack(fields)


WIDTHS = [1, 1, 7, 5] + [5] * 10
NAMES = ['v0', 'v1', 'Wo', 'E'] + ['lsp%d' % i for i in range(10)]


def pack(fields):
    word = 0
    for v, w in zip(fields, WIDTHS):
        word = (word << w) | (v ^ (v >> 1))
    return word.to_bytes(8, 'big')


def unpack(frame):
    word = int.from_bytes(frame, 'big')
    pos, out = 64, []
    for w in WIDTHS:
        pos -= w
        g = (word >> pos) & ((1 << w) - 1)
        v, s = g, 1
        while s < w:
            v ^= v >> s
            s <<= 1
        out.append(v)
    return out


def compare(a, ref):
    n = min(len(a), len(ref)) // 8
    fa = [unpack(a[8 * i:8 * i + 8]) for i in range(n)]
    fr = [unpack(ref[8 * i:8 * i + 8]) for i in range(n)]
    same = sum(1 for i in range(n) if fa[i] == fr[i])
    print('%d frames, %d identical (%.1f%%)' % (n, same, 100.0 * same / n))
    for j, name in enumerate(NAMES):
        diffs = [fa[i][j] - fr[i][j] for i in range(n)]
        bad = sum(1 for d in diffs if d)
        big = sum(1 for d in diffs if abs(d) > 1)
        print('  %-6s differ %5d (%.1f%%), by >1: %d' % (name, bad, 100.0 * bad / n, big))


def main():
    if sys.argv[1] == '--cmp':  # --cmp ours.c2 ref.c2
        compare(open(sys.argv[2], 'rb').read(), open(sys.argv[3], 'rb').read())
        return
    pcm = np.fromfile(sys.argv[1], dtype='<i2').astype(float)
    enc = Encoder()
    out = bytearray()
    frames = []
    for i in range(len(pcm) // 160):
        f, b = enc.encode(pcm[160 * i:160 * i + 160])
        frames.append(f)
        out += b
    open(sys.argv[2], 'wb').write(out)
    if len(sys.argv) > 3:
        compare(bytes(out), open(sys.argv[3], 'rb').read())

if __name__ == '__main__':
    main()
