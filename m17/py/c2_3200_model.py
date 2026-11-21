"""Float model of the Codec 2 3200 decoder as described in m17/codec2-3200.md (our own code, for checking our
understanding against the reference decoder's output, and src/services/c2dec.c against this model).

  python m17/py/c2_3200_model.py in.c2 out.raw
"""
import sys

import numpy as np

N = 80
NFFT = 512
R = 2 * np.pi / NFFT
WO_MIN, WO_MAX = 2 * np.pi / 160, 2 * np.pi / 20
BETA, GAMMA = 0.2, 0.5


def unpack(frame):
    word = int.from_bytes(frame, 'big')
    pos = [0]

    def field(width):
        v = (word >> (64 - pos[0] - width)) & ((1 << width) - 1)
        pos[0] += width
        g, s = v, 1
        while s < width:
            g ^= g >> s
            s <<= 1
        return g
    v0, v1, wi, ei = field(1), field(1), field(7), field(5)
    hz, lsp = 0.0, []
    for i in range(10):
        k = field(5)
        hz += 200 + 50 * (k - 7) if (3 <= i <= 5 and k >= 8) else 25 * (k + 1)
        lsp.append(np.pi / 4000 * hz)
    wo = WO_MIN + (WO_MAX - WO_MIN) / 128 * wi
    e = 10 ** ((-10 + 50 / 32 * ei) / 10)
    return v0, v1, wo, e, np.array(lsp)


def lpc(lsp):
    P, Q = np.array([1.0]), np.array([1.0])
    for i in range(0, 10, 2):
        P = np.convolve(P, [1, -2 * np.cos(lsp[i]), 1])
    for i in range(1, 10, 2):
        Q = np.convolve(Q, [1, -2 * np.cos(lsp[i]), 1])
    return ((np.convolve(P, [1, 1]) + np.convolve(Q, [1, -1])) / 2)[:11]


class Decoder:
    def __init__(self):
        self.prev = dict(voiced=0, wo=WO_MIN, e=1.0, lsp=np.arange(10) * np.pi / 11)
        self.ex_phase = 0.0
        self.bg = 0.0
        self.rand_next = 1
        self.sn = np.zeros(2 * N)
        self.pn = np.array([i / N if i < N else 1 - (i - N) / N for i in range(2 * N)])

    def rand(self):
        self.rand_next = (self.rand_next * 1103515245 + 12345) & 0xFFFFFFFFFFFFFFFF  # unsigned long (64 bit)
        return (self.rand_next // 65536) % 32768

    def subframe(self, voiced, wo, e, lsp):
        L = int(np.pi / wo)
        a = lpc(lsp)
        k = np.arange(NFFT // 2)
        Aw = np.fft.rfft(np.concatenate([a, np.zeros(NFFT - 11)]))  # 257 points: the phase may use bin 256
        pw = 1 / (np.abs(Aw[:NFFT // 2]) ** 2 + 1e-6)
        ww = np.abs(np.fft.rfft(np.concatenate([a * GAMMA ** np.arange(11), np.zeros(NFFT - 11)]))[:NFFT // 2]) ** 2
        rw = np.sqrt(ww * pw)
        before = 1e-4 + pw.sum()
        pw = pw * rw ** (2 * BETA)
        pw *= before / (1e-4 + pw.sum()) * e
        pw[:NFFT // 8] *= 1.4 * 1.4
        A = np.zeros(L + 1)
        for m in range(1, L + 1):
            am, bm = int((m - 0.5) * wo / R + 0.5), min(int((m + 0.5) * wo / R + 0.5), NFFT // 2)
            A[m] = np.sqrt(pw[am:bm].sum())
        if wo < np.pi * 150 / 4000:
            A[1] *= 0.032
        # phase
        self.ex_phase += wo * N
        self.ex_phase -= 2 * np.pi * np.floor(self.ex_phase / (2 * np.pi) + 0.5)
        phi = np.zeros(L + 1)
        for m in range(1, L + 1):
            b = int(m * wo / R + 0.5)
            h = np.conj(Aw[b])
            ex = m * self.ex_phase if voiced else 2 * np.pi * self.rand() / 32767
            phi[m] = np.angle(h * np.exp(1j * ex))
        # background noise post processing
        en = 10 * np.log10(1e-12 + np.sum(A[1:] ** 2) / L)
        if en < 40 and not voiced:
            self.bg = self.bg * 0.9 + en * 0.1
        thresh = 10 ** ((self.bg + 6) / 20)
        if voiced:
            for m in range(1, L + 1):
                if A[m] < thresh:
                    phi[m] = 2 * np.pi / 32767 * self.rand()
        # synthesis: bins, IDFT around 0, triangle overlap add
        n = np.arange(-79, 81)
        s = np.zeros(2 * N)
        for m in range(1, L + 1):
            b = min(int(m * wo * NFFT / (2 * np.pi) + 0.5), NFFT // 2 - 1)
            s += 2 * A[m] * np.cos(2 * np.pi * b * n / NFFT + phi[m])
        self.sn[:N - 1] = self.sn[N:2 * N - 1]
        self.sn[N - 1] = 0
        self.sn[:N - 1] += s[:N - 1] * self.pn[:N - 1]
        self.sn[N - 1:] = s[N - 1:] * self.pn[N - 1:]
        out = self.sn[:N].copy()
        mx = out.max()
        if mx > 30000:
            out *= 1 / (mx / 30000) ** 2
        return np.clip(out, -32767, 32767).astype(np.int16)

    def decode(self, frame):
        v0, v1, wo, e, lsp = unpack(frame)
        prev = self.prev
        vm = v0
        if vm and not prev['voiced'] and not v1:
            vm = 0
        if vm:
            if prev['voiced'] and v1:
                wom = (prev['wo'] + wo) / 2
            elif v1:
                wom = wo
            else:
                wom = prev['wo']
        else:
            wom = WO_MIN
        out0 = self.subframe(vm, wom, np.sqrt(prev['e'] * e), (prev['lsp'] + lsp) / 2)
        out1 = self.subframe(v1, wo if v1 else wo, e, lsp)
        self.prev = dict(voiced=v1, wo=wo, e=e, lsp=lsp)
        return np.concatenate([out0, out1])


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    d = Decoder()
    pcm = np.concatenate([d.decode(data[i:i + 8]) for i in range(0, len(data) - 7, 8)])
    pcm.tofile(sys.argv[2])
