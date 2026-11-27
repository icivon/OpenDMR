# Yaesu System Fusion (YSF / C4FM) air interface: facts

These are facts only, with no code. Each fact names its source. Short names for the sources:

- **[SPEC]**: Yaesu Musen, *Amateur Radio Digital Standards*, v1.0 (2013-01-15), rev 1.02 (2015-07-29), 38 pages. This is the primary source. Copy used: https://www.rcqsl.com/manuals/78.pdf (the same document is also at https://qsl.net/kb9mwr/projects/dv/yaesu/Yaesu_Amateur%20Radio%20Digital%20Specs_1V02_EN-GB.pdf). Table and section numbers below refer to this document.
- **[HOST]**: g4klx/MMDVMHost master: YSFDefines.h, YSFFICH.cpp, YSFPayload.cpp, YSFConvolution.cpp, YSFControl.cpp, CRC.cpp, Golay24128.cpp, AMBEFEC.cpp, Hamming.cpp, Sync.cpp, MMDVM-Host.ini. (The old file name MMDVM.ini no longer exists.)
- **[FW]**: g4klx/MMDVM firmware master: YSFDefines.h, YSFTX.cpp, YSFRX.cpp; DMRDMOTX.cpp and P25TX.cpp for comparison.
- **[HS]**: juribeparada/MMDVM_HS: ADF7021.h, ADF7021.cpp, YSFTX.cpp.
- **[CM]**: juribeparada/MMDVM_CM YSF2DMR and DMR2YSF: ModeConv.cpp, YSF2DMR.cpp, YSFFICH.cpp, Conf.cpp, ini files.
- **[DSDCC]**: f4exb/dsdcc ysf.cpp and ysf.h.
- **[CAP]**: my own decode of real recordings, done with a scratch demodulator and decoder written from [SPEC]:
  - FTM-400D, DN mode, direct: SystemFusion_DN_PY2DZA_DiscrOut.wav
  - FTM-400D, VW mode, direct: SystemFusion_VW_PY2DZA_DiscrOut.wav
  - F5ZOO repeater downlink: dsdcc samples/ysf_f5zoo.dis

  The decoded FICH, CSD and DCH values are quoted below as observed.

**Source agreement.** All bit layouts below were cross-checked. [SPEC], [HOST], [DSDCC] and [CM] agree unless a disagreement is noted. In [CAP], an HC frame encoded from these rules matched a real FTM-400D header to within 4 of 960 bits (channel errors). A VD2 DCH matched to within 7 of 200 bits.

**Bit order conventions in this document.**
- "MSB first" means bit 0 of a field is the MSB of its first byte.
- The first bit of a frame is transmitted first.
- A dibit is two consecutive bits. The first bit is the dibit's MSB.

---

## 1. Modulation

| Item | Value | Source |
|---|---|---|
| Modulation | C4FM: 4-level FSK with Nyquist shaping. Emission F1D (data) or F7W (data and voice). | [SPEC] 3.1 |
| Symbol rate | 4800 sym/s. Bit rate 9600 bit/s. Rate tolerance ±5 ppm. | [SPEC] 3.1(7), 3.3(7) |
| Frame | 960 bits = 480 symbols = 120 bytes = 100 ms | [SPEC] 3.1(8), 4.1; [HOST]/[FW] YSF_FRAME_LENGTH_BYTES=120 |
| Max deviation | Less than ±4950 Hz | [SPEC] 3.3(8) |
| Modulation accuracy | 10 % or less | [SPEC] 3.3(9) |
| Reference sensitivity | BER 1e-2 with PN511 test signal at 0 dBµV (rough reading of a garbled PDF table) | [SPEC] 3.4 |

**Dibit to symbol to deviation** ([SPEC] Table 3-1):

| Dibit | Symbol | Wide (Dev=0) | Narrow (Dev=1) |
|---|---|---|---|
| 01 | +3 | +2700 Hz | +1350 Hz |
| 00 | +1 | +900 Hz | +450 Hz |
| 10 | −1 | −900 Hz | −450 Hz |
| 11 | −3 | −2700 Hz | −1350 Hz |

**Same mapping as DMR and P25.** Both also use 01=+3, 00=+1, 10=−1, 11=−3. Only the scale differs:

| Mode | +1 symbol | Outer (±3) |
|---|---|---|
| YSF wide | 900 Hz | 2700 Hz |
| YSF narrow | 450 Hz | 1350 Hz |
| DMR | 648 Hz | 1944 Hz |
| P25 | 600 Hz | 1800 Hz |

So YSF wide = 1.389 × DMR deviation, and YSF narrow = 0.694 × DMR.

Other sources agree:
- [HS] ADF7021.cpp comment: "Dev: +1 symb 900/450 Hz". The ADF7021 deviation words scale accordingly: YSF_H 32, YSF_L 16, DMR 23, P25 22.
- [FW] YSFTX.cpp levels, high/low: ±1893/±631 and ±948/±316. DMRDMOTX levels are ±1362/±454. Both use the same RRC 0.2 filter. 1893/1362 × 1944 Hz = 2702 Hz.
- [FW] YSFDefines.h comment maps the sync dibits as 11→−3, 01→+3, 00→+1, 10→−1.

The MMDVM firmware TX writes dibit 11 to the positive level. That is a global inversion, the same for DMR and P25 in that firmware (fixed in hardware or by the TX invert option). It does not mean YSF differs from DMR.

[CAP]: with the discriminator output taken non-inverted, the sync correlated in all three recordings. With inverted polarity it correlated in none.

**Pulse shaping.**
- [SPEC] 3.2(3) and (4): the TX filter H(f) and the RX filter H(f) are both root-raised-cosine, T = 1/4800 s, roll-off α = 0.2. (The PDF formula text is garbled. It is the cos(πT/4α·(2|f| − (1−α)/T)) form, i.e. square-root RC. TX and RX together make full RC.)
- [FW] YSFTX uses RRC α=0.2, span 8 symbols, 5 samples/symbol, 45 taps.
- Unlike P25 there is no extra inverse-sinc shaping filter. (MMDVM P25TX uses a "normal" RC filter. YSF and DMR use RRC.)

**Frame sync** ([SPEC] 4.2.1(3); [HOST] YSF_SYNC_BYTES; [FW]):
- 40 bits = 0xD4 71 C9 63 4D.
- Symbols: −3 +3 +3 +1 +3 −3 +1 +3 −3 +1 −1 +3 +3 −1 +3 −3 +3 +1 −3 +3.
- [FW] RX accepts up to 3 symbol errors in the 20-symbol sync word (MAX_SYNC_SYMBOLS_ERRS=3).
- [SPEC] 4.7: sync is established after N1 = 1 sync received (recommended). Sync is lost after N2 = 4 consecutive misses (recommended).

---

## 2. Frame structure and transmission sequence

**Every frame** ([SPEC] 4.1):

| Field | Frame bits | Frame bytes |
|---|---|---|
| FS (sync) | 0–39 | 0–4 |
| FICH | 40–239 (200 bits = 100 dibits) | 5–29 |
| Payload | 240–959 (720 bits) | 30–119 |

In the payload layouts below, payload bit p = frame bit 240+p.

**Frame types**, from FICH FI:
- HC (header), FI=0
- CC (communication), FI=1
- TC (terminator), FI=2
- Test, FI=3

**Sequence:** HC, then CC × n, then TC. [SPEC] 4.6(1): "before transmitting in the CC, the radio must continuously transmit the HC a prescribed number of times (N0: recommended value 1)".

**What a real radio sends** ([CAP], FTM-400D direct):

1. **Carrier and preamble.** Carrier on with DC (zero modulation). Then about 200–205 symbols (about 42 ms) of alternating +3/−3, i.e. dibits 01 11 repeating = byte 0x77. The last preamble symbol before the sync is −3. MMDVM firmware also sends 0x77 bytes as its YSF TX-delay preamble ([FW]/[HS] YSF_START_SYNC=0x77). The preamble length is not specified in [SPEC]; the value here is from one capture.
2. **Exactly one HC.**
3. **CC frames, contiguous**, every 100 ms.
4. **Exactly one TC.** In [CAP], DN mode cut audio at FN=4 (FT=6), so the TC does not wait for the FN cycle to finish.
5. **Tail.** About 25–30 ms of unmodulated carrier, then carrier off.

In both DN and VW, the HC and TC carry the same CSD1/CSD2 as each other.

**HC / TC layout** (all modes; [SPEC] Fig 4-7). The payload is 10 blocks of 72 bits, alternating:

DCH-1(0) DCH-2(0) DCH-1(1) DCH-2(1) … DCH-1(4) DCH-2(4)

- DCH-1(j) is at payload bits 144j … 144j+71.
- DCH-2(j) is at payload bits 144j+72 … 144j+143.
- DCH-1 = the concatenation DCH-1(0)…DCH-1(4) = 360 coded bits carrying CSD1 (dest 10 bytes + src 10 bytes).
- DCH-2 likewise carries CSD2 (downlink 10 bytes + uplink 10 bytes). It is filled with 0x20 (space) when empty, for example in direct mode.
- Byte view: DCH-1 = payload bytes 0–8, 18–26, 36–44, 54–62, 72–80. DCH-2 = bytes 9–17, 27–35, …

---

## 3. FICH (bit-exact)

### 3.1 The 32 information bits ([SPEC] Fig 4-9, Table 4-1)

Bit 31 is the MSB of byte 0.

| Bits | Byte.bit | Field | Values |
|---|---|---|---|
| 31:30 | b0.7–6 | FI, frame info | 0 HC, 1 CC, 2 TC, 3 Test |
| 29:28 | b0.5–4 | CS, callsign info | 2 (binary 10) = "callsigns 1, 2, 3 assigned". 0, 1, 3 reserved. [SPEC] says CS=10 is fixed in all modes. |
| 27:26 | b0.3–2 | CM, call mode | 0 Group/CQ, 1 Radio ID, 2 reserved, 3 Individual |
| 25:24 | b0.1–0 | BN, block number | Used by Data FR |
| 23:22 | b1.7–6 | BT, block total | Used by Data FR |
| 21:19 | b1.5–3 | FN, frame number | 0..7 |
| 18:16 | b1.2–0 | FT, frame total | 0..7 |
| 15 | b2.7 | reserved | 0 |
| 14 | b2.6 | Dev | 0 wide, 1 narrow |
| 13:11 | b2.5–3 | MR, message routing | 0 direct, 1 downlink with uplink not busy, 2 downlink with uplink busy, 3–7 reserved |
| 10 | b2.2 | VoIP | 0 local, 1 via Internet |
| 9:8 | b2.1–0 | DT, data type | 0 V/D mode 1, 1 Data FR, 2 V/D mode 2 (DN), 3 Voice FR (VW) |
| 7 | b3.7 | SQL type | 0 = SQL code enabled, 1 = disabled |
| 6:0 | b3.6–0 | SC, squelch code | 0 none, 1..126 code, 127 break-out code |

Notes:
- [DSDCC] ysf.h writes FT as "18:17". That is a typo; FT is 3 bits, 18:16.
- **DG-ID (uncertain).** Yaesu's later DG-ID feature is not in the 2015 [SPEC]. [HOST] reads and writes DG-ID as byte 3 bits 6–0, i.e. the SC field, and leaves bit 7 alone (getDGId/setDGId).
- **MMDVMHost MR quirk.** setMR writes 3 bits (mask 0x38). getMR reads only 2 bits ((b2>>3)&3). Treat MR as 3 bits, as in [SPEC] and [DSDCC].
- **Observed values** ([CAP]):
  - Direct DN: CS=2, CM=0, BN=BT=0, Dev=0, MR=0, VoIP=0, DT=2, SQL=0, SC=0.
  - F5ZOO repeater downlink: MR=2, and everything else the same.
  - MMDVMHost repeating a frame sets MR=2 (busy), or MR=1 with RemoteGateway, and Dev = LowDeviation ([HOST] YSFControl).

### 3.2 Encoding chain ([SPEC] 4.5(1), Fig 4-24 and 4-26; [HOST] YSFFICH::encode; [DSDCC])

1. **CRC.** 32 info bits (4 bytes) + CRC-16 = 48 bits. The CRC is appended big-endian (high byte first) as bytes 4–5.
   - CRC parameters, used for FICH and all DCH: polynomial x^16+x^12+x^5+1 (0x1021), init 0x0000, MSB-first (not reflected), final XOR 0xFFFF. ([SPEC] "initial values of all registers zero, all bits inverted at the end"; [HOST] CRC.cpp addCCITT162; [DSDCC] CRC(PolyCCITT16, init 0, xor 0xFFFF).)
   - This is the catalogue CRC-16/GSM. Check value for ASCII "123456789" = **0xCE3C** (computed).
2. **Golay.** Split the 48 bits into 4 × 12 bits, MSB first. Bits 0–11 are byte0 plus the high nibble of byte1, and so on. Encode each with extended Golay(24,12,8). Each codeword is d11…d0 p11…p0, data first and MSB first ([SPEC] App. A). Concatenate the 4 codewords to get 96 bits.
3. **Tail.** Append 4 zero bits: 100 bits.
4. **Convolutional code.** Rate 1/2, K=5 (section 3.4). This gives 200 bits.
5. **Interleave.** Treat the 200 bits as 100 dibits S00..S99 (dibit i = coded bits 2i, 2i+1). Write them row by row into a 20 × 5 matrix (5 dibits per row). Read column by column: on air, S00 S05 S10 … S95 S01 S06 …
   - Equivalently, dibit S_i is placed at FICH dibit position 20·(i mod 5) + ⌊i/5⌋.
   - In bit terms: coded bits 2i and 2i+1 go to FICH bits 40·(i mod 5) + 2·⌊i/5⌋ and +1 (FICH bit 0 = frame bit 40).
   - [SPEC] text says "M = 5 dibits, depth N = 10", but its figure shows 20 rows. 100/5 = 20, so N=10 is a typo.

Decoding: [HOST] runs a hard-decision Viterbi, Golay-decodes the 4 words (rejecting the frame if any is uncorrectable), then checks the CRC.

### 3.3 Golay(24,12,8) ([SPEC] App. A; [HOST] Golay24128.cpp)

- Generator polynomial of the underlying (23,12) code: g(x) = x^11+x^10+x^6+x^5+x^4+x^2+1 = **0xC75**.
- Codeword = data(12) << 12 | p(12), where p = [(data·x^11 mod g) << 1] | overall even-parity bit.
- [SPEC] generator-matrix parity rows:

| Data bit | Parity row |
|---|---|
| d11 | 110001110101 |
| d10 | 011000111011 |
| d9 | 111101101000 |
| … | … |
| d0 | 100011101011 |

- Test values (computed; they match [HOST] ENCODING_TABLE_24128):

| Data | Codeword |
|---|---|
| 0x001 | 0x0018EB |
| 0x800 | 0x800C75 |
| 0xFFF | 0xFFFFFF |

- [HOST] accepts a word if the syndrome weight is under 3, or if the corrected word has even parity.

### 3.4 Convolutional code ([SPEC] 4.5(1)(d), App. B; [HOST] YSFConvolution.cpp)

- Rate 1/2, K = 5, encoder registers start at 0.
- G1 = 1 + x^3 + x^4. G2 = 1 + x + x^2 + x^4.
- With d = current input bit and d1..d4 = previous inputs (d1 most recent): g1 = d ⊕ d3 ⊕ d4, g2 = d ⊕ d1 ⊕ d2 ⊕ d4.
- In octal, written current-input first (1,D,D²,D³,D⁴): G1 = 10011 = 23, G2 = 11101 = 35.
- Output order: g1, then g2, for every input bit.
- 4 zero tail bits make the trellis end in state 0. [HOST] starts the chainback from state 0.

### 3.5 FICH test vectors

All are computed with the rules above. The first two were confirmed against a real FTM-400D capture ([CAP]); the capture differed by 2 bits (BE/BF and 76/77), which are channel errors.

| Frame | FICH 4+2 bytes (info+CRC) | 25 bytes on air (frame bytes 5–29) |
|---|---|---|
| HC, CS2 CM0 FN0 FT6 MR0 DT2 (direct DN) | 20 06 02 00 1C 73 | 213D38A4BE3121FC3123CF8280E0944BB300767C4FDC230728 |
| CC FN0 FT6 DT2 | 60 06 02 00 72 EF | 11CD384DC1F2B1FC46DD8E7283E40E077302E857C51C23F260 |
| CC FN1 FT6 DT2 | 60 0E 02 00 DB 4E | 11DD40DC62F2A2F333558E501274F00743F08CA1C5DF80A637 |
| TC FT6 DT2 | A0 06 02 00 C1 4B | E29D38784170C1FF01B883B28296BDC1F300F7378C1C23AAD8 |
| HC FT7 MR2 DT2 (repeater) | 20 07 12 00 28 30 | 21365C9A1A3121A0D7DDCFB5909C5C4BA60373D14FC5119AF7 |

Further CRC-valid FICH info+CRC bytes seen in [CAP]:

| Frame | FICH bytes |
|---|---|
| CC FN2..FN6 FT6 DT2 | 60160200318C, 601E0200982D, 60260200F429, 602E02005D88, 60360200B74A |
| VW HC FT0 DT3 | 200003009DE2 |
| VW CC FN0 FT1 | 60010300C44E |
| VW CC FN1 FT1 | 600903006DEF |
| VW CC FN0 FT0 | 60000300F37E |
| VW TC | A000030040DA |
| Repeater HC FT6 | 200612001F00 |
| Repeater TC FT7 | A0071200F508 |
| Repeater TC FT6 | A0061200C238 |

---

## 4. Payload channels

### 4.0 Whitening (PN9), shared by every DCH and the VD2 voice

- [SPEC] 4.5(2)(b): PN(9,5) LFSR, polynomial x^9 + x^5 + 1. Registers S08..S00 start as 1 1 1 0 0 1 0 0 1, i.e. **0x1C9** ([DSDCC] PN_9_5(0x1C9)).
- The register is re-initialised for each unit (each DCH block and each VCH).
- Bit-sequence form (verified by Berlekamp-Massey against the [HOST] table):
  - w0..w8 = 1 0 0 1 0 0 1 1 1
  - w[n] = w[n−5] ⊕ w[n−9]
- XOR w0, w1, … onto the unit's data bits from the first (top) bit.
- First 20 bytes, used by [HOST] WHITENING_DATA and [CM]:

  93 D7 51 21 9C 2F 6C D0 EF 0F F8 3D F1 73 20 94 ED 1E 7C D8

### 4.1 DCH coding ([SPEC] 4.5(2), Fig 4-27, 4-30, 4-31; [HOST] YSFPayload; [DSDCC])

The order is whitening, then CRC, then the convolutional code. **The CRC is computed over the whitened bytes, and the CRC itself is not whitened.**

**Long DCH** (HC/TC DCH-1 and DCH-2, V/D mode 1 CC, Data FR CC, VW sub-header):
1. 20 data bytes (160 bits) XOR whitening bytes 0–19.
2. Append CRC-16 big-endian: 176 bits.
3. Append 4 zero tail bits: 180 bits.
4. Convolutional code: 360 bits = 180 dibits.
5. Interleave as a 20-row × 9-dibit matrix: dibit S_i goes to position 20·(i mod 9) + ⌊i/9⌋, i.e. on air S00 S09 S18 …
   - In bits: coded bits 2i, 2i+1 go to DCH bits 40·(i mod 9) + 2·⌊i/9⌋ and +1.
6. Cut the 360 DCH bits into 5 × 72-bit pieces DCH(0)..DCH(4), in order, and place them in the frame.

**Short DCH** (V/D mode 2 CC):
1. 10 data bytes (80 bits) XOR whitening bytes 0–9.
2. Append CRC-16: 96 bits.
3. Append 4 tail bits: 100 bits.
4. Convolutional code: 200 bits = 100 dibits.
5. Interleave as 20 rows × 5 dibits. This is the same rule as the FICH: S_i goes to 20·(i mod 5) + ⌊i/5⌋.
6. Cut into 5 × 40-bit pieces.

**DCH test vectors** (computed; the first was confirmed against [CAP] within 7/200 channel errors):

| Content | 12 bytes, whitened + CRC | 200 interleaved bits (as 25 bytes; split into 5 × 40 bits) |
|---|---|---|
| VD2 DCH "**********" | B9FD7B0BB60546FAC5255162 | E77BFC86E1485358796DA9F0EBFDCE18FB301A5734DF1946E0 |
| VD2 DCH "PY2DZA    " | C38E6365C66E4CF0CF2F6F15 | F2C8FB6E75B4798606C322306574D6F9B207E0DA6C2821D90F |

**Complete HC frame**, 120 bytes. FICH is HC FT6 DT2, CSD1 = "**********PY2DZA    ", CSD2 = 20 spaces. It matches the real FTM-400D header to within 4 bits ([CAP]):

```
D471C9634D213D38A4BE3121FC3123CF8280E0944BB300767C4FDC230728F3F8FB1C1853118134DF
2363C017607F1C88BEA5E04FC29426FF9BB9A85ABA46921014BE11A92CD37A891B9C7575A8F77FB8
1110F2C664F3083EE8A7C9948F3DEF0E84251E49946425B6063C570E3151BC1373F60DFBD5890193
```

### 4.2 Callsign data ([SPEC] 4.2.1(5), Table 4-2)

All fields are ASCII, padded with spaces (0x20).

| Block | Content |
|---|---|
| CSD1 (20 bytes) | Dest (10) + Src (10). With CM=1 (Radio ID mode), Dest is Dest Radio ID (5) + Src Radio ID (5). |
| CSD2 (20 bytes) | Downlink callsign (10) + Uplink callsign (10) |
| CSD3 (20 bytes) | Rem1 (5) + Rem2 (5) + Rem3 (5) + Rem4 (5) |

CSD3 fields:
- Rem1 = downlink ID of the repeater or VoIP station.
- Rem2 = uplink ID of the repeater or VoIP station.
- Rem3 = VoIP station / relay system ID.
- Rem4 = source radio ID.

"Radio ID" is a 5-byte device-specific identifier.

**Observed values** ([CAP]):
- FTM-400D direct:
  - Dest = "**********" (10 asterisks, group/CQ), Src = "PY2DZA    ".
  - CSD2 = all spaces.
  - Rem1+Rem2 = spaces. Rem3 = spaces. Rem4 = "F0Ckw" (radio ID).
- F5ZOO repeater downlink:
  - Down = Up = "F5ZOO-R1  ".
  - Rem1+Rem2 = "R0MWZR0MWZ".
- [CM] YSF2DMR instead puts "*****" + RadioID into Dest. That is a choice made by that converter; real radios send 10 asterisks.

### 4.3 V/D mode 2 ("DN"; DT=2): the common Fusion mode

**CC layout** ([SPEC] Fig 4-12; [HOST] processVDMode2*; [DSDCC]). For j = 0..4:

| Piece | Payload bits |
|---|---|
| DCH(j) | 144j … 144j+39 (40 bits) |
| VCH(j) + VeCH(j) | 144j+40 … 144j+143 (104 bits: 72 VCH followed by 32 VeCH, contiguous) |

The byte view is in section 6.

Each 104-bit voice block carries one 20 ms AMBE+2 half-rate frame (49 bits). That is 5 frames per 100 ms.

**DCH content per FN** ([SPEC] Table 4-4; [HOST]; [DSDCC]; [CAP]):

| FN | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| Content | Dest | Src | Downlink | Uplink | Rem1+Rem2 | Rem3+Rem4 | DT1 | DT2 |

- FT=5: no text data.
- FT=6: DT1 (1–10 bytes). This is the normal radio case: the FTM-400D sends FT=6, FN 0..6 repeating.
- FT=7: DT1+DT2 (11–20 bytes). Seen on the F5ZOO repeater from a GPS-equipped station, where DT1/DT2 change every cycle.

DT1 content in [CAP] (FTM-400D) was 48 22 61 5F 25 03 52 00 00 00. Through the repeater it was 55 22 61 5F 25 03 5F 00 00 00. The [CM] example config uses DT1 = 01 22 61 5F 2B 03 11 00 00 00 and DT2 = 00 00 00 00 6C 20 1C 20 03 08. The meaning of these bytes (radio-model and GPS data) is not defined in [SPEC].

**Voice FEC** ([SPEC] 4.5(3), Fig 4-32; [HOST] processVDMode2Audio; [CM] ModeConv; [DSDCC]):
1. Split the 49 voice bits into the first 27 bits and the last 22 bits.
2. Triple each of the first 27 bits in place: bit k goes to positions 3k, 3k+1, 3k+2 (positions 0–80).
3. Copy the 22 remaining bits to positions 81–102.
4. Position 103 = 0. This gives 104 bits.
5. XOR with whitening bits w0..w103. This is the same PN9 as the DCH, freshly seeded, i.e. whitening bytes 0–12.
6. Interleave with M = 26, N = 4: bit V_i (i = 0..103) is transmitted at block position 4·(i mod 26) + ⌊i/26⌋. On air the order is V0 V26 V52 V78 V1 V27 …
7. Air positions 0–71 are the "VCH" and 72–103 the "VeCH".
8. Receive: majority vote on each triplet.

**49-bit order vs DMR AMBE+2** ([CM] ModeConv putAMBE2YSF / putYSF; verified with silence vectors):

The 49 bits are a(12) | b(12) | c(25), each MSB first:
- **a** = the 12 information bits of DMR's Golay(24,12) word (DMR "A"/C0).
- **b** = the 12 information bits of DMR's Golay(23,12) word (C1), after removing the DMR scrambler. That is, b_info = Golay23-decode(B23 ⊕ (PRNG[a] >> 1)), where PRNG is the 4096-entry DMR/D-STAR AMBE scrambler table indexed by a.
- **c** = DMR's 25 unprotected C2 bits.

So the 27 protected (tripled) bits are a[11..0], b[11..0], c[24..22]. The 22 plain bits are c[21..0].

YSF V/D2 carries no Golay and no PRNG on the voice. To convert to DMR, Golay-encode a, Golay-encode b, XOR b with PRNG(a)>>1, and place a, b and c with the DMR A/B/C tables (section 4.4).

[DSDCC] also gives a 49-entry reorder from this vector to the DVSI AMBE3000 packed-frame bit order (gr-ysf issue #12):

0,3,6,9,12,15,18,21,24,27,30,33,36,39,41,43,45,47, 1,4,7,10,13,16,19,22,25,28,31,34,37,40,42,44,46,48, 2,5,8,11,14,17,20,23,26,29,32,35,38

That is, YSF bit k goes to DVSI bit table[k].

**Silence test vector** ([CM] YSF_SILENCE; decoded and verified here):

| Form | Value |
|---|---|
| On-air 104-bit block (13 bytes, already whitened and interleaved) | 7B B2 8E 43 36 E4 A2 39 78 49 33 68 33 |
| Decoded | a=0xF80, b=0x1A9, c=0x13F19C1. All 27 triplets are consistent and the pad bit is 0. |
| 49 bits | 111110000000 000110101001 1001111110001100111000001 |
| Equivalent DMR 72-bit AMBE silence ([CM] DMR_SILENCE) | B9 E8 81 52 61 73 00 2A 6B (A24=F80148, scrambled B23=54150D, C=13F19C1) |

**BER counting in MMDVMHost** ([HOST] YSFControl / YSFPayload):
- Errors = number of triplets that were not unanimous (27 per block, 135 per frame).
- Each frame adds 405 to the bit count, i.e. 5 × 81.
- Log: "V/D Mode 2 … Repetition FEC x/405".
- The final "BER: %" is the sum of errors × 100 / the sum of bits. The bit counter starts at 1.
- The 22 unprotected bits and the pad bit are not counted. [HOST] notes that the pad bit "apparently is not" always zero. In [CAP] it was always 0.

### 4.4 V/D mode 1 (DT=0)

**CC layout** ([SPEC] Fig 4-8; [HOST]; [DSDCC]). For j = 0..4:

| Piece | Payload bits |
|---|---|
| DCH(j) | 144j … 144j+71 |
| VCH(j) | 144j+72 … 144j+143 |

- The DCH is a long DCH (20 bytes per frame).
- Each VCH is exactly one DMR-style 72-bit AMBE+2 3600x2450 frame (49 voice + 23 FEC). It uses the same bit layout, Golay(24,12) on A, Golay(23,12) on B, the PRNG(A) scrambling of B, and the same intra-frame interleave as a DMR voice frame. [HOST] uses the DMR tables directly (regenerateYSFDN), and [DSDCC] uses DMR's rW/rX/rY/rZ schedule.
- Positions of the bits within the 72 bits ([HOST] AMBEFEC.cpp), MSB of each word first:
  - A (24): 0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,1,5,9,13,17,21
  - B (23): 25,29,33,37,41,45,49,53,57,61,65,69,2,6,10,14,18,22,26,30,34,38,42
  - C (25): 46,50,54,58,62,66,70,3,7,11,15,19,23,27,31,35,39,43,47,51,55,59,63,67,71

**DCH content per FN** ([SPEC] Table 4-3, garbled in the PDF; [HOST]; [DSDCC]):

| FN | Content |
|---|---|
| 0 | CSD1 (dest+src) |
| 1 | CSD2 (down+up) |
| 2 | CSD3 (Rem1–4) |
| 3..7 | DT1..DT5 (20 bytes each, up to 100 bytes) |

FT is 2..7 depending on the data length.

**BER** ([HOST]): bit differences after Golay re-encode of A(24) + B(23) = 47 per VCH, 235 per frame. An undecodable A counts as 10 errors.

**When it is used.** [SPEC] 4.2.3: radios may switch between type 1 and type 2 mid-transmission to send text, completing at least one FN roll. No V/D-1 capture was available, so the radio behaviour is unverified.

### 4.5 Voice FR mode ("VW", DT=3)

**Sequence** ([SPEC] 4.4, Fig 4-20; [CAP] FTM-400D VW):

| Frame | FICH | Content |
|---|---|---|
| HC | FT=0 | Normal HC, same layout as other modes |
| CC0 "sub-header" | FN=0, FT=1 | See layout below |
| next CC | FN=1, FT=1 | Seen in [CAP]. Normal full-IMBE layout. |
| all later CC | FN=0, FT=0 | Full IMBE |
| TC | FT=0 | Normal TC |

**Sub-header (CC0) layout:**

| Payload bits | Content |
|---|---|
| 0–359 | One long DCH, contiguous and not split, carrying CSD3. Its interleave is the 20×9 rule applied to these 360 bits directly. |
| 360–431 | 72 reserved bits |
| 432–575 | VCH(3) |
| 576–719 | VCH(4) |

**Normal CC layout:** 5 × 144-bit VCH at payload bits 144j … 144j+143.

[HOST] treats only FN=0 with FT=1 as the sub-header (processVoiceFRModeData + Audio2 at payload bytes 54 and 72). It treats everything else as 5 IMBE frames. In [CAP], the CSD3 of the sub-header was "               F0Ckw".

**VCH = AMBE+2 Enhanced Full-Rate, 144 bits** (88 voice + 56 FEC; [SPEC] 4.4(7)). Its FEC, scrambling and interleave are identical to the P25 Phase 1 IMBE 7200 bit/s frame. [HOST] runs the same regenerateIMBE() for P25 and YSF, and [DSDCC] uses the same structure. [CAP] check: after de-interleaving, 71 of 275 VW c0 words had Golay syndrome 0 even with a crude demodulator; random data would give about 0.05 %.

In FEC (de-interleaved) bit order:

| Bits | Field | Code |
|---|---|---|
| 0–22 | c0 | Golay(23,12): 12 data bits, then 11 parity. Poly 0xC75. |
| 23–45 | c1 | Golay(23,12) |
| 46–68 | c2 | Golay(23,12) |
| 69–91 | c3 | Golay(23,12) |
| 92–106 | c4 | Hamming(15,11) |
| 107–121 | c5 | Hamming(15,11) |
| 122–136 | c6 | Hamming(15,11) |
| 137–143 | c7 | 7 unprotected bits |

**Hamming(15,11)**, P25 variant ([HOST] Hamming.cpp decode15113_1). For data d0..d10 followed by parity bits 11..14:
- p11 = d0⊕d1⊕d2⊕d3⊕d4⊕d5⊕d6
- p12 = d0⊕d1⊕d2⊕d3⊕d7⊕d8⊕d9
- p13 = d0⊕d1⊕d4⊕d5⊕d7⊕d8⊕d10
- p14 = d0⊕d2⊕d4⊕d6⊕d7⊕d9⊕d10

**Scrambling** ([HOST]; [DSDCC] scrambleVFR(seed = c0 data, shift 4)):
- p0 = 16 × (12-bit c0 data).
- p(k+1) = (173·p(k) + 13849) mod 65536.
- Whitening bit k (k = 0..113) = 1 if p(k+1) ≥ 32768.
- XOR it onto FEC bits 23..136 (c1..c6). Not onto c0 or c7.

**Interleave** ([HOST] IMBE_INTERLEAVE; [DSDCC] m_vfrInterleave is its inverse): FEC bit i is transmitted at block position T[i]. The table, in 6 rows of 24:

```
 0  7 12 19 24 31 36 43 48 55 60 67 72 79 84 91  96 103 108 115 120 127 132 139
 1  6 13 18 25 30 37 42 49 54 61 66 73 78 85 90  97 102 109 114 121 126 133 138
 2  9 14 21 26 33 38 45 50 57 62 69 74 81 86 93  98 105 110 117 122 129 134 141
 3  8 15 20 27 32 39 44 51 56 63 68 75 80 87 92  99 104 111 116 123 128 135 140
 4 11 16 23 28 35 40 47 52 59 64 71 76 83 88 95 100 107 112 119 124 131 136 143
 5 10 17 22 29 34 41 46 53 58 65 70 77 82 89 94 101 106 113 118 125 130 137 142
```

**BER** ([HOST]): bit differences after full FEC regenerate. That is 720 bits per frame, or 288 for the sub-header frame.

### 4.6 Data FR mode (DT=1)

**CC layout** ([SPEC] 4.3, Fig 4-15): same as HC — DCH-1(j) and DCH-2(j) alternate in 72-bit pieces. Two long DCH per frame, 40 bytes. Nothing is repeated.

**FN content** ([SPEC] Table 4-5, garbled in the PDF; [HOST] comments):

| FN | DCH-1 | DCH-2 |
|---|---|---|
| 0 | CSD1 | CSD2 |
| 1 | CSD3 | DT1 |
| 2 | DT2 | DT3 |
| … | … | … |
| 7 | DT12 | DT13 |

- Up to 260 bytes per block (FN 0..7).
- BT/BN allow up to 4 blocks (1040 bytes). One block is 0.8 s; the maximum transmission is 3.4 s ([SPEC] 4.3(7)).
- Wires-X uses this mode. Its contents are not described here.

---

## 5. Receiver acceptance in MMDVMHost ([HOST] YSFControl.cpp)

**Frame acceptance:**
- A frame is "valid" only if the FICH decodes: all 4 Golay words are correctable and the CRC is OK.
- If the FICH is invalid while a call is active, MMDVMHost extrapolates from the last FICH. In DN it uses FN+1, wrapping past FT to 0.

**Call start:**
- **Header path:** a valid HC whose long DCH-1 (CSD1) passes its CRC starts the call. Source = CSD1 Src. Dest = "ALL" if CM is 0 or 1, otherwise CSD1 Dest. This works for all DT types.
- **Late entry:** DN modes only (V/D 1 and V/D 2), and only from clean frames (valid FICH and DCH CRC OK).
  - V/D 1: an FN=0 frame (CSD1) gives both callsigns.
  - V/D 2: needs FN=0 (Dest) and FN=1 (Src), so two frames before audio starts.
  - VW and Data FR have no late entry: they need the HC.

**Call end:**
- A valid TC ends the call. MMDVMHost parses its DCH like a header and logs "received RF end of transmission … BER x%".
- Loss of sync ends it as "transmission lost". The modem reports loss after missing sync for MAX_SYNC_FRAMES.

**Options and constants:**
- SelfOnly: reject sources other than the node callsign.
- MMDVM-Host.ini `[System Fusion]`: Enable, LowDeviation (0/1), SelfOnly, TXHang (s), RemoteGateway, ModeHang. YSFTXLevel is in the `[Modem]` section.
  - LowDeviation sets FICH Dev=1 on retransmitted frames ([HOST]).
  - It also selects the narrow TX levels in the modem: ±948/±316 versus ±1893/±631 ([FW]). On MMDVM_HS it selects the narrow deviation word plus a different discriminator bandwidth and slicer threshold ([HS]).
- MMDVM firmware TX delay: 600 + 12·TXDelay bytes of 0x77 preamble, maximum 1200 bytes. TX hang is filled with zero-level symbols ([FW] YSFTX).
- YSF_FRAME_TIME = 100 ms ([HOST]).

---

## 6. Byte map, for implementers

Frame bytes: 0–4 sync, 5–29 FICH, 30–119 payload. Payload byte q = frame byte 30+q, and each 18-byte group j covers payload bytes 18j … 18j+17.

| Mode | Payload bytes 18j…18j+17 |
|---|---|
| HC / TC / Data FR | DCH-1 = bytes 18j..18j+8, DCH-2 = bytes 18j+9..18j+17 |
| V/D 1 | DCH = bytes 18j..18j+8, VCH = bytes 18j+9..18j+17 |
| V/D 2 | DCH = bytes 18j..18j+4, voice block = bytes 18j+5..18j+17 (104 bits) |
| VW normal | VCH(j) = bytes 18j..18j+17 |
| VW sub-header | DCH = bytes 0–44, reserved = 45–53, VCH(3) = 54–71, VCH(4) = 72–89 |

---

## 7. Public recordings

| Recording | Format | URL |
|---|---|---|
| FTM-400D DN mode, direct, Motorola GM300 discriminator (1 HC, 201 CC, 1 TC) | WAV, 44.1 kHz, 24-bit mono | https://qsl.net/kb9mwr/projects/dv/yaesu/SystemFusion_DN_PY2DZA_DiscrOut.wav |
| FTM-400D VW mode, same setup | WAV, 44.1 kHz, 24-bit mono | https://qsl.net/kb9mwr/projects/dv/yaesu/SystemFusion_VW_PY2DZA_DiscrOut.wav |
| Notes on the two recordings above | text | https://qsl.net/kb9mwr/projects/dv/yaesu/Recording%20Information.txt |
| F5ZOO repeater downlink, DN, two callers (FT=6 and FT=7) | discriminator, S16LE 48 kS/s mono | https://github.com/f4exb/dsdcc/blob/master/samples/ysf_f5zoo.dis |

The sigidwiki "System Fusion" page has no samples. No public IQ (complex) YSF recording was found.

---

## 8. Uncertain or conflicting points

1. **DG-ID.** It lives in byte 3 bits 6–0 (the [SPEC] SC field) per MMDVMHost. Its interaction with the SQL bit, and whether radios set bit 7, are unverified. [CAP] shows SQL=0, SC=0 everywhere.
2. **MR width.** It is 3 bits in [SPEC] and [DSDCC] and in MMDVMHost's setMR, but MMDVMHost's getMR reads only 2 bits.
3. **Preamble length.** About 200 symbols of 0x77 (+3/−3), measured once on an FTM-400D. Other radios, and narrow mode, are unknown. [SPEC] does not define a preamble.
4. **Header count.** [SPEC] recommends N0 = 1. The FTM-400D sent 1. There is no data for other models, or for repeaters keying up.
5. **V/D mode 1, Data FR and Test frames.** The layouts are from [SPEC], [HOST] and [DSDCC], but I had no captures. The [SPEC] tables 4-3 and 4-5 are partly garbled in the PDF text; the FN content above follows [HOST] and [DSDCC].
6. **Narrow (Dev=1) transmissions.** [SPEC] gives 450/1350 Hz. No capture.
7. **DT1/DT2 byte meanings** (radio type, GPS) are not documented in [SPEC]. Treat them as opaque, or send what the converters send.
8. **VW second frame.** In [CAP] the frame after the sub-header has FN=1, FT=1 before FN=0, FT=0 continues. [SPEC] Fig 4-20 is ambiguous here. MMDVMHost decodes it as a normal 5-IMBE frame.
9. **Pad bit.** The VD2 pad bit (position 103) should be 0. [HOST] remarks that it sometimes is not.
10. **TX filter.** The [SPEC] filter formula is garbled. The square-root-RC reading matches MMDVM's RRC 0.2 TX.
11. **Sync tolerance.** The 3-symbol-error sync threshold is an MMDVM implementation choice, not part of [SPEC].
