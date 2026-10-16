/* blip25c public API: engine driver, half-rate FEC (blip25-vocoder src/halfrate/frame.rs, src/fec.rs) and bit
 * formats. */
#include "blip25.h"
#include "b25_internal.h"

/* ---- engine driver (blip25-vocoder src/engine/mod.rs Decoder) */
static const b25_dec_tables DEC_TABLES = {
    {TB_SUBMODE, TB_VOICING_PATTERNS, TB_GAIN32, TB_BLOCK_LENS, TB_PRBA9, TB_PRBA7, TB_HOC_A, TB_NIBBLES, TB_GAIN64,
     TB_HOC_WEIGHT, TB_HOC_LOADING, TB_CODEWORDS, TB_BLOB},
    TB_HILBERT,
    TB_SQRT80,
};

void blip25_init(blip25_decoder *dec) {
    memset(dec, 0, sizeof(*dec));
    b25_dec_init(dec->state, 1); /* Mode::HalfRate */
}

int blip25_decode_bits(blip25_decoder *dec, const uint8_t bits[49], int status, int16_t pcm[160]) {
    int16_t ctrl[26] = {0};
    int i;
    int16_t disp;
    /* FrameStatus::to_ctrl: word 0x24/2 = error rate 0..15, word 0x32/2 = forcing word (bit 2 = lost) */
    if (status < 0) {
        ctrl[0x24 / 2] = 15;
        ctrl[0x32 / 2] = 4;
    } else {
        ctrl[0x24 / 2] = (int16_t)(status > 15 ? 15 : status);
    }
    memset(dec->bits, 0, sizeof(dec->bits));
    for (i = 0; i < 49; i++) dec->bits[i] = (int16_t)(bits[i] & 1);
    b25_dec_ctrl(dec->state, ctrl);
    disp = b25_dec_synth(pcm, 80, dec->bits, 0, 0, 0, dec->state, &DEC_TABLES);
    b25_dec_synth(pcm + 80, 80, dec->bits, 0, 0, 1, dec->state, &DEC_TABLES);
    return (int)(uint16_t)disp;
}

int blip25_decode_2450(blip25_decoder *dec, const uint8_t frame[7], int16_t pcm[160]) {
    uint8_t bits[49];
    int i;
    for (i = 0; i < 49; i++) bits[i] = (uint8_t)((frame[i >> 3] >> (7 - (i & 7))) & 1);
    return blip25_decode_bits(dec, bits, 0, pcm);
}

int blip25_decode_2450_thumbdv(blip25_decoder *dec, const uint8_t frame[7], int16_t pcm[160]) {
    uint8_t bits[49];
    blip25_thumbdv_to_bits(frame, bits);
    return blip25_decode_bits(dec, bits, 0, pcm);
}

int blip25_decode_lost(blip25_decoder *dec, int16_t pcm[160]) {
    static const uint8_t zero[49] = {0};
    return blip25_decode_bits(dec, zero, BLIP25_LOST, pcm);
}

int blip25_decode_3600(blip25_decoder *dec, const uint8_t frame[9], int16_t pcm[160], int *errors) {
    uint8_t bits[49];
    int e = blip25_fec_3600(frame, bits, NULL);
    if (errors) *errors = e < 0 ? 4 : e;
    /* Vocoder::decode at HalfRate3600x2450: uncorrectable c0 -> LOST, else severity = total corrected errors */
    return blip25_decode_bits(dec, bits, e < 0 ? BLIP25_LOST : e, pcm);
}

/* ---- half-rate FEC */
extern const uint32_t b25_golay23_gen[12];
extern const uint32_t b25_golay23_syndrome_table[2048];

static uint32_t golay23_encode(uint16_t info) {
    uint32_t cw = 0;
    int i;
    for (i = 0; i < 12; i++)
        if ((info >> (11 - i)) & 1) cw ^= b25_golay23_gen[i];
    return cw;
}

static uint16_t golay23_syndrome(uint32_t cw) {
    uint32_t info = (cw >> 11) & 0xFFF, rec = 0;
    int i;
    for (i = 0; i < 12; i++)
        if ((info >> (11 - i)) & 1) rec ^= b25_golay23_gen[i] & 0x7FF;
    return (uint16_t)((cw & 0x7FF) ^ rec);
}

static uint16_t golay23_decode(uint32_t codeword, uint8_t *errors) {
    uint32_t cw = codeword & 0x7FFFFF;
    uint32_t pat = b25_golay23_syndrome_table[golay23_syndrome(cw)];
    *errors = (uint8_t)popcnt32(pat);
    return (uint16_t)(((cw ^ pat) >> 11) & 0xFFF);
}

static uint16_t golay24_decode(uint32_t codeword, uint8_t *errors) {
    uint32_t cw = codeword & 0xFFFFFF;
    uint8_t e23;
    uint16_t info = golay23_decode((cw >> 1) & 0x7FFFFF, &e23);
    int match = (popcnt32(golay23_encode(info)) & 1) == (cw & 1);
    *errors = (e23 == 3 && !match) ? 255 : match ? e23 : (uint8_t)(e23 + 1);
    return info;
}

int blip25_fec_codewords(const uint32_t c[4], uint8_t bits[49], uint8_t errors[4]) {
    uint8_t e[4] = {0, 0, 0, 0};
    uint16_t u[4], pr = 0;
    uint32_t mask = 0;
    int i, k, n = 0;
    static const int widths[4] = {12, 12, 11, 14};
    u[0] = golay24_decode(c[0], &e[0]);
    /* PN sequence seeded by 16 * u0; bit k of the c1 mask = MSB of p_r(1 + k), first PN at bit 22 */
    pr = (uint16_t)((u[0] & 0x0FFF) * 16u);
    for (k = 0; k < 23; k++) {
        pr = (uint16_t)((173u * pr + 13849u) & 0xFFFFu);
        mask |= (uint32_t)(pr >> 15) << (22 - k);
    }
    u[1] = golay23_decode(c[1] ^ mask, &e[1]);
    u[2] = (uint16_t)(c[2] & 0x7FF);
    u[3] = (uint16_t)(c[3] & 0x3FFF);
    for (i = 0; i < 4; i++)
        for (k = widths[i] - 1; k >= 0; k--) bits[n++] = (uint8_t)((u[i] >> k) & 1);
    if (errors) memcpy(errors, e, 4);
    return e[0] == 255 ? -1 : e[0] + e[1];
}

/* Annex S (TIA-102.BABA-A): dibit -> (codeword, bit) for its high bit and its low bit */
static const uint8_t ANNEX_S[36][4] = {
    {0, 23, 0, 5},
    {1, 10, 2, 3},
    {0, 22, 0, 4},
    {1, 9, 2, 2},
    {0, 21, 0, 3},
    {1, 8, 2, 1},
    {0, 20, 0, 2},
    {1, 7, 2, 0},
    {0, 19, 0, 1},
    {1, 6, 3, 13},
    {0, 18, 0, 0},
    {1, 5, 3, 12},
    {0, 17, 1, 22},
    {1, 4, 3, 11},
    {0, 16, 1, 21},
    {1, 3, 3, 10},
    {0, 15, 1, 20},
    {1, 2, 3, 9},
    {0, 14, 1, 19},
    {1, 1, 3, 8},
    {0, 13, 1, 18},
    {1, 0, 3, 7},
    {0, 12, 1, 17},
    {2, 10, 3, 6},
    {0, 11, 1, 16},
    {2, 9, 3, 5},
    {0, 10, 1, 15},
    {2, 8, 3, 4},
    {0, 9, 1, 14},
    {2, 7, 3, 3},
    {0, 8, 1, 13},
    {2, 6, 3, 2},
    {0, 7, 1, 12},
    {2, 5, 3, 1},
    {0, 6, 1, 11},
    {2, 4, 3, 0}
};

int blip25_fec_3600(const uint8_t frame[9], uint8_t bits[49], uint8_t errors[4]) {
    uint32_t c[4] = {0, 0, 0, 0};
    int s;
    for (s = 0; s < 36; s++) {
        uint32_t hi = (frame[(2 * s) >> 3] >> (7 - ((2 * s) & 7))) & 1;
        uint32_t lo = (frame[(2 * s + 1) >> 3] >> (7 - ((2 * s + 1) & 7))) & 1;
        c[ANNEX_S[s][0]] |= hi << ANNEX_S[s][1];
        c[ANNEX_S[s][2]] |= lo << ANNEX_S[s][3];
    }
    return blip25_fec_codewords(c, bits, errors);
}

/* ---- ThumbDV (AMBE-3000R rate 34) bit order: raw bit i is ambe_d bit RAW_TO_AMBE_D[i] */
static const uint8_t RAW_TO_AMBE_D[49] = {0, 18, 36, 1, 19, 37, 2, 20, 38, 3, 21, 39, 4, 22, 40, 5, 23, 41, 6, 24, 42, 7, 25, 43, 8, 26, 44, 9, 27, 45, 10, 28, 46, 11, 29, 47, 12, 30, 48, 13, 31, 14, 32, 15, 33, 16, 34, 17, 35};

void blip25_thumbdv_to_bits(const uint8_t frame[7], uint8_t bits[49]) {
    int i;
    for (i = 0; i < 49; i++) bits[RAW_TO_AMBE_D[i]] = (uint8_t)((frame[i >> 3] >> (7 - (i & 7))) & 1);
}

void blip25_bits_to_thumbdv(const uint8_t bits[49], uint8_t frame[7]) {
    int i;
    memset(frame, 0, 7);
    for (i = 0; i < 49; i++) frame[i >> 3] |= (uint8_t)((bits[RAW_TO_AMBE_D[i]] & 1) << (7 - (i & 7)));
}
