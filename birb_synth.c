/*
 * birb_synth.c — minimal chiptune synth engine
 * No stdlib, no malloc, no floats.
 */
#include "birb_synth.h"

/* ---------- note frequency table ---------- */
/* Phase increment per sample at BIRB_SAMPLE_RATE=44100
 * freq[n] = (note_hz / SAMPLE_RATE) * FX_ONE
 * C0 (16.35 Hz) to B7 (7902.13 Hz), 96 entries
 * Generated from: f(n) = 440 * 2^((n-57)/12), increment = f/44100 * 65536
 */
const fixed16 birb_note_freq[96] = {
    /* C0  */ 24,    26,    27,    29,    31,    32,    34,    36,    38,    41,    43,    46,
    /* C1  */ 48,    51,    54,    57,    61,    64,    68,    72,    76,    81,    86,    91,
    /* C2  */ 96,    102,   108,   115,   121,   129,   136,   144,   153,   162,   172,   182,
    /* C3  */ 193,   204,   216,   229,   243,   257,   272,   289,   306,   324,   343,   364,
    /* C4  */ 385,   408,   433,   458,   486,   515,   545,   578,   612,   649,   687,   728,
    /* C5  */ 771,   817,   866,   917,   972,  1030,  1091,  1156,  1225,  1297,  1374,  1456,
    /* C6  */ 1542,  1634,  1731,  1834,  1943,  2059,  2182,  2312,  2449,  2595,  2749,  2912,
    /* C7  */ 3084,  3268,  3462,  3668,  3886,  4118,  4363,  4624,  4899,  5191,  5498,  5825,
};

/* ---------- sine approximation ---------- */
/* Fast sine using parabolic approximation.
 * Input: phase in fixed16, 0..FX_ONE = 0..2*pi
 * Output: fixed16 in range -FX_ONE..FX_ONE */
fixed16 birb_sin_approx(fixed16 phase) {
    /* normalize to 0..FX_ONE */
    phase &= FX_MASK;
    /* map to -FX_HALF..FX_HALF centered at quarter points */
    fixed16 x;
    if (phase < FX_HALF) {
        /* first half: 0..0.5 maps to 0..1..0 */
        x = phase - (FX_ONE / 4);
    } else {
        /* second half: 0.5..1.0 maps to 0..-1..0 */
        x = (FX_ONE * 3 / 4) - phase;
    }
    /* x is now -0.25..0.25 in fixed point, scale to -1..1 */
    x <<= 2;
    /* parabolic approx: sin(x) ~ x * (3 - x*x) * 0.5  (rough, but good enough for vibrato) */
    /* simplified: 4*x*(FX_ONE - abs(x)) / FX_ONE */
    fixed16 ax = x < 0 ? -x : x;
    return FX_MUL(x, FX_ONE - ax) * 4 / FX_ONE;
}

/* ---------- waveform generation ---------- */
/* All generators take phase (0..FX_ONE) and return sample in -32767..32767 range (int16 scale) */

static int16_t gen_pulse(fixed16 phase, fixed16 duty) {
    return phase < duty ? 16383 : -16383;
}

static int16_t gen_triangle(fixed16 phase) {
    /* rising 0..0.5, falling 0.5..1.0 */
    if (phase < FX_HALF) {
        /* 0..0.5 -> -32767..32767 */
        return (int16_t)(((int32_t)phase * 4 - FX_ONE) * 32767 / FX_ONE);
    } else {
        /* 0.5..1.0 -> 32767..-32767 */
        return (int16_t)((FX_ONE * 3 - (int32_t)phase * 4) * 32767 / FX_ONE);
    }
}

static int16_t gen_sawtooth(fixed16 phase) {
    /* 0..1.0 -> -32767..32767 */
    return (int16_t)(((int32_t)phase * 2 - FX_ONE) * 32767 / FX_ONE);
}

static int16_t gen_noise(birb_channel *ch) {
    ch->u.basic.lfsr_count++;
    if (ch->u.basic.lfsr_count >= ch->u.basic.lfsr_period) {
        ch->u.basic.lfsr_count = 0;
        /* Galois LFSR, 15-bit (like NES) */
        uint16_t bit = (ch->u.basic.lfsr ^ (ch->u.basic.lfsr >> 1)) & 1;
        ch->u.basic.lfsr = (ch->u.basic.lfsr >> 1) | (bit << 14);
    }
    return (ch->u.basic.lfsr & 1) ? 16383 : -16383;
}

static int16_t gen_sine(fixed16 phase) {
    fixed16 s = birb_sin_approx(phase);
    return (int16_t)((int32_t)s * 32767 / FX_ONE);
}

static int16_t gen_basic(birb_channel *ch) {
    switch (ch->u.basic.waveform) {
        case WAVE_PULSE:    return gen_pulse(ch->phase, ch->u.basic.duty);
        case WAVE_TRIANGLE: return gen_triangle(ch->phase);
        case WAVE_SAWTOOTH: return gen_sawtooth(ch->phase);
        case WAVE_NOISE:    return gen_noise(ch);
        case WAVE_SINE:     return gen_sine(ch->phase);
        default:            return 0;
    }
}

#ifndef BIRB_NO_FM
/* 2-op FM generator. op1 modulates op0 (carrier). For 2-op, both ops share
 * the channel's master ADSR (the common FM idiom: velocity maps to mod index,
 * amplitude envelope shapes both carrier and modulator). 4-op topologies are
 * sketched in the union but not yet implemented — TODO tracked in plan. */
static int16_t gen_fm(birb_channel *ch) {
    /* 2-op path: op1 modulates op0. 4-op is stubbed as a TODO — see plan. */
    fixed16 mod_phase = ch->u.fm.op_phase[1];
    fixed16 mod_raw = birb_sin_approx(mod_phase);
    /* scale by op1 level + global mod index + op1 envelope */
    int32_t scale = (int32_t)ch->u.fm.mod_index * ch->u.fm.op_env[1] / 255;
    /* mod_raw is -FX_ONE..FX_ONE; produce a phase offset in the same units */
    fixed16 mod_out = FX_MUL(mod_raw, (fixed16)scale);

    /* feedback on op0 */
    if (ch->u.fm.feedback) {
        int32_t fb_add = ((int32_t)ch->u.fm.prev_out * ch->u.fm.feedback) >> 8;
        mod_out += fb_add;
    }

    fixed16 car_phase = ch->u.fm.op_phase[0] + mod_out;
    fixed16 car_raw = birb_sin_approx(car_phase);
    ch->u.fm.prev_out = car_raw;

    /* advance both phases */
    ch->u.fm.op_phase[0] += ch->u.fm.op_freq[0];
    ch->u.fm.op_phase[1] += ch->u.fm.op_freq[1];
    ch->u.fm.op_phase[0] &= FX_MASK;
    ch->u.fm.op_phase[1] &= FX_MASK;

    /* scale by op0 (carrier) output level */
    int32_t out = ((int32_t)car_raw * ch->u.fm.op_env[0]) >> FX_SHIFT;
    /* scale to int16 range. car_raw is -FX_ONE..FX_ONE; op_env is 0..FX_ONE;
     * product divided by FX_ONE sits in -FX_ONE..FX_ONE, then ×32767/FX_ONE. */
    out = (out * 32767) >> FX_SHIFT;
    if (out > 32767) out = 32767;
    if (out < -32767) out = -32767;
    return (int16_t)out;
}
#endif /* BIRB_NO_FM */

#ifndef BIRB_NO_KS
/* Karplus-Strong pluck. Reads the current sample from the delay line, writes
 * back a low-pass-filtered, damped copy, and advances the head. The buffer is
 * pre-filled with noise on trigger (see trigger_note); over time the filter
 * eats the high frequencies, leaving a harmonic that decays to silence at a
 * rate controlled by `damping`. */
static int16_t generate_ks(birb_channel *ch) {
    uint16_t len = ch->u.ks.buf_len;
    if (len < 2) return 0;
    uint16_t pos = ch->u.ks.buf_pos;
    uint16_t next = pos + 1; if (next >= len) next = 0;
    int16_t out = ch->u.ks.buf[pos];
    /* simple 2-tap low-pass: average of current + next tap */
    int32_t avg = ((int32_t)out + ch->u.ks.buf[next]) >> 1;
    /* damping attenuates what we write back. damping=0 → pure avg (rings);
     * damping=255 → silent write-back (instant decay). */
    int32_t damped = (avg * (255 - ch->u.ks.damping)) >> 8;
    ch->u.ks.buf[pos] = (int16_t)damped;
    ch->u.ks.buf_pos = next;
    return out;
}
#endif

#if !defined(BIRB_NO_DRUM) || !defined(BIRB_NO_FORMANT)
/* ---------- shared DSP primitives ----------
 *
 * exp_decay_lut: entry[i] = FX_ONE * 0.5^(i/8). 64 entries × 4B = 256 B.
 * Used for KICK pitch decay + future exponential ADSR stages. */
__attribute__((unused)) static const fixed16 exp_decay_lut[64] = {
    65536, 60132, 55175, 50627, 46453, 42616, 39097, 35866,
    32768, 30067, 27588, 25314, 23226, 21309, 19548, 17933,
    16384, 15033, 13794, 12657, 11613, 10654,  9774,  8966,
     8192,  7517,  6897,  6328,  5807,  5327,  4887,  4483,
     4096,  3758,  3449,  3164,  2903,  2664,  2443,  2242,
     2048,  1879,  1724,  1582,  1452,  1332,  1222,  1121,
     1024,   940,   862,   791,   726,   666,   611,   560,
      512,   470,   431,   396,   363,   333,   305,   280
};
#endif

#ifndef BIRB_NO_DRUM
/* Drums currently use simplified 2-pole resonators inline (b0 gain + pole
 * state). The full DF-II-T biquad primitive lives next to the formant code
 * and is shared via `#if !defined(BIRB_NO_DRUM) || !defined(BIRB_NO_FORMANT)`;
 * drum code does not yet use it (migration would save ~60 B; deferred). */

static inline uint16_t drum_noise(birb_channel *ch) {
    uint16_t v = ch->u.drum.noise_lfsr;
    uint16_t bit = (v ^ (v >> 1)) & 1;
    v = (v >> 1) | (bit << 14);
    if (!v) v = 0x7FFF;
    ch->u.drum.noise_lfsr = v;
    return v;
}

/* Algorithmic drums. Six flavours, four unique generators (TOM uses KICK code
 * with different trigger-time params; CRASH uses HAT code similarly).
 *
 * Channel field layout at trigger time (see trigger_note):
 *   KICK/TOM  pitch_env = current freq (decays to pitch_env_target),
 *             base_duty = per-sample decay rate, stage = click amount,
 *             stage_tick = click remaining, ttl = sample lifetime
 *   SNARE     ch->freq = body osc freq, stage = body/noise mix (0..255),
 *             pitch_env = BP gain, pitch_env_target = BP pole
 *   HAT/CRASH ch->freq = op0 freq, phase2 = op1 phase,
 *             pitch_env = mod index, pitch_env_target = HP coefficient
 *   CLAP      pitch_env / pitch_env_target = BP gain / pole,
 *             stage = current burst (0..3), stage_tick = samples left in stage */
static int16_t generate_drum(birb_channel *ch) {
    uint32_t ttl = ((uint32_t)ch->u.drum.ttl_hi << 16) | ch->u.drum.ttl_lo;
    if (ttl == 0) {
        ch->env_level = 0;
        ch->env_stage = ENV_OFF;
        return 0;
    }
    ttl--;
    ch->u.drum.ttl_hi = (uint8_t)(ttl >> 16);
    ch->u.drum.ttl_lo = (uint16_t)(ttl & 0xFFFF);

    uint8_t dt = ch->u.drum.drum_type;
    int32_t out = 0;

    if (dt == 0 || dt == 4) {
        /* KICK / TOM — sine with exponential pitch decay from pitch_env to
         * pitch_env_target. base_duty carries the per-sample decay rate. */
        fixed16 p = ch->u.drum.pitch_env;
        fixed16 t = ch->u.drum.pitch_env_target;
        int32_t gap = p - t;
        fixed16 rate = ch->base_duty;
        if (rate < 16) rate = 16;
        p -= (fixed16)(((int64_t)gap * rate) >> 20);
        ch->u.drum.pitch_env = p;
        ch->phase += p;
        ch->phase &= FX_MASK;
        fixed16 s = birb_sin_approx(ch->phase);
        out = ((int32_t)s * 24000) >> FX_SHIFT;
        /* attack click — decays over stage_tick samples */
        if (ch->u.drum.stage_tick > 0) {
            uint16_t n = drum_noise(ch);
            int32_t bit = (n & 1) ? 20000 : -20000;
            int32_t click = (bit * ch->u.drum.stage * ch->u.drum.stage_tick) >> 16;
            out += click;
            ch->u.drum.stage_tick--;
        }
    } else if (dt == 1) {
        /* SNARE — mixed noise + body sine through 2-pole resonator. */
        uint16_t n = drum_noise(ch);
        int32_t noise = (n & 1) ? 14000 : -14000;
        ch->phase += ch->freq;
        ch->phase &= FX_MASK;
        fixed16 body = birb_sin_approx(ch->phase);
        int32_t body_s = ((int32_t)body * 18000) >> FX_SHIFT;
        int32_t mix = (noise * (255 - ch->u.drum.stage) + body_s * ch->u.drum.stage) / 255;
        /* 2-pole resonant bandpass: y[n] = g*x[n] + p*y[n-1] - r*y[n-2]
         * where g = pitch_env (gain), p = pitch_env_target (resonance pole),
         * r is fixed near FX_ONE - small leak. */
        fixed16 g = ch->u.drum.pitch_env;
        fixed16 p = ch->u.drum.pitch_env_target;
        int64_t acc = ((int64_t)mix * g) >> FX_SHIFT;
        acc += ((int64_t)ch->u.drum.bq_z1[0] * p) >> FX_SHIFT;
        /* r ~= 0.88, keeps the resonator peaked but decaying. */
        acc -= ((int64_t)ch->u.drum.bq_z2[0] * (FX_ONE - (FX_ONE >> 3))) >> FX_SHIFT;
        if (acc > 32767) acc = 32767; else if (acc < -32767) acc = -32767;
        ch->u.drum.bq_z2[0] = ch->u.drum.bq_z1[0];
        ch->u.drum.bq_z1[0] = (int32_t)acc;
        out = (int32_t)acc;
    } else if (dt == 2 || dt == 5) {
        /* HAT / CRASH — 2-op FM at 1:17 ratio through one-pole highpass. */
        ch->phase += ch->freq;
        ch->phase &= FX_MASK;
        fixed16 f2 = ch->freq * 17;
        ch->u.drum.phase2 += f2;
        ch->u.drum.phase2 &= FX_MASK;
        fixed16 mod_raw = birb_sin_approx(ch->u.drum.phase2);
        int32_t mi = ch->u.drum.pitch_env;
        fixed16 mod_out = (fixed16)(((int64_t)mod_raw * mi) >> FX_SHIFT);
        fixed16 car = birb_sin_approx(ch->phase + mod_out);
        int32_t s = ((int32_t)car * 18000) >> FX_SHIFT;
        /* one-pole HP: y = x - z, z += coeff*y */
        fixed16 hp = ch->u.drum.pitch_env_target;
        int32_t y = s - ch->u.drum.bq_z1[0];
        ch->u.drum.bq_z1[0] += (int32_t)(((int64_t)y * hp) >> FX_SHIFT);
        if (dt == 5) {
            /* CRASH ~8Hz amp LFO shimmer. phase2 doubles as LFO counter. */
            fixed16 lfo = birb_sin_approx((fixed16)((uint32_t)ttl << 3));
            int32_t mul = FX_ONE + (lfo >> 1);
            y = (int32_t)(((int64_t)y * mul) >> FX_SHIFT);
        }
        out = y;
    } else {
        /* CLAP — 3 quick noise bursts + a tail, each through a bandpass.
         * stage_tick counts samples remaining in current stage (stored with
         * burst length in bq_z2[1]; tail uses a longer run). */
        uint16_t n = drum_noise(ch);
        int32_t noise = (n & 1) ? 16000 : -16000;
        fixed16 g = ch->u.drum.pitch_env;
        fixed16 p = ch->u.drum.pitch_env_target;
        int64_t acc = ((int64_t)noise * g) >> FX_SHIFT;
        acc += ((int64_t)ch->u.drum.bq_z1[0] * p) >> FX_SHIFT;
        /* r ~= 0.88, keeps the resonator peaked but decaying. */
        acc -= ((int64_t)ch->u.drum.bq_z2[0] * (FX_ONE - (FX_ONE >> 3))) >> FX_SHIFT;
        if (acc > 32767) acc = 32767; else if (acc < -32767) acc = -32767;
        ch->u.drum.bq_z2[0] = ch->u.drum.bq_z1[0];
        ch->u.drum.bq_z1[0] = (int32_t)acc;
        /* envelope: bursts during stage 0/1/2, tail during stage 3. */
        if (ch->u.drum.stage_tick == 0 && ch->u.drum.stage < 3) {
            ch->u.drum.stage++;
            /* bq_z2[1] holds the burst length in samples */
            ch->u.drum.stage_tick = (uint8_t)(ch->u.drum.bq_z2[1] & 0xFF);
            if (ch->u.drum.stage == 3) ch->u.drum.stage_tick = 0xFF;
        } else if (ch->u.drum.stage_tick) {
            ch->u.drum.stage_tick--;
        }
        /* Active during bursts (stg 0..2) full; stage 3 tail at half amp. */
        int32_t scale = (ch->u.drum.stage < 3) ? 255 : 128;
        out = (int32_t)((acc * scale) >> 8);
    }

    if (out > 32767) out = 32767; else if (out < -32767) out = -32767;
    return (int16_t)out;
}
#endif /* BIRB_NO_DRUM */

#ifndef BIRB_NO_FORMANT
/* ---------- vowel formant coefficient table ----------
 * 5 vowels × 3 formants × 3 coefficients (b0, a1, a2) = 45 fixed16 = 180 B.
 * b1 is always 0 for the bandpass form (RBJ BPF constant-skirt), b2 = -b0.
 * Coefficients were pre-computed offline at Q=8 with per-formant gain baked
 * into b0: F1=1.0, F2=0.7, F3=0.4. Frequencies from standard phonetics
 * tables (Peterson & Barney). Generate via the Python in birb2_plan notes. */
static const fixed16 formant_coeffs[5][3][3] = {
    /* A */ { {    423, -129523,  64691 }, {    439, -128255,  64281 }, {    547, -120662,  62803 } },
    /* E */ { {    308, -130085,  64921 }, {    731, -124576,  63447 }, {    555, -120371,  62761 } },
    /* I */ { {    157, -130661,  65222 }, {    901, -121719,  62962 }, {    664, -116183,  62216 } },
    /* O */ { {    331, -129981,  64875 }, {    340, -129171,  64565 }, {    540, -120877,  62835 } },
    /* U */ { {    175, -130603,  65187 }, {    352, -129069,  64531 }, {    504, -122060,  63015 } },
};

/* Interpolate vowel A and B coefficients into the channel's working set at
 * sweep position t (0..255). Runs every N samples, not per-sample.
 * Signed intermediates throughout: `one_minus_t` stays int32_t so negative
 * coefficients don't get promoted to uint32_t during multiply. */
static void formant_interp(birb_channel *ch) {
    uint8_t va = ch->u.formant.vowel_a;
    uint8_t vb = ch->u.formant.vowel_b;
    if (va > 4) va = 0;
    if (vb > 4) vb = 0;
    int32_t t = ch->u.formant.sweep_pos;          /* 0..255 */
    int32_t omt = 255 - t;
    for (int i = 0; i < 3; i++) {
        ch->u.formant.bq_b0[i] = (fixed16)((formant_coeffs[va][i][0] * omt
                                          + formant_coeffs[vb][i][0] * t) / 255);
        ch->u.formant.bq_a1[i] = (fixed16)((formant_coeffs[va][i][1] * omt
                                          + formant_coeffs[vb][i][1] * t) / 255);
        ch->u.formant.bq_a2[i] = (fixed16)((formant_coeffs[va][i][2] * omt
                                          + formant_coeffs[vb][i][2] * t) / 255);
    }
}

/* Direct Form II Transposed biquad step. b1=0 and b2=-b0 for the BPF form, so
 * the caller passes just b0 + a1 + a2. All intermediates go through int64
 * via FX_MUL; state vars are int32 and represent the filter delay line. */
static inline int32_t biquad_bp_step(int32_t x, fixed16 b0, fixed16 a1, fixed16 a2,
                                      int32_t *z1, int32_t *z2) {
    int32_t y = (int32_t)((((int64_t)b0 * x) >> FX_SHIFT) + *z1);
    *z1 = (int32_t)(0 /* b1*x */ - (((int64_t)a1 * y) >> FX_SHIFT) + *z2);
    *z2 = (int32_t)((-(int64_t)b0 * x) >> FX_SHIFT) - (int32_t)(((int64_t)a2 * y) >> FX_SHIFT);
    return y;
}

/* Source oscillator for formant: pulse / saw / noise driven by ch->phase.
 * Phase itself is advanced by the main render loop. Output is ~±16000 which,
 * after three parallel Q=8 bandpass biquads at vowel formant frequencies and
 * the default amplitude weights (F1=1, F2=0.7, F3=0.4), sums to a few thousand
 * peak — well within the 16-bit mixer headroom. */
static int16_t formant_source(birb_channel *ch) {
    fixed16 p = ch->phase;
    switch (ch->u.formant.src_wave) {
        case WAVE_PULSE:
            return p < ch->base_duty ? 16383 : -16383;
        case WAVE_NOISE: {
            uint16_t v = ch->u.formant.src_lfsr;
            uint16_t bit = (v ^ (v >> 1)) & 1;
            v = (v >> 1) | (bit << 14);
            if (!v) v = 0x7FFF;
            ch->u.formant.src_lfsr = v;
            return (v & 1) ? 16383 : -16383;
        }
        case WAVE_SAWTOOTH:
        default:
            return (int16_t)(((int32_t)p * 2 - FX_ONE) * 32767 / FX_ONE);
    }
}

/* SYNTH_FORMANT: drive a source oscillator through three parallel bandpass
 * biquads at vowel formant frequencies. Linearly interpolate between vowel A
 * and B coefficients as the sweep advances; re-interpolate every 32 samples
 * so coefficient updates aren't per-sample. */
static int16_t generate_formant(birb_channel *ch) {
    int16_t src = formant_source(ch);

    /* three parallel BPFs summed; coefficients have gain baked in. */
    int32_t y0 = biquad_bp_step(src, ch->u.formant.bq_b0[0],
                                 ch->u.formant.bq_a1[0], ch->u.formant.bq_a2[0],
                                 &ch->u.formant.bq_z1[0], &ch->u.formant.bq_z2[0]);
    int32_t y1 = biquad_bp_step(src, ch->u.formant.bq_b0[1],
                                 ch->u.formant.bq_a1[1], ch->u.formant.bq_a2[1],
                                 &ch->u.formant.bq_z1[1], &ch->u.formant.bq_z2[1]);
    int32_t y2 = biquad_bp_step(src, ch->u.formant.bq_b0[2],
                                 ch->u.formant.bq_a1[2], ch->u.formant.bq_a2[2],
                                 &ch->u.formant.bq_z1[2], &ch->u.formant.bq_z2[2]);
    int32_t out = y0 + y1 + y2;

    /* sweep position update + periodic re-interp (every 32 samples to amortize
     * the 3×3 coefficient interpolation; bounces A→B→A). */
    if (ch->u.formant.sweep_speed) {
        uint8_t r = ++ch->u.formant.recalc;
        if ((r & 0x1F) == 0) {
            uint8_t step = (ch->u.formant.sweep_speed >> 3);
            if (!step) step = 1;
            int16_t sp = (int16_t)ch->u.formant.sweep_pos + ch->u.formant.sweep_dir * step;
            if (sp >= 255) { sp = 255; ch->u.formant.sweep_dir = -1; }
            else if (sp <= 0) { sp = 0; ch->u.formant.sweep_dir = +1; }
            ch->u.formant.sweep_pos = (uint8_t)sp;
            formant_interp(ch);
        }
    }

    if (out > 32767) out = 32767;
    if (out < -32767) out = -32767;
    return (int16_t)out;
}
#endif /* BIRB_NO_FORMANT */

#ifndef BIRB_NO_SAMPLES
static int16_t gen_sample_playback(birb_channel *ch, birb_song *song) {
    if (!ch->u.sample.sample_active || ch->u.sample.sample_idx >= song->num_samples) return 0;
    birb_sample_meta *m = &song->samples[ch->u.sample.sample_idx];
    uint32_t pos = ch->u.sample.sample_pos >> FX_SHIFT;
    if (pos >= m->length) {
        if (m->loop_start != 0xFFFFFFFFu && m->loop_end > m->loop_start) {
            /* wrap back into loop region */
            uint32_t loop_len = m->loop_end - m->loop_start;
            pos = m->loop_start + ((pos - m->loop_start) % loop_len);
            ch->u.sample.sample_pos = ((uint32_t)pos << FX_SHIFT) | (ch->u.sample.sample_pos & FX_MASK);
        } else {
            ch->u.sample.sample_active = 0;
            return 0;
        }
    }
    /* linear interpolation between pos and pos+1 */
    int16_t s0 = song->sample_pool[m->offset + pos];
    uint32_t next = pos + 1;
    if (next >= m->length) {
        if (m->loop_start != 0xFFFFFFFFu && m->loop_end > m->loop_start) next = m->loop_start;
        else next = pos;
    }
    int16_t s1 = song->sample_pool[m->offset + next];
    fixed16 frac = ch->u.sample.sample_pos & FX_MASK;
    int32_t out = s0 + ((int32_t)(s1 - s0) * frac >> FX_SHIFT);
    ch->u.sample.sample_pos += ch->u.sample.sample_speed;
    return (int16_t)out;
}
#endif /* BIRB_NO_SAMPLES */

static int16_t generate_sample(birb_channel *ch, birb_song *song) {
    (void)song;
    switch (ch->synth_type) {
        case SYNTH_BASIC:  return gen_basic(ch);
#ifndef BIRB_NO_SAMPLES
        case SYNTH_SAMPLE: return gen_sample_playback(ch, song);
#endif
#ifndef BIRB_NO_FM
        case SYNTH_FM:     return gen_fm(ch);
#endif
#ifndef BIRB_NO_KS
        case SYNTH_KS:     return generate_ks(ch);
#endif
#ifndef BIRB_NO_DRUM
        case SYNTH_DRUM:   return generate_drum(ch);
#endif
#ifndef BIRB_NO_FORMANT
        case SYNTH_FORMANT: return generate_formant(ch);
#endif
        default:           return 0;
    }
}

/* ---------- envelope processing ---------- */

static void envelope_tick(birb_channel *ch) {
    switch (ch->env_stage) {
        case ENV_ATTACK:
            if (ch->adsr.attack == 0) {
                ch->env_level = FX_ONE;
                ch->env_stage = ENV_DECAY;
            } else {
                ch->env_level += FX_ONE / (ch->adsr.attack + 1);
                if (ch->env_level >= FX_ONE) {
                    ch->env_level = FX_ONE;
                    ch->env_stage = ENV_DECAY;
                }
            }
            break;

        case ENV_DECAY:
            if (ch->adsr.decay == 0) {
                ch->env_level = FX_FROM_INT(ch->adsr.sustain) / 255;
                ch->env_stage = ENV_SUSTAIN;
            } else {
                fixed16 target = FX_ONE * ch->adsr.sustain / 255;
                ch->env_level -= (FX_ONE - target) / (ch->adsr.decay + 1);
                if (ch->env_level <= target) {
                    ch->env_level = target;
                    ch->env_stage = ENV_SUSTAIN;
                }
            }
            break;

        case ENV_SUSTAIN:
            /* hold at sustain level */
            break;

        case ENV_RELEASE:
            if (ch->adsr.release == 0) {
                ch->env_level = 0;
                ch->env_stage = ENV_OFF;
            } else {
                ch->env_level -= ch->env_level / (ch->adsr.release + 1);
                if (ch->env_level < 64) { /* threshold to cut off */
                    ch->env_level = 0;
                    ch->env_stage = ENV_OFF;
                }
            }
            break;

        case ENV_OFF:
        default:
            ch->env_level = 0;
            break;
    }
}

/* ---------- note helpers ---------- */

static fixed16 note_to_freq(int note) {
    if (note < 0) note = 0;
    if (note > 95) note = 95;
    return birb_note_freq[note];
}

static void trigger_note(birb_channel *ch, uint8_t note, birb_instrument *inst, birb_song *song) {
    (void)song;
    int semitone = note - BIRB_NOTE_C0;
    ch->base_note = (uint8_t)semitone;
    ch->base_freq = note_to_freq(semitone);
    ch->freq = ch->base_freq;
    ch->phase = 0;
    ch->synth_type = inst->synth_type;
    ch->base_duty = inst->duty;
    ch->adsr = inst->envelope;
    ch->env_stage = ENV_ATTACK;
    ch->env_level = 0;
    ch->pitch_env = inst->pitch_env;
    ch->pitch_env_ticks = inst->pitch_env_len;
    ch->arp_note1 = inst->arp_note1;
    ch->arp_note2 = inst->arp_note2;
    ch->volume = inst->volume;
    ch->row_vol = 255;
    ch->arp_tick = 0;
    ch->vibrato_phase = 0;
    ch->vibrato_speed = 0;
    ch->vibrato_depth = 0;
    ch->pitch_slide = 0;
    ch->duty_sweep = 0;

    if (inst->synth_type == SYNTH_BASIC) {
        ch->u.basic.waveform = inst->waveform;
        ch->u.basic.duty = inst->duty;
        /* noise init */
        if (inst->waveform == WAVE_NOISE) {
            ch->u.basic.lfsr = 0x7FFF;
            ch->u.basic.lfsr_count = 0;
            /* higher notes = shorter period = higher pitch noise */
            ch->u.basic.lfsr_period = (uint16_t)(256 >> (semitone / 12));
            if (ch->u.basic.lfsr_period < 1) ch->u.basic.lfsr_period = 1;
        }
    }
#ifndef BIRB_NO_FM
    else if (inst->synth_type == SYNTH_FM) {
        int nops = inst->fm.num_ops ? inst->fm.num_ops : 2;
        if (nops > 4) nops = 4;
        ch->u.fm.num_ops = (uint8_t)nops;
        ch->u.fm.algorithm = inst->fm.algorithm;
        ch->u.fm.feedback = inst->fm.feedback;
        ch->u.fm.mod_index = inst->fm.mod_index ? inst->fm.mod_index : 64;
        ch->u.fm.prev_out = 0;
        for (int i = 0; i < 4; i++) {
            ch->u.fm.op_phase[i] = 0;
            /* op_freq = carrier * (ratio_i + ratio_f/16), default ratio=1 */
            uint32_t ri = inst->fm.ops[i].ratio_i;
            uint32_t rf = inst->fm.ops[i].ratio_f;
            if (i < nops && (ri | rf) == 0) { ri = 1; rf = 0; }
            ch->u.fm.op_freq[i] = (fixed16)(((uint64_t)ch->base_freq * ((ri << 4) | (rf & 0xF))) >> 4);
            ch->u.fm.op_env[i] = FX_ONE * (inst->fm.ops[i].level ? inst->fm.ops[i].level : 255) / 255;
            ch->u.fm.op_stage[i] = ENV_ATTACK;
        }
    }
#endif
#ifndef BIRB_NO_KS
    else if (inst->synth_type == SYNTH_KS) {
        /* buffer length = SAMPLE_RATE / freq_hz. ch->freq is the 16.16 phase
         * increment at SAMPLE_RATE, so len == FX_ONE / freq directly. */
        uint32_t len = ch->freq > 0 ? (uint32_t)(FX_ONE / ch->freq) : 0;
        if (len < 4) len = 4;
        if (len > BIRB_KS_BUF_SIZE) len = BIRB_KS_BUF_SIZE;
        ch->u.ks.buf_len = (uint16_t)len;
        ch->u.ks.buf_pos = 0;
        ch->u.ks.damping = inst->ks_damping;
        /* Fill buffer with a deterministic LFSR noise burst. Seed is shifted
         * from the note so identical instruments on different pitches don't
         * alias phase-locked. */
        uint16_t lfsr = (uint16_t)(0x7FFF ^ ((uint16_t)semitone * 0x1D79));
        if (!lfsr) lfsr = 0x7FFF;
        for (uint16_t i = 0; i < len; i++) {
            uint16_t bit = (lfsr ^ (lfsr >> 1)) & 1;
            lfsr = (lfsr >> 1) | (bit << 14);
            ch->u.ks.buf[i] = (lfsr & 1) ? 16383 : -16383;
        }
    }
#endif
#ifndef BIRB_NO_DRUM
    else if (inst->synth_type == SYNTH_DRUM) {
        /* Reset all drum state, then set up params per drum_type. */
        uint8_t dt = inst->drum_type;
        if (dt > 5) dt = 0;
        /* TOM reuses KICK algorithm (dt=0), CRASH reuses HAT (dt=2);
         * the difference is folded into the param init below. */
        uint8_t algo = (dt == 4) ? 0 : (dt == 5) ? 2 : dt;
        ch->u.drum.drum_type = algo;
        ch->u.drum.stage = 0;
        ch->u.drum.stage_tick = 0;
        ch->u.drum.phase2 = 0;
        ch->u.drum.bq_z1[0] = ch->u.drum.bq_z1[1] = 0;
        ch->u.drum.bq_z2[0] = ch->u.drum.bq_z2[1] = 0;
        ch->u.drum.noise_lfsr = (uint16_t)(0x7FFF ^ ((uint16_t)semitone * 0x3D7F));
        if (!ch->u.drum.noise_lfsr) ch->u.drum.noise_lfsr = 0x7FFF;

        /* Apply drum_tune as semitone offset to the note for KICK/TOM body,
         * and as a base freq for SNARE/HAT. */
        int dn = semitone + (int)inst->drum_tune;
        if (dn < 0) dn = 0; if (dn > 95) dn = 95;
        fixed16 dfreq = note_to_freq(dn);

        /* Default lifetime in samples. Scaled by decay param. */
        uint32_t ttl;
        if (algo == 0) {
            /* KICK / TOM */
            /* Start freq derived from drum_tune note; end freq ~1/8 of that. */
            fixed16 start_f = (dt == 4) ? dfreq : dfreq;   /* TOM defaults identical */
            ch->u.drum.pitch_env = start_f << 1;            /* bump start up a bit */
            ch->u.drum.pitch_env_target = start_f >> 3;     /* settle low */
            /* Decay rate: higher `tone` = faster pitch decay. */
            ch->base_duty = (fixed16)((uint32_t)inst->drum_tone * 256);
            if (ch->base_duty < 16) ch->base_duty = 16;
            /* click mix */
            ch->u.drum.stage = inst->drum_snap;
            ch->u.drum.stage_tick = 64;
            /* lifetime: decay param maps ~8..200ms at 44.1kHz */
            ttl = (uint32_t)inst->drum_decay * 200 + 1024;
            if (dt == 4) ttl = ttl * 2; /* TOM holds longer */
        } else if (algo == 1) {
            /* SNARE */
            ch->freq = dfreq > 0 ? dfreq : birb_note_freq[26]; /* D3 fallback */
            /* body/noise mix in stage, 0..255 */
            ch->u.drum.stage = inst->drum_snap;
            /* pitch_env = BP gain, pitch_env_target = pole position (cos(ω) × r) */
            ch->u.drum.pitch_env = FX_ONE >> 2; /* g = 0.25 */
            /* Pole at ~ tone/256 of Nyquist. */
            fixed16 tone = (fixed16)((uint32_t)inst->drum_tone * (FX_ONE * 15 / 16) / 255);
            ch->u.drum.pitch_env_target = (FX_ONE - (FX_ONE >> 5)) - tone / 2;
            /* positive resonance pole */
            if (ch->u.drum.pitch_env_target < 0) ch->u.drum.pitch_env_target = 0;
            if (ch->u.drum.pitch_env_target > FX_ONE) ch->u.drum.pitch_env_target = FX_ONE - 1;
            ttl = (uint32_t)inst->drum_decay * 120 + 1024;
        } else if (algo == 2) {
            /* HAT / CRASH — op0 freq, op1 = op0*17 for irrational ratio. */
            ch->freq = dfreq > 0 ? dfreq : birb_note_freq[62]; /* D6ish */
            /* mod index from tone, HP coeff from snap */
            ch->u.drum.pitch_env = (fixed16)((uint32_t)inst->drum_tone * (FX_ONE * 4) / 255);
            fixed16 hp = (fixed16)((uint32_t)inst->drum_snap * (FX_ONE * 15 / 16) / 255);
            if (hp < FX_ONE / 16) hp = FX_ONE / 16;
            ch->u.drum.pitch_env_target = hp;
            ttl = (uint32_t)inst->drum_decay * 180 + 1024;
            if (dt == 5) ttl = 90000 + (uint32_t)inst->drum_decay * 400; /* CRASH ~2s+ */
        } else {
            /* CLAP */
            ch->u.drum.pitch_env = FX_ONE >> 2;
            fixed16 tone = (fixed16)((uint32_t)inst->drum_tone * (FX_ONE * 15 / 16) / 255);
            ch->u.drum.pitch_env_target = (FX_ONE - (FX_ONE >> 5)) - tone / 2;
            if (ch->u.drum.pitch_env_target < 0) ch->u.drum.pitch_env_target = 0;
            if (ch->u.drum.pitch_env_target > FX_ONE) ch->u.drum.pitch_env_target = FX_ONE - 1;
            /* burst length stored in bq_z2[1]; snap controls burst spacing. */
            ch->u.drum.bq_z2[1] = 20 + (inst->drum_snap >> 3);
            ch->u.drum.stage = 0;
            ch->u.drum.stage_tick = (uint8_t)ch->u.drum.bq_z2[1];
            ttl = (uint32_t)inst->drum_decay * 160 + 2048;
        }
        /* Clamp ttl to 24-bit. */
        if (ttl > 0xFFFFFFu) ttl = 0xFFFFFFu;
        ch->u.drum.ttl_hi = (uint8_t)(ttl >> 16);
        ch->u.drum.ttl_lo = (uint16_t)(ttl & 0xFFFF);
        /* Force a fast-decay envelope so the drum body fades over its ttl.
         * Release is scaled from drum_decay so drum sustains match ttl roughly.
         * adsr.attack=0 → instant on, decay=0 (goes to 0 sustain), release fires
         * only after note-off which won't come — so we just start in ENV_RELEASE
         * with level=FX_ONE and a decay rate proportional to drum_decay. */
        ch->env_level = FX_ONE;
        ch->env_stage = ENV_RELEASE;
        /* Release time in ticks ≈ drum_decay/8: 0 → quick, 255 → ~32 ticks. */
        ch->adsr.release = (uint8_t)(inst->drum_decay >> 3);
        if (ch->adsr.release < 1) ch->adsr.release = 1;
    }
#endif
#ifndef BIRB_NO_FORMANT
    else if (inst->synth_type == SYNTH_FORMANT) {
        /* Reset biquad state and sweep; initial vowel comes straight from
         * vowel_a (sweep_pos = 0). Source wave defaults to sawtooth — that's
         * the voice-like option when the instrument left it zero. */
        for (int i = 0; i < 3; i++) {
            ch->u.formant.bq_z1[i] = 0;
            ch->u.formant.bq_z2[i] = 0;
        }
        uint8_t sw = inst->formant_source_wave;
        if (sw != WAVE_PULSE && sw != WAVE_SAWTOOTH && sw != WAVE_NOISE)
            sw = WAVE_SAWTOOTH;
        ch->u.formant.src_wave = sw;
        ch->u.formant.src_lfsr = (uint16_t)(0x7FFF ^ ((uint16_t)semitone * 0x2BCD));
        if (!ch->u.formant.src_lfsr) ch->u.formant.src_lfsr = 0x7FFF;
        ch->u.formant.vowel_a = inst->formant_vowel_a > 4 ? 0 : inst->formant_vowel_a;
        ch->u.formant.vowel_b = inst->formant_vowel_b > 4 ? 0 : inst->formant_vowel_b;
        ch->u.formant.sweep_speed = inst->formant_sweep_speed;
        ch->u.formant.sweep_pos = 0;
        ch->u.formant.sweep_dir = +1;
        ch->u.formant.recalc = 0;
        /* decode pulse duty code (0..3) into fixed16 duty reused by source. */
        switch (inst->formant_duty) {
            case 0: ch->base_duty = DUTY_12; break;
            case 1: ch->base_duty = DUTY_25; break;
            case 3: ch->base_duty = DUTY_75; break;
            default: ch->base_duty = DUTY_50; break;
        }
        formant_interp(ch);
    }
#endif
#ifndef BIRB_NO_SAMPLES
    else if (inst->synth_type == SYNTH_SAMPLE) {
        ch->u.sample.sample_active = 0;
        if (song && inst->sample_idx < song->num_samples) {
            birb_sample_meta *m = &song->samples[inst->sample_idx];
            ch->u.sample.sample_idx = inst->sample_idx;
            ch->u.sample.sample_pos = 0;
            /* Playback speed = note_freq / base_note_freq, in 16.16 */
            int base = m->base_note;
            if (base > 95) base = 95;
            fixed16 note_f = note_to_freq(semitone);
            fixed16 base_f = note_to_freq(base);
            if (base_f > 0) {
                ch->u.sample.sample_speed = (uint32_t)(((uint64_t)note_f << FX_SHIFT) / base_f);
            } else {
                ch->u.sample.sample_speed = FX_ONE;
            }
            ch->u.sample.sample_active = 1;
        }
    }
#endif
}

static void release_note(birb_channel *ch) {
    if (ch->env_stage != ENV_OFF) {
        ch->env_stage = ENV_RELEASE;
    }
}

/* ---------- effect processing ---------- */

static void process_effects(birb_channel *ch, uint8_t effect, uint8_t param, birb_state *state) {
    switch (effect) {
        case FX_ARPEGGIO:
            ch->arp_note1 = (param >> 4) & 0x0F;
            ch->arp_note2 = param & 0x0F;
            ch->arp_tick = 0;
            break;
        case FX_PITCH_UP:
            ch->pitch_slide = (fixed16)param << 2;
            break;
        case FX_PITCH_DOWN:
            ch->pitch_slide = -((fixed16)param << 2);
            break;
        case FX_VIBRATO:
            ch->vibrato_speed = FX_ONE / 64 * ((param >> 4) & 0x0F);
            ch->vibrato_depth = (fixed16)(param & 0x0F) << 4;
            break;
        case FX_TONE_PORTA:
            ch->porta_speed = (fixed16)param << 2;
            break;
        case FX_RETRIGGER:
            ch->retrig_interval = param;
            break;
        case FX_EXTENDED: {
            int sub = (param >> 4) & 0x0F;
            int val = param & 0x0F;
            if (sub == 0xC) ch->note_cut_tick = val;
            else if (sub == 0xD) ch->note_delay_tick = val;
            break;
        }
        case FX_TREMOLO:
            ch->tremolo_speed = FX_ONE / 64 * ((param >> 4) & 0x0F);
            ch->tremolo_depth = (fixed16)(param & 0x0F) << 4;
            break;
        case FX_SAMPLE_OFFSET:
#ifndef BIRB_NO_SAMPLES
            if (ch->synth_type == SYNTH_SAMPLE) {
                ch->u.sample.sample_pos = ((uint32_t)param << 8) << FX_SHIFT;
            }
#endif
            break;
        case FX_POS_JUMP:
            state->jump_order = param;
            state->jump_row = 0;
            break;
        case FX_PAT_BREAK:
            if (state->jump_order < 0) state->jump_order = state->order_pos + 1;
            state->jump_row = param;
            break;
        case FX_SET_SPEED:
            if (param == 0) break;
            if (param < 0x20) {
                state->song->ticks_per_row = param;
            } else {
                state->song->bpm = param;
                state->samples_per_tick = BIRB_SAMPLE_RATE * 5 / (param * 2);
            }
            break;
        default:
            break;
    }
}

static void tick_effects(birb_channel *ch) {
    /* pitch envelope */
    if (ch->pitch_env_ticks > 0) {
        ch->base_freq += (fixed16)ch->pitch_env << 2;
        if (ch->base_freq < 1) ch->base_freq = 1;
        ch->pitch_env_ticks--;
    }

    /* pitch slide */
    if (ch->pitch_slide != 0) {
        ch->base_freq += ch->pitch_slide;
        if (ch->base_freq < 1) ch->base_freq = 1;
    }

    /* arpeggio */
    if (ch->arp_note1 != 0 || ch->arp_note2 != 0) {
        int note = ch->base_note;
        switch (ch->arp_tick % 3) {
            case 0: break; /* base note */
            case 1: note += ch->arp_note1; break;
            case 2: note += ch->arp_note2; break;
        }
        ch->freq = note_to_freq(note);
        ch->arp_tick++;
    } else {
        ch->freq = ch->base_freq;
    }

    /* vibrato */
    if (ch->vibrato_depth > 0) {
        fixed16 vib = birb_sin_approx(ch->vibrato_phase);
        ch->freq += FX_MUL(vib, ch->vibrato_depth);
        ch->vibrato_phase += ch->vibrato_speed;
    }

    /* tremolo — cache modulation amount for mixer */
    if (ch->tremolo_depth > 0) {
        fixed16 tr = birb_sin_approx(ch->tremolo_phase);
        ch->tremolo_mod = FX_MUL(tr, ch->tremolo_depth);
        ch->tremolo_phase += ch->tremolo_speed;
    } else {
        ch->tremolo_mod = 0;
    }

    /* tone portamento — slide toward target */
    if (ch->porta_target > 0 && ch->porta_speed > 0) {
        if (ch->base_freq < ch->porta_target) {
            ch->base_freq += ch->porta_speed;
            if (ch->base_freq > ch->porta_target) ch->base_freq = ch->porta_target;
        } else if (ch->base_freq > ch->porta_target) {
            ch->base_freq -= ch->porta_speed;
            if (ch->base_freq < ch->porta_target) ch->base_freq = ch->porta_target;
        }
        if (!(ch->arp_note1 || ch->arp_note2)) ch->freq = ch->base_freq;
    }

    /* envelope */
    envelope_tick(ch);

#ifndef BIRB_NO_FM
    /* FM: derive per-operator frequencies from the current carrier freq.
     * The op_freq array is rebuilt each tick so arpeggio/vibrato/porta/slide
     * all affect the FM note pitch without extra plumbing. Per-op ratio is
     * stored as (ratio_i<<4 | ratio_f) fixed-point 4.4 in op_env upper bits?
     * No — we keep the ratio on the instrument and re-read via channel state:
     * op_freq already holds the initial ratio-weighted freq at note-on. Here
     * we scale proportionally to any freq change since. */
    if (ch->synth_type == SYNTH_FM && ch->base_freq > 0) {
        /* Simple approach: recompute op_freq[i] = ch->freq * (op_freq_init[i] / base_freq_init).
         * We don't store the initial ratios separately; instead, divide current
         * op_freq by its old carrier freq and re-multiply. To keep this cheap
         * and avoid cumulative drift, we cheat: op_freq[0] tracks ch->freq
         * directly (carrier), and op_freq[i] is scaled relative to op_freq[0]. */
        fixed16 old_carrier = ch->u.fm.op_freq[0];
        if (old_carrier > 0) {
            for (int i = 1; i < 4; i++) {
                if (ch->u.fm.op_freq[i]) {
                    /* ratio scaled to new carrier */
                    ch->u.fm.op_freq[i] = (fixed16)(((int64_t)ch->u.fm.op_freq[i] * ch->freq) / old_carrier);
                }
            }
        }
        ch->u.fm.op_freq[0] = ch->freq;
    }
#endif
}

/* ---------- sequencer ---------- */

static void process_row(birb_state *state) {
    birb_song *song = state->song;
    int order_pos = state->order_pos;
    int row = state->current_row;

    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
        int pat_idx = song->order[order_pos][c];
        if (pat_idx >= song->num_patterns) continue;

        birb_row *r = &song->patterns[pat_idx][row][c];

        birb_channel *ch = &state->channels[c];

        /* reset per-row effect state */
        ch->retrig_interval = 0;
        ch->note_cut_tick = 0;
        ch->note_delay_tick = 0;

        /* parse effect first to check for tone porta or note delay */
        int is_tone_porta = (r->effect == FX_TONE_PORTA);
        int is_note_delay = (r->effect == FX_EXTENDED && ((r->param >> 4) & 0xF) == 0xD);

        /* note */
        if (is_note_delay && r->note >= BIRB_NOTE_C0) {
            /* delay: store note, trigger later in tick_effects */
            ch->delayed_note = r->note;
            ch->delayed_inst = (r->instrument != 0xFF) ? r->instrument : ch->cur_instrument;
            ch->note_delay_tick = r->param & 0x0F;
        } else if (r->note == BIRB_NOTE_OFF) {
            release_note(ch);
        } else if (r->note >= BIRB_NOTE_C0) {
            if (is_tone_porta) {
                /* tone porta: set target, don't retrigger */
                int semi = r->note - BIRB_NOTE_C0;
                ch->porta_target = note_to_freq(semi);
            } else {
                uint8_t inst_idx = r->instrument;
                if (inst_idx == 0xFF) inst_idx = ch->cur_instrument;
                if (inst_idx < song->num_instruments) {
                    ch->cur_instrument = inst_idx;
                    trigger_note(ch, r->note, &song->instruments[inst_idx], song);
                }
            }
        }

        /* row volume (0 = no change, 1-255 = level) */
        if (r->volume) {
            ch->row_vol = r->volume;
        }

        /* effect */
        if (r->effect != FX_NONE) {
            process_effects(ch, r->effect, r->param, state);
        }
    }
}

static void advance_tick(birb_state *state) {
    birb_song *song = state->song;
    state->current_tick++;

    if (state->current_tick >= song->ticks_per_row) {
        state->current_tick = 0;

        /* honor pending jump (from Bxx / Dxx) */
        if (state->jump_order >= 0) {
            state->order_pos = state->jump_order;
            if (state->order_pos >= song->order_length) state->order_pos = 0;
            state->current_row = state->jump_row;
            state->jump_order = -1;
            state->jump_row = 0;
        } else {
            state->current_row++;
            int pat_len = song->pattern_lengths[song->order[state->order_pos][0]];
            if (pat_len == 0) pat_len = BIRB_MAX_ROWS;
            if (state->current_row >= pat_len) {
                state->current_row = 0;
                state->order_pos++;
                if (state->order_pos >= song->order_length) {
                    state->order_pos = 0; /* loop */
                }
            }
        }

        process_row(state);
        state->row_out = state->current_row;
        state->pattern_out = state->order_pos;
    }

    /* tick effects on every tick (not just row boundaries) */
    int tick = state->current_tick;
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
        birb_channel *ch = &state->channels[c];

        /* note delay — trigger note after N ticks */
        if (ch->note_delay_tick > 0 && tick == ch->note_delay_tick) {
            if (ch->delayed_inst < state->song->num_instruments) {
                ch->cur_instrument = ch->delayed_inst;
                trigger_note(ch, ch->delayed_note, &state->song->instruments[ch->delayed_inst], state->song);
            }
            ch->note_delay_tick = 0;
        }

        /* note cut — kill after N ticks */
        if (ch->note_cut_tick > 0 && tick == ch->note_cut_tick) {
            ch->env_level = 0;
            ch->env_stage = ENV_OFF;
        }

        /* retrigger — re-trigger note every N ticks */
        if (ch->retrig_interval > 0 && tick > 0 && (tick % ch->retrig_interval) == 0) {
            ch->phase = 0;
            ch->env_stage = ENV_ATTACK;
            ch->env_level = 0;
            if (ch->synth_type == SYNTH_BASIC && ch->u.basic.waveform == WAVE_NOISE) {
                ch->u.basic.lfsr = 0x7FFF;
                ch->u.basic.lfsr_count = 0;
            }
        }

        tick_effects(ch);
    }
}

/* ---------- public API ---------- */

void birb_init(birb_state *state, birb_song *song) {
    /* zero everything */
    for (int i = 0; i < (int)sizeof(birb_state); i++) {
        ((uint8_t *)state)[i] = 0;
    }
    state->song = song;

    /* compute samples per tick from BPM and ticks_per_row
     * ticks per second = (BPM * ticks_per_row) / 60
     * but we want classic tracker feel: BPM means "rows per minute" / 4
     * standard: ticks/sec = BPM * 2 / 5 (at 6 ticks/row)
     * more precisely: samples_per_tick = sample_rate * 60 / (BPM * ticks_per_row * 4)
     */
    int tpr = song->ticks_per_row;
    if (tpr == 0) tpr = 6;
    int bpm = song->bpm;
    if (bpm == 0) bpm = 125;

    /* samples_per_tick = sample_rate * 5 / (bpm * 2) for classic tracker timing */
    state->samples_per_tick = BIRB_SAMPLE_RATE * 5 / (bpm * 2);
    state->tick_counter = 0;
    state->order_pos = 0;
    state->current_row = 0;
    state->current_tick = 0;
    state->jump_order = -1;
    state->jump_row = 0;

    /* init noise LFSRs (basic-synth union arm; safe to touch because the
     * channel default synth_type is SYNTH_BASIC=0 via the zero-init above) */
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
        state->channels[c].u.basic.lfsr = 0x7FFF;
        state->channels[c].u.basic.lfsr_period = 16;
    }

    /* process row 0 immediately so first notes trigger */
    process_row(state);
}

void birb_render(birb_state *state, int16_t *output, int num_samples) {
    for (int i = 0; i < num_samples; i++) {
        /* advance sequencer */
        if (state->tick_counter <= 0) {
            advance_tick(state);
            state->tick_counter = state->samples_per_tick;
        }
        state->tick_counter--;

        /* mix all channels */
        int32_t mix = 0;
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
            birb_channel *ch = &state->channels[c];
            if (ch->env_stage == ENV_OFF && ch->env_level == 0) continue;

            int16_t sample = generate_sample(ch, state->song);
            /* apply envelope, instrument volume, row volume, and tremolo */
            int32_t vol = ch->volume ? ch->volume : 255;
            int32_t rvol = ch->row_vol;
            fixed16 env = ch->env_level;
            if (ch->tremolo_mod) {
                env += FX_MUL(env, ch->tremolo_mod);
                if (env < 0) env = 0;
                if (env > FX_ONE) env = FX_ONE;
            }
            int32_t out = ((int32_t)sample * FX_TO_INT(env * 256)) >> 8;
            out = out * vol / 255;
            out = out * rvol / 255;
            mix += out;

            /* advance phase */
            ch->phase += ch->freq;
            if (ch->phase >= FX_ONE) {
                ch->phase -= FX_ONE;
            }
        }

        /* clamp */
        if (mix > 32767) mix = 32767;
        if (mix < -32767) mix = -32767;
        output[i] = (int16_t)mix;
    }
}

int birb_get_row(birb_state *state) {
    return state->row_out;
}

int birb_get_pattern(birb_state *state) {
    return state->pattern_out;
}
