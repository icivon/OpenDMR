/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden without the written
 *    permission of the copyright holder. This includes, without limitation, incorporation in a commercial product or
 *    incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, service: the Codec 2 3200 decoder (c2dec.h), step by step as m17/codec2-3200.md describes it (the section
 * numbers below are that file's); m17/py/c2_3200_model.py is the same in floating point.
 *
 * All integer (no FPU): the LPC spectrum |A|, arg A straight from the LSPs in product form, A = e^(-j 5.5 w)
 * (cos(w/2) prod(2 cos w - 2 cos w_even) + j sin(w/2) prod(2 cos w - 2 cos w_odd)), with normalised mantissas (exact
 * near the sharp peaks close LSPs make) and CORDIC for the angle; the post filter's A(z / gamma) from the LPC
 * coefficients (gamma = 1/2: smooth); power spectra, the post filter, band sums and square roots in log2 (Q16), sums
 * through block exponents; the synthesis an oscillator bank on the DFT grid.
 */

#include <string.h>

#include "c2dec.h"

#ifdef C2DEC_PROFILE // the time each step takes (the firmware's debug build: c2dec_prof over the cable), us
uint32_t systime_get_us(void);
uint32_t c2dec_prof[6];
static uint32_t prof_t;
#define PROF(n) do { uint32_t t_ = systime_get_us(); if ((n) >= 0) c2dec_prof[(n) < 0 ? 0 : (n)] += t_ - prof_t; prof_t = t_; } while (0)
#else
#define PROF(n)
#endif

#define N      C2DEC_N
#define BINS   C2DEC_BINS
#define MAX_L  80
#define WO_MIN (uint32_t)(4294967296ULL / 160) // 50 Hz
#define WO_150 80530637U                        // 150 Hz
#define W_STEP 8                                // the post filter's |A(w / gamma)| computed every 8 bins

// log2 Q16 constants
#define ONE         65536
#define NEG         (-(1 << 28)) // log2 of 0
#define L2_E0       (-217706)    // the energy's -10 dB
#define L2_E_STEP   34017        // its 50 / 32 dB step
#define L2_1E6      (-1306235)
#define L2_1E4      (-870824)
#define L2_BASS     63626        // log2(1.4^2)
#define L2_LPC_CORR (-325438)    // log2(0.032)
#define DB_PER_L2   197283       // 10 log10(2), Q16
#define L2_PER_DB20 10885        // log2(10) / 20, Q16

#define sintab c2_sintab

#ifndef C2DEC_BASS_BOOST
#define C2DEC_BASS_BOOST 0
#endif
// the reference decoder's +3 dB under 1 kHz (codec2-3200.md 3.3); 0: the +3 dB at all frequencies, as loud and flat
// (the reference's is 2.5 - 4.5 dB down at 1 - 3.5 kHz against the speech, AMBE+2's 0 - 2: "muddy", 2026-11-20)
volatile uint32_t c2dec_bass_boost = C2DEC_BASS_BOOST;

const int16_t c2_sintab[1024] = { // sin, Q15, 1024 a turn (also c2enc.c)
         0,    201,    402,    603,    804,   1005,   1206,   1407,   1608,   1809,   2009,   2210,   2410,   2611,   2811,   3012,
      3212,   3412,   3612,   3811,   4011,   4210,   4410,   4609,   4808,   5007,   5205,   5404,   5602,   5800,   5998,   6195,
      6393,   6590,   6786,   6983,   7179,   7375,   7571,   7767,   7962,   8157,   8351,   8545,   8739,   8933,   9126,   9319,
      9512,   9704,   9896,  10087,  10278,  10469,  10659,  10849,  11039,  11228,  11417,  11605,  11793,  11980,  12167,  12353,
     12539,  12725,  12910,  13094,  13279,  13462,  13645,  13828,  14010,  14191,  14372,  14553,  14732,  14912,  15090,  15269,
     15446,  15623,  15800,  15976,  16151,  16325,  16499,  16673,  16846,  17018,  17189,  17360,  17530,  17700,  17869,  18037,
     18204,  18371,  18537,  18703,  18868,  19032,  19195,  19357,  19519,  19680,  19841,  20000,  20159,  20317,  20475,  20631,
     20787,  20942,  21096,  21250,  21403,  21554,  21705,  21856,  22005,  22154,  22301,  22448,  22594,  22739,  22884,  23027,
     23170,  23311,  23452,  23592,  23731,  23870,  24007,  24143,  24279,  24413,  24547,  24680,  24811,  24942,  25072,  25201,
     25329,  25456,  25582,  25708,  25832,  25955,  26077,  26198,  26319,  26438,  26556,  26674,  26790,  26905,  27019,  27133,
     27245,  27356,  27466,  27575,  27683,  27790,  27896,  28001,  28105,  28208,  28310,  28411,  28510,  28609,  28706,  28803,
     28898,  28992,  29085,  29177,  29268,  29358,  29447,  29534,  29621,  29706,  29791,  29874,  29956,  30037,  30117,  30195,
     30273,  30349,  30424,  30498,  30571,  30643,  30714,  30783,  30852,  30919,  30985,  31050,  31113,  31176,  31237,  31297,
     31356,  31414,  31470,  31526,  31580,  31633,  31685,  31736,  31785,  31833,  31880,  31926,  31971,  32014,  32057,  32098,
     32137,  32176,  32213,  32250,  32285,  32318,  32351,  32382,  32412,  32441,  32469,  32495,  32521,  32545,  32567,  32589,
     32609,  32628,  32646,  32663,  32678,  32692,  32705,  32717,  32728,  32737,  32745,  32752,  32757,  32761,  32765,  32766,
     32767,  32766,  32765,  32761,  32757,  32752,  32745,  32737,  32728,  32717,  32705,  32692,  32678,  32663,  32646,  32628,
     32609,  32589,  32567,  32545,  32521,  32495,  32469,  32441,  32412,  32382,  32351,  32318,  32285,  32250,  32213,  32176,
     32137,  32098,  32057,  32014,  31971,  31926,  31880,  31833,  31785,  31736,  31685,  31633,  31580,  31526,  31470,  31414,
     31356,  31297,  31237,  31176,  31113,  31050,  30985,  30919,  30852,  30783,  30714,  30643,  30571,  30498,  30424,  30349,
     30273,  30195,  30117,  30037,  29956,  29874,  29791,  29706,  29621,  29534,  29447,  29358,  29268,  29177,  29085,  28992,
     28898,  28803,  28706,  28609,  28510,  28411,  28310,  28208,  28105,  28001,  27896,  27790,  27683,  27575,  27466,  27356,
     27245,  27133,  27019,  26905,  26790,  26674,  26556,  26438,  26319,  26198,  26077,  25955,  25832,  25708,  25582,  25456,
     25329,  25201,  25072,  24942,  24811,  24680,  24547,  24413,  24279,  24143,  24007,  23870,  23731,  23592,  23452,  23311,
     23170,  23027,  22884,  22739,  22594,  22448,  22301,  22154,  22005,  21856,  21705,  21554,  21403,  21250,  21096,  20942,
     20787,  20631,  20475,  20317,  20159,  20000,  19841,  19680,  19519,  19357,  19195,  19032,  18868,  18703,  18537,  18371,
     18204,  18037,  17869,  17700,  17530,  17360,  17189,  17018,  16846,  16673,  16499,  16325,  16151,  15976,  15800,  15623,
     15446,  15269,  15090,  14912,  14732,  14553,  14372,  14191,  14010,  13828,  13645,  13462,  13279,  13094,  12910,  12725,
     12539,  12353,  12167,  11980,  11793,  11605,  11417,  11228,  11039,  10849,  10659,  10469,  10278,  10087,   9896,   9704,
      9512,   9319,   9126,   8933,   8739,   8545,   8351,   8157,   7962,   7767,   7571,   7375,   7179,   6983,   6786,   6590,
      6393,   6195,   5998,   5800,   5602,   5404,   5205,   5007,   4808,   4609,   4410,   4210,   4011,   3811,   3612,   3412,
      3212,   3012,   2811,   2611,   2410,   2210,   2009,   1809,   1608,   1407,   1206,   1005,    804,    603,    402,    201,
         0,   -201,   -402,   -603,   -804,  -1005,  -1206,  -1407,  -1608,  -1809,  -2009,  -2210,  -2410,  -2611,  -2811,  -3012,
     -3212,  -3412,  -3612,  -3811,  -4011,  -4210,  -4410,  -4609,  -4808,  -5007,  -5205,  -5404,  -5602,  -5800,  -5998,  -6195,
     -6393,  -6590,  -6786,  -6983,  -7179,  -7375,  -7571,  -7767,  -7962,  -8157,  -8351,  -8545,  -8739,  -8933,  -9126,  -9319,
     -9512,  -9704,  -9896, -10087, -10278, -10469, -10659, -10849, -11039, -11228, -11417, -11605, -11793, -11980, -12167, -12353,
    -12539, -12725, -12910, -13094, -13279, -13462, -13645, -13828, -14010, -14191, -14372, -14553, -14732, -14912, -15090, -15269,
    -15446, -15623, -15800, -15976, -16151, -16325, -16499, -16673, -16846, -17018, -17189, -17360, -17530, -17700, -17869, -18037,
    -18204, -18371, -18537, -18703, -18868, -19032, -19195, -19357, -19519, -19680, -19841, -20000, -20159, -20317, -20475, -20631,
    -20787, -20942, -21096, -21250, -21403, -21554, -21705, -21856, -22005, -22154, -22301, -22448, -22594, -22739, -22884, -23027,
    -23170, -23311, -23452, -23592, -23731, -23870, -24007, -24143, -24279, -24413, -24547, -24680, -24811, -24942, -25072, -25201,
    -25329, -25456, -25582, -25708, -25832, -25955, -26077, -26198, -26319, -26438, -26556, -26674, -26790, -26905, -27019, -27133,
    -27245, -27356, -27466, -27575, -27683, -27790, -27896, -28001, -28105, -28208, -28310, -28411, -28510, -28609, -28706, -28803,
    -28898, -28992, -29085, -29177, -29268, -29358, -29447, -29534, -29621, -29706, -29791, -29874, -29956, -30037, -30117, -30195,
    -30273, -30349, -30424, -30498, -30571, -30643, -30714, -30783, -30852, -30919, -30985, -31050, -31113, -31176, -31237, -31297,
    -31356, -31414, -31470, -31526, -31580, -31633, -31685, -31736, -31785, -31833, -31880, -31926, -31971, -32014, -32057, -32098,
    -32137, -32176, -32213, -32250, -32285, -32318, -32351, -32382, -32412, -32441, -32469, -32495, -32521, -32545, -32567, -32589,
    -32609, -32628, -32646, -32663, -32678, -32692, -32705, -32717, -32728, -32737, -32745, -32752, -32757, -32761, -32765, -32766,
    -32767, -32766, -32765, -32761, -32757, -32752, -32745, -32737, -32728, -32717, -32705, -32692, -32678, -32663, -32646, -32628,
    -32609, -32589, -32567, -32545, -32521, -32495, -32469, -32441, -32412, -32382, -32351, -32318, -32285, -32250, -32213, -32176,
    -32137, -32098, -32057, -32014, -31971, -31926, -31880, -31833, -31785, -31736, -31685, -31633, -31580, -31526, -31470, -31414,
    -31356, -31297, -31237, -31176, -31113, -31050, -30985, -30919, -30852, -30783, -30714, -30643, -30571, -30498, -30424, -30349,
    -30273, -30195, -30117, -30037, -29956, -29874, -29791, -29706, -29621, -29534, -29447, -29358, -29268, -29177, -29085, -28992,
    -28898, -28803, -28706, -28609, -28510, -28411, -28310, -28208, -28105, -28001, -27896, -27790, -27683, -27575, -27466, -27356,
    -27245, -27133, -27019, -26905, -26790, -26674, -26556, -26438, -26319, -26198, -26077, -25955, -25832, -25708, -25582, -25456,
    -25329, -25201, -25072, -24942, -24811, -24680, -24547, -24413, -24279, -24143, -24007, -23870, -23731, -23592, -23452, -23311,
    -23170, -23027, -22884, -22739, -22594, -22448, -22301, -22154, -22005, -21856, -21705, -21554, -21403, -21250, -21096, -20942,
    -20787, -20631, -20475, -20317, -20159, -20000, -19841, -19680, -19519, -19357, -19195, -19032, -18868, -18703, -18537, -18371,
    -18204, -18037, -17869, -17700, -17530, -17360, -17189, -17018, -16846, -16673, -16499, -16325, -16151, -15976, -15800, -15623,
    -15446, -15269, -15090, -14912, -14732, -14553, -14372, -14191, -14010, -13828, -13645, -13462, -13279, -13094, -12910, -12725,
    -12539, -12353, -12167, -11980, -11793, -11605, -11417, -11228, -11039, -10849, -10659, -10469, -10278, -10087,  -9896,  -9704,
     -9512,  -9319,  -9126,  -8933,  -8739,  -8545,  -8351,  -8157,  -7962,  -7767,  -7571,  -7375,  -7179,  -6983,  -6786,  -6590,
     -6393,  -6195,  -5998,  -5800,  -5602,  -5404,  -5205,  -5007,  -4808,  -4609,  -4410,  -4210,  -4011,  -3811,  -3612,  -3412,
     -3212,  -3012,  -2811,  -2611,  -2410,  -2210,  -2009,  -1809,  -1608,  -1407,  -1206,  -1005,   -804,   -603,   -402,   -201,
};

// log2(1 + i / 256), Q16
static const uint32_t log2tab[257] = {
         0,    369,    736,   1102,   1466,   1829,   2190,   2551,   2909,   3267,   3623,   3978,
      4331,   4683,   5034,   5384,   5732,   6079,   6425,   6769,   7112,   7454,   7795,   8134,
      8473,   8810,   9146,   9480,   9814,  10146,  10477,  10807,  11136,  11464,  11791,  12116,
     12440,  12764,  13086,  13407,  13727,  14046,  14363,  14680,  14996,  15310,  15624,  15937,
     16248,  16559,  16868,  17177,  17484,  17791,  18096,  18401,  18704,  19007,  19308,  19609,
     19909,  20207,  20505,  20802,  21098,  21393,  21687,  21980,  22272,  22564,  22854,  23144,
     23433,  23720,  24007,  24293,  24579,  24863,  25146,  25429,  25711,  25992,  26272,  26551,
     26830,  27108,  27384,  27660,  27936,  28210,  28484,  28757,  29029,  29300,  29571,  29840,
     30109,  30378,  30645,  30912,  31178,  31443,  31707,  31971,  32234,  32496,  32758,  33019,
     33279,  33538,  33797,  34055,  34312,  34569,  34825,  35080,  35334,  35588,  35841,  36094,
     36346,  36597,  36847,  37097,  37346,  37595,  37842,  38090,  38336,  38582,  38827,  39072,
     39316,  39559,  39802,  40044,  40286,  40527,  40767,  41006,  41246,  41484,  41722,  41959,
     42196,  42432,  42667,  42902,  43137,  43370,  43603,  43836,  44068,  44300,  44530,  44761,
     44990,  45220,  45448,  45676,  45904,  46131,  46357,  46583,  46809,  47034,  47258,  47482,
     47705,  47928,  48150,  48372,  48593,  48813,  49034,  49253,  49472,  49691,  49909,  50127,
     50344,  50560,  50776,  50992,  51207,  51422,  51636,  51850,  52063,  52276,  52488,  52700,
     52911,  53122,  53332,  53542,  53751,  53960,  54169,  54377,  54584,  54791,  54998,  55204,
     55410,  55615,  55820,  56025,  56229,  56432,  56635,  56838,  57040,  57242,  57443,  57644,
     57845,  58045,  58245,  58444,  58643,  58841,  59039,  59237,  59434,  59631,  59827,  60023,
     60219,  60414,  60609,  60803,  60997,  61190,  61384,  61576,  61769,  61961,  62152,  62343,
     62534,  62725,  62915,  63104,  63294,  63483,  63671,  63859,  64047,  64234,  64421,  64608,
     64794,  64980,  65166,  65351,  65536,
};

// 2^(i / 256), Q30
static const uint32_t exp2tab[257] = {
     1073741824,  1076653033,  1079572136,  1082499153,  1085434106,  1088377016,  1091327906,  1094286796,
     1097253708,  1100228665,  1103211687,  1106202798,  1109202018,  1112209370,  1115224875,  1118248556,
     1121280436,  1124320536,  1127368878,  1130425485,  1133490379,  1136563583,  1139645120,  1142735011,
     1145833280,  1148939949,  1152055042,  1155178580,  1158310587,  1161451085,  1164600099,  1167757650,
     1170923762,  1174098458,  1177281762,  1180473697,  1183674286,  1186883552,  1190101520,  1193328213,
     1196563654,  1199807867,  1203060876,  1206322705,  1209593378,  1212872918,  1216161350,  1219458698,
     1222764986,  1226080238,  1229404479,  1232737732,  1236080024,  1239431376,  1242791816,  1246161366,
     1249540052,  1252927899,  1256324931,  1259731174,  1263146652,  1266571390,  1270005413,  1273448747,
     1276901417,  1280363448,  1283834865,  1287315695,  1290805962,  1294305692,  1297814910,  1301333643,
     1304861917,  1308399756,  1311947188,  1315504238,  1319070932,  1322647296,  1326233356,  1329829140,
     1333434672,  1337049980,  1340675091,  1344310030,  1347954824,  1351609500,  1355274085,  1358948606,
     1362633090,  1366327563,  1370032052,  1373746586,  1377471191,  1381205894,  1384950723,  1388705706,
     1392470869,  1396246240,  1400031848,  1403827719,  1407633882,  1411450365,  1415277195,  1419114401,
     1422962010,  1426820052,  1430688553,  1434567544,  1438457051,  1442357104,  1446267730,  1450188960,
     1454120821,  1458063343,  1462016553,  1465980482,  1469955159,  1473940611,  1477936870,  1481943963,
     1485961921,  1489990772,  1494030547,  1498081275,  1502142985,  1506215708,  1510299473,  1514394310,
     1518500250,  1522617322,  1526745556,  1530884983,  1535035634,  1539197537,  1543370725,  1547555228,
     1551751076,  1555958300,  1560176931,  1564406999,  1568648537,  1572901575,  1577166143,  1581442275,
     1585730000,  1590029350,  1594340357,  1598663052,  1602997467,  1607343634,  1611701585,  1616071351,
     1620452965,  1624846459,  1629251865,  1633669214,  1638098541,  1642539877,  1646993254,  1651458706,
     1655936265,  1660425963,  1664927835,  1669441912,  1673968228,  1678506817,  1683057710,  1687620943,
     1692196547,  1696784557,  1701385007,  1705997930,  1710623359,  1715261330,  1719911875,  1724575029,
     1729250827,  1733939301,  1738640488,  1743354420,  1748081133,  1752820662,  1757573041,  1762338305,
     1767116489,  1771907628,  1776711757,  1781528911,  1786359126,  1791202437,  1796058879,  1800928489,
     1805811301,  1810707353,  1815616678,  1820539314,  1825475297,  1830424663,  1835387448,  1840363688,
     1845353420,  1850356681,  1855373507,  1860403934,  1865448001,  1870505744,  1875577199,  1880662405,
     1885761398,  1890874216,  1896000896,  1901141476,  1906295993,  1911464486,  1916646992,  1921843549,
     1927054196,  1932278970,  1937517909,  1942771053,  1948038440,  1953320108,  1958616096,  1963926443,
     1969251188,  1974590370,  1979944027,  1985312200,  1990694927,  1996092249,  2001504204,  2006930832,
     2012372174,  2017828268,  2023299156,  2028784876,  2034285470,  2039800978,  2045331439,  2050876895,
     2056437387,  2062012954,  2067603638,  2073209480,  2078830522,  2084466803,  2090118366,  2095785251,
     2101467502,  2107165158,  2112878262,  2118606857,  2124350982,  2130110682,  2135885998,  2141676973,
     2147483648,
};

// atan(2^-i), 2^32: 2 pi
static const uint32_t atantab[16] = {
    536870912, 316933406, 167458907, 85004756, 42667331, 21354465, 10679838, 5340245,
    2670163,   1335087,   667544,    333772,   166886,   83443,    41722,    20861,
};

/* ---- arithmetic ---- */

// sin, cos of a phase (2^32: 2 pi), Q15, interpolated
static int32_t isin(uint32_t p)
{
    uint32_t i = p >> 22;
    int32_t a = sintab[i], b = sintab[(i + 1) & 1023];
    return a + (((b - a) * (int32_t)((p >> 6) & 0xFFFF)) >> 16);
}

static int32_t icos(uint32_t p)
{
    return isin(p + 0x40000000U);
}

// log2(x), x > 0, Q16 (32 bits: the CPU multiplies 32 x 32 -> 32 only, wider is a library call)
static int32_t ilog2_32(uint32_t x)
{
    int n = 31 - __builtin_clz(x);
    uint32_t f = n ? x << (32 - n) : 0; // the fraction after the leading one
    uint32_t i = f >> 24, t = (f >> 8) & 0xFFFF;
    return n * ONE + (int32_t)(log2tab[i] + (((log2tab[i + 1] - log2tab[i]) * t) >> 16));
}

int32_t c2_ilog2(uint64_t x)
{
    uint32_t hi = (uint32_t)(x >> 32);
    if (!hi) {
        return ilog2_32((uint32_t)x);
    }
    int s = 32 - __builtin_clz(hi);
    return ilog2_32((uint32_t)(x >> s)) + s * ONE;
}

// 2^x for x <= 0 (Q16), Q30 (0 below 2^-30)
static uint32_t iexp2_neg(int32_t x)
{
    if (x < -30 * ONE) {
        return 0;
    }
    int32_t ip = x >> 16; // floor
    uint32_t fr = (uint32_t)x & 0xFFFF, i = fr >> 8, t = fr & 0xFF;
    uint32_t m = exp2tab[i] + (((exp2tab[i + 1] - exp2tab[i]) * t) >> 8);
    return m >> -ip;
}

// log2(the sum of 2^v[i], lo <= i < hi)
static int32_t log2_sum(const int32_t *v, unsigned lo, unsigned hi)
{
    int32_t max = NEG;
    for (unsigned i = lo; i < hi; i++) {
        if (v[i] > max) {
            max = v[i];
        }
    }
    if (max <= NEG) {
        return NEG;
    }
    uint64_t acc = 0;
    for (unsigned i = lo; i < hi; i++) {
        acc += iexp2_neg(v[i] - max);
    }
    return c2_ilog2(acc) - 30 * ONE + max;
}

// log2(2^a + 2^b)
static int32_t log2_add(int32_t a, int32_t b)
{
    int32_t hi = a > b ? a : b, lo = a > b ? b : a;
    return hi + c2_ilog2((1ULL << 30) + iexp2_neg(lo - hi)) - 30 * ONE;
}

// v (not 0) as m 2^e: |m| in [2^14, 2^15), the shift added to *e
static int32_t norm15(int32_t v, int32_t *e)
{
    int lz = __builtin_clz((uint32_t)(v < 0 ? -v : v)); // 17: |v| in [2^14, 2^15)
    if (lz < 17) {
        *e += 17 - lz;
        return v >> (17 - lz);
    }
    *e -= lz - 17;
    return v << (lz - 17);
}

// a running product m 2^e (|m| in [2^14, 2^15), or 0) times f 2^-28: 15 bits each, a 32 bit multiply
static void mul_norm(int32_t *m, int32_t *e, int32_t f)
{
    if (!f) {
        *m = 0;
        return;
    }
    int32_t fe = -28;
    f = norm15(f, &fe);
    *e += fe;
    *m = norm15(*m * f, e);
}

// A(e^jw) e^(j 5.5 w) = (x + j y) 2^e, |x|, |y| < 2^15; false: 0
static bool lpc_spectrum(const int32_t r[10], uint32_t w, int32_t *x, int32_t *y, int32_t *e)
{
    int32_t x2 = icos(w) << 14; // 2 cos w, Q28
    int32_t pm = icos(w >> 1), pe = -15;
    int32_t qm = isin(w >> 1), qe = -15;
    if (pm) {
        pm = norm15(pm, &pe);
    }
    if (qm) {
        qm = norm15(qm, &qe);
    }
    for (int i = 0; i < 10; i += 2) {
        if (pm) {
            mul_norm(&pm, &pe, x2 - r[i]);
        }
        if (qm) {
            mul_norm(&qm, &qe, x2 - r[i + 1]);
        }
    }
    if (!pm && !qm) {
        return false;
    }
    *e = (!pm) ? qe : (!qm) ? pe : (pe > qe ? pe : qe);
    *x = (pm && *e - pe < 31) ? (pm >> (*e - pe)) : 0;
    *y = (qm && *e - qe < 31) ? (qm >> (*e - qe)) : 0;
    return true;
}

// the angle of (x, y), 2^32: 2 pi
uint32_t c2_cordic_angle(int32_t x, int32_t y)
{
    uint32_t z = 0;
    if (x < 0) {
        x = -x;
        y = -y;
        z = 0x80000000U;
    }
    for (int i = 0; i < 16; i++) {
        int32_t nx;
        if (y > 0) {
            nx = x + (y >> i);
            y -= x >> i;
            z += atantab[i];
        } else {
            nx = x - (y >> i);
            y += x >> i;
            z -= atantab[i];
        }
        x = nx;
    }
    return z;
}

static uint32_t rand_phase(c2dec_t *d)
{
    d->rand = d->rand * 1103515245U + 12345U;
    return ((d->rand >> 16) & 0x7FFF) * 131076U; // 2 pi r / 32767
}

/* ---- a subframe (sections 3 - 5) ---- */

static void subframe(c2dec_t *d, const c2dec_model_t *mdl, int16_t *pcm)
{
    PROF(-1);
    uint32_t wo = mdl->wo;
    unsigned L = 0x80000000U / wo;
    if (L > MAX_L) {
        L = MAX_L;
    }
    int32_t r[10]; // 2 cos w_i, Q28
    for (int i = 0; i < 10; i++) {
        r[i] = icos(mdl->lsp[i]) << 14;
    }

    // the LPC coefficients for the post filter's A(z / gamma): P(z), Q(z) multiplied out (Q20)
    int64_t P[12] = {1 << 20}, Q[12] = {1 << 20};
    for (int i = 0; i < 10; i += 2) {
        for (int j = i + 2; j >= 1; j--) {
            P[j] += -((r[i] * P[j - 1]) >> 28) + ((j >= 2) ? P[j - 2] : 0);
            Q[j] += -((r[i + 1] * Q[j - 1]) >> 28) + ((j >= 2) ? Q[j - 2] : 0);
        }
    }
    int32_t c[11]; // a_k gamma^k = a_k / 2^k, Q20
    for (int k = 0; k <= 10; k++) {
        int64_t p = P[k] + ((k > 0) ? P[k - 1] : 0); // times (1 + 1/z)
        int64_t q = Q[k] - ((k > 0) ? Q[k - 1] : 0); // times (1 - 1/z)
        c[k] = (int32_t)(((p + q) / 2) >> k);
    }

    // the bins the harmonics take their phases from (section 4)
    uint32_t need[(BINS + 1 + 31) / 32];
    memset(need, 0, sizeof(need));
    for (unsigned m = 1; m <= L; m++) {
        unsigned b = (m * wo + (1U << 22)) >> 23;
        need[b >> 5] |= 1U << (b & 31);
    }

    // |A(w / gamma)|^2 from the coefficients, log2. It enters as its 5th root: 32 bits (Q10 Q14) are plenty. The phases
    // 2 pi j k / 512 are exactly on the sine table
    int32_t c10[11];
    for (int k = 0; k <= 10; k++) {
        c10[k] = c[k] >> 10;
    }
    int32_t lw_grid[BINS / W_STEP + 1];
    for (unsigned g = 0; g <= BINS / W_STEP; g++) {
        unsigned k = g * W_STEP;
        int32_t re = 0, im = 0;
        for (unsigned j = 0; j <= 10; j++) {
            unsigned idx = (2 * j * k) & 1023;
            re += c10[j] * (sintab[(idx + 256) & 1023] >> 1);
            im += c10[j] * (sintab[idx] >> 1);
        }
        uint32_t mx = (uint32_t)(re < 0 ? -re : re) | (uint32_t)(im < 0 ? -im : im);
        int sh = mx ? 17 - __builtin_clz(mx) : 0; // to below 2^15
        if (sh > 0) {
            re >>= sh;
            im >>= sh;
        } else {
            re <<= -sh;
            im <<= -sh;
        }
        uint32_t w2 = (uint32_t)(re * re + im * im);
        lw_grid[g] = w2 ? ilog2_32(w2) + 2 * sh * ONE - 48 * ONE : NEG; // Q24 squared
    }

    PROF(0); // the LPC coefficients, |A(w / gamma)|
    // section 3: Pw = 1 / (|A|^2 + 1e-6), the post filter, the energy
    int32_t pre[BINS];
    for (unsigned k = 0; k <= BINS; k++) {
        uint32_t w = (uint32_t)k << 23;
        int32_t x = 0, y = 0, e = 0;
        bool nonzero = lpc_spectrum(r, w, &x, &y, &e);
        if ((need[k >> 5] >> (k & 31)) & 1) {
            d->arg_h[k] = 5 * w + (w >> 1) - (nonzero ? c2_cordic_angle(x << 13, y << 13) : 0); // -arg A
        }
        if (k == BINS) {
            break;
        }
        int32_t a2 = nonzero ? ilog2_32((uint32_t)(x * x + y * y)) + 2 * e * ONE : NEG;
        int32_t lp = -log2_add(a2, L2_1E6);
        // |A(w / gamma)|^2, smooth: every W_STEP bins, linear (log2) in between
        int32_t lw = lw_grid[k / W_STEP] + (((lw_grid[k / W_STEP + 1] - lw_grid[k / W_STEP]) * (int32_t)(k % W_STEP)) >> 3);
        pre[k] = lp;
        d->pw[k] = (lw > NEG) ? lp + (lw + lp) / 5 : NEG; // Pw (W Pw)^beta, beta = 1/5
    }
    int32_t gain = log2_add(log2_sum(pre, 0, BINS), L2_1E4) - log2_add(log2_sum(d->pw, 0, BINS), L2_1E4) + mdl->e;
    for (unsigned k = 0; k < BINS; k++) {
        d->pw[k] += gain + (((k < BINS / 4) || !c2dec_bass_boost) ? L2_BASS : 0);
    }

    PROF(1); // the spectrum
    // the amplitudes: the energy in each harmonic's band (log2 A_m, Q16)
    int32_t la[MAX_L + 1];
    for (unsigned m = 1; m <= L; m++) {
        unsigned lo = (m * wo - wo / 2 + (1U << 22)) >> 23; // round((m -+ 1/2) w0 / (2 pi / 512)), m w0 <= pi
        unsigned hi = (m * wo + wo / 2 + (1U << 22)) >> 23;
        if (hi > BINS) {
            hi = BINS;
        }
        la[m] = (lo < hi) ? log2_sum(d->pw, lo, hi) / 2 : NEG;
    }
    if (wo < WO_150 && la[1] > NEG) {
        la[1] += L2_LPC_CORR;
    }

    PROF(2); // the amplitudes
    // section 4: the phases
    uint32_t theta[MAX_L + 1];
    d->ex_phase += wo * N;
    for (unsigned m = 1; m <= L; m++) {
        unsigned b = (m * wo + (1U << 22)) >> 23;
        theta[m] = (mdl->voiced ? m * d->ex_phase : rand_phase(d)) + d->arg_h[b];
    }
    // the background level (dB); a voiced subframe's harmonics below it + 6 dB get random phases
    int32_t a2m[MAX_L + 1];
    for (unsigned m = 1; m <= L; m++) {
        a2m[m] = (la[m] > NEG) ? 2 * la[m] : NEG;
    }
    int32_t s = log2_sum(a2m, 1, L + 1);
    int32_t level = (s > NEG) ? (int32_t)(((int64_t)(s - c2_ilog2(L)) * DB_PER_L2) >> 16) : -120 * ONE;
    if (level < 40 * ONE && !mdl->voiced) {
        d->bg = d->bg - d->bg / 10 + level / 10;
    }
    int32_t thresh = (int32_t)(((int64_t)(d->bg + 6 * ONE) * L2_PER_DB20) >> 16);
    if (mdl->voiced) {
        for (unsigned m = 1; m <= L; m++) {
            if (la[m] < thresh) {
                theta[m] = rand_phase(d);
            }
        }
    }

    PROF(3); // the phases
    // section 5: 2 A_m cos(2 pi b n / 512 + theta_m), n = -79 .. 80, Q1
    int32_t buf[2 * N];
    memset(buf, 0, sizeof(buf));
    for (unsigned m = 1; m <= L; m++) {
        if (la[m] <= NEG) {
            continue;
        }
        int32_t l = la[m] + 2 * ONE; // 2 A_m, Q1
        uint32_t amp = (l >= 16 * ONE) ? 65535 : (iexp2_neg(l - 16 * ONE) >> 14);
        if (amp > 65535) {
            amp = 65535;
        }
        if (!amp) {
            continue;
        }
        unsigned b = (m * wo + (1U << 22)) >> 23;
        if (b > BINS - 1) {
            b = BINS - 1;
        }
        uint32_t step = (uint32_t)b << 23;
        uint32_t p = theta[m] - 79 * step + 0x40000000U + (1U << 21); // cos; the table index rounded
        int32_t a = (int32_t)amp;
        for (int n = 0; n < 2 * N; n++) {
            buf[n] += (a * sintab[p >> 22]) >> 15;
            p += step;
        }
    }
    PROF(4); // the oscillators
    // the triangle window, overlap add
    memmove(d->sn, d->sn + N, (N - 1) * sizeof(d->sn[0]));
    d->sn[N - 1] = 0;
    for (int i = 0; i < N - 1; i++) {
        d->sn[i] += buf[i] * i / N;
    }
    for (int i = N - 1; i < 2 * N; i++) {
        d->sn[i] = buf[i] * ((i < N) ? i : 2 * N - i) / N;
    }
    // out: 2 dB down for each dB the subframe's peak is over 30000
    int32_t max = 0;
    for (int i = 0; i < N; i++) {
        if (d->sn[i] > max) {
            max = d->sn[i];
        }
    }
    int32_t g = (max > 60000) ? (int32_t)((60000LL << 15) / max) : 32768;
    g = (g * g) >> 15;
    for (int i = 0; i < N; i++) {
        int32_t v = ((g == 32768) ? d->sn[i] : (int32_t)(((int64_t)d->sn[i] * g) >> 15)) / 2;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
    PROF(5); // the overlap add
}

/* ---- a frame (sections 1, 2) ---- */

static unsigned field(const uint8_t *frame, unsigned *bit, unsigned width)
{
    unsigned v = 0;
    for (unsigned n = 0; n < width; n++, (*bit)++) {
        v = (v << 1) | ((frame[*bit >> 3] >> (7 - (*bit & 7))) & 1);
    }
    for (unsigned s = 1; s < width; s <<= 1) { // Gray to binary
        v ^= v >> s;
    }
    return v;
}

void c2dec_init(c2dec_t *d)
{
    memset(d, 0, sizeof(*d));
    d->prev.wo = WO_MIN;
    for (int i = 0; i < 10; i++) {
        d->prev.lsp[i] = (uint32_t)(((uint64_t)i << 31) / 11); // i pi / 11
    }
    d->rand = 1;
}

void c2dec_decode(c2dec_t *d, const uint8_t frame[C2DEC_BYTES], int16_t pcm[C2DEC_SAMPLES])
{
    unsigned bit = 0;
    bool v0 = field(frame, &bit, 1);
    c2dec_model_t cur, mid;

    cur.voiced = field(frame, &bit, 1);
    cur.wo = (uint32_t)(((uint64_t)(128 + 7 * field(frame, &bit, 7)) << 32) / 20480); // 50 + 350 i / 128 Hz
    cur.e = L2_E0 + L2_E_STEP * (int32_t)field(frame, &bit, 5);
    unsigned hz = 0;
    for (int i = 0; i < 10; i++) {
        unsigned k = field(frame, &bit, 5);
        hz += (i >= 3 && i <= 5 && k >= 8) ? 200 + 50 * (k - 7) : 25 * (k + 1);
        cur.lsp[i] = (uint32_t)(((uint64_t)hz << 32) / 8000);
    }

    mid.voiced = v0 && (d->prev.voiced || cur.voiced);
    if (!mid.voiced) {
        mid.wo = WO_MIN;
    } else if (d->prev.voiced && cur.voiced) {
        mid.wo = d->prev.wo / 2 + cur.wo / 2;
    } else {
        mid.wo = cur.voiced ? cur.wo : d->prev.wo;
    }
    mid.e = (d->prev.e + cur.e) / 2;
    for (int i = 0; i < 10; i++) {
        mid.lsp[i] = (uint32_t)(((uint64_t)d->prev.lsp[i] + cur.lsp[i]) / 2);
    }

    subframe(d, &mid, pcm);
    subframe(d, &cur, pcm + N);
    d->prev = cur;
}
