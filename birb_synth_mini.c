/*
 * birb_synth_mini.c — size-optimized synth for 4K web demos
 * Same core as birb_synth.c but stripped for minimal WASM output.
 * No stored tables, fewer features, tighter code.
 */
#include "birb_synth.h"

/* ---------- sine approximation for vibrato ---------- */
fixed16 birb_sin_approx(fixed16 phase) {
    /* Parabolic sine ≈ FX_ONE amplitude, peak at FX_ONE/4. */
    phase &= FX_MASK;
    int negate = (phase >= FX_HALF);
    fixed16 t = negate ? (phase - FX_HALF) : phase;
    t <<= 1;
    if (t > FX_ONE) t = FX_ONE;
    fixed16 y = FX_MUL(t, FX_ONE - t) << 2;
    return negate ? -y : y;
}

/* ---------- note frequency computation ---------- */
/* Instead of a 384-byte table, compute on the fly.
 * freq = 440 * 2^((note-57)/12) / SAMPLE_RATE * 65536
 *
 * We use a small 12-entry octave-0 table (48 bytes) and shift for octaves.
 * C0..B0 base frequencies as phase increments at 44100Hz. */
static const fixed16 base_freq[12] = {
    24, 26, 27, 29, 31, 32, 34, 36, 38, 41, 43, 46
};

static fixed16 note_to_freq(int note) {
    if (note < 0) note = 0;
    if (note > 95) note = 95;
    return base_freq[note % 12] << (note / 12);
}

/* ---------- waveform generation ---------- */

static int16_t gen_pulse(fixed16 phase, fixed16 duty) {
    return phase < duty ? 16383 : -16383;
}

static int16_t gen_triangle(fixed16 phase) {
    if (phase < FX_HALF)
        return (int16_t)(((int32_t)phase * 4 - FX_ONE) * 32767 / FX_ONE);
    else
        return (int16_t)((FX_ONE * 3 - (int32_t)phase * 4) * 32767 / FX_ONE);
}

static int16_t gen_sawtooth(fixed16 phase) {
    return (int16_t)(((int32_t)phase * 2 - FX_ONE) * 32767 / FX_ONE);
}

static int16_t gen_noise(birb_channel *ch) {
    ch->u.basic.lfsr_count++;
    if (ch->u.basic.lfsr_count >= ch->u.basic.lfsr_period) {
        ch->u.basic.lfsr_count = 0;
        uint16_t bit = (ch->u.basic.lfsr ^ (ch->u.basic.lfsr >> 1)) & 1;
        ch->u.basic.lfsr = (ch->u.basic.lfsr >> 1) | (bit << 14);
    }
    return (ch->u.basic.lfsr & 1) ? 16383 : -16383;
}

static int16_t gen_basic(birb_channel *ch) {
    switch (ch->u.basic.waveform) {
        case WAVE_PULSE:    return gen_pulse(ch->phase, ch->u.basic.duty);
        case WAVE_TRIANGLE: return gen_triangle(ch->phase);
        case WAVE_SAWTOOTH: return gen_sawtooth(ch->phase);
        case WAVE_NOISE:    return gen_noise(ch);
        default:            return gen_triangle(ch->phase); /* sine→tri fallback */
    }
}

#ifndef BIRB_NO_FM
/* 4-op FM with 8 algorithms — mirrors fmRender4() in editor.html. Same maths
 * as birb_synth.c gen_fm; for size we keep this version structurally identical
 * (no 2-op fast path elision — JS uses both, this stays in sync). */
static int16_t gen_fm(birb_channel *ch) {
    fixed16 lvl[4];
    for (int i = 0; i < 4; i++)
        lvl[i] = (fixed16)(((int64_t)ch->u.fm.op_lvl[i] * ch->u.fm.op_env[i]) >> FX_SHIFT);
    int32_t mi = ch->u.fm.mod_index;
    fixed16 fb = ch->u.fm.feedback
        ? (fixed16)((int32_t)ch->u.fm.prev_out * ch->u.fm.feedback / 256)
        : 0;
    fixed16 raw, s;
    int nops = ch->u.fm.num_ops < 4 ? 2 : 4;
    if (nops == 4) {
        fixed16 ph0 = ch->u.fm.op_phase[0], ph1 = ch->u.fm.op_phase[1];
        fixed16 ph2 = ch->u.fm.op_phase[2], ph3 = ch->u.fm.op_phase[3];
        switch (ch->u.fm.algorithm & 7) {
            case 0: {
                fixed16 s3 = (fixed16)(((int64_t)birb_sin_approx(ph3 + fb) * lvl[3]) >> FX_SHIFT);
                fixed16 s2 = (fixed16)(((int64_t)birb_sin_approx(ph2 + s3) * lvl[2]) >> FX_SHIFT);
                fixed16 s1 = (fixed16)(((int64_t)birb_sin_approx(ph1 + s2) * lvl[1]) >> FX_SHIFT);
                fixed16 s1m = (fixed16)(((int64_t)s1 * mi) / 255);
                raw = birb_sin_approx(ph0 + s1m);
                s = (fixed16)(((int64_t)raw * lvl[0]) >> FX_SHIFT);
                break;
            }
            case 1: {
                fixed16 s3 = (fixed16)(((int64_t)birb_sin_approx(ph3 + fb) * lvl[3]) >> FX_SHIFT);
                fixed16 s2 = (fixed16)(((int64_t)birb_sin_approx(ph2)      * lvl[2]) >> FX_SHIFT);
                fixed16 s1 = (fixed16)(((int64_t)birb_sin_approx(ph1 + s3 + s2) * lvl[1]) >> FX_SHIFT);
                fixed16 s1m = (fixed16)(((int64_t)s1 * mi) / 255);
                raw = birb_sin_approx(ph0 + s1m);
                s = (fixed16)(((int64_t)raw * lvl[0]) >> FX_SHIFT);
                break;
            }
            case 2: {
                fixed16 s3 = (fixed16)(((int64_t)birb_sin_approx(ph3 + fb) * lvl[3]) >> FX_SHIFT);
                fixed16 s2 = (fixed16)(((int64_t)birb_sin_approx(ph2 + s3) * lvl[2]) >> FX_SHIFT);
                fixed16 s1 = (fixed16)(((int64_t)birb_sin_approx(ph1)      * lvl[1]) >> FX_SHIFT);
                fixed16 mod = (fixed16)((((int64_t)s2 + s1) * mi) / 255);
                raw = birb_sin_approx(ph0 + mod);
                s = (fixed16)(((int64_t)raw * lvl[0]) >> FX_SHIFT);
                break;
            }
            case 3: {
                fixed16 s3 = (fixed16)(((int64_t)birb_sin_approx(ph3 + fb) * lvl[3]) >> FX_SHIFT);
                fixed16 s1 = (fixed16)(((int64_t)birb_sin_approx(ph1 + s3) * lvl[1]) >> FX_SHIFT);
                fixed16 s2 = (fixed16)(((int64_t)birb_sin_approx(ph2)      * lvl[2]) >> FX_SHIFT);
                fixed16 mod = (fixed16)((((int64_t)s1 + s2) * mi) / 255);
                raw = birb_sin_approx(ph0 + mod);
                s = (fixed16)(((int64_t)raw * lvl[0]) >> FX_SHIFT);
                break;
            }
            case 4: {
                fixed16 s3 = (fixed16)(((int64_t)birb_sin_approx(ph3 + fb) * lvl[3]) >> FX_SHIFT);
                fixed16 s2 = (fixed16)(((int64_t)birb_sin_approx(ph2)      * lvl[2]) >> FX_SHIFT);
                fixed16 s1 = (fixed16)(((int64_t)birb_sin_approx(ph1)      * lvl[1]) >> FX_SHIFT);
                fixed16 mod = (fixed16)((((int64_t)s3 + s2 + s1) * mi) / 255);
                raw = birb_sin_approx(ph0 + mod);
                s = (fixed16)(((int64_t)raw * lvl[0]) >> FX_SHIFT);
                break;
            }
            case 5: {
                fixed16 s3 = (fixed16)(((int64_t)birb_sin_approx(ph3 + fb) * lvl[3]) >> FX_SHIFT);
                fixed16 s1 = (fixed16)(((int64_t)birb_sin_approx(ph1)      * lvl[1]) >> FX_SHIFT);
                fixed16 s3m = (fixed16)(((int64_t)s3 * mi) / 255);
                fixed16 s1m = (fixed16)(((int64_t)s1 * mi) / 255);
                fixed16 r2 = birb_sin_approx(ph2 + s3m);
                raw = birb_sin_approx(ph0 + s1m);
                int64_t sum = (int64_t)r2 * lvl[2] + (int64_t)raw * lvl[0];
                s = (fixed16)((sum >> FX_SHIFT) >> 1);
                break;
            }
            case 6: {
                fixed16 s3 = (fixed16)(((int64_t)birb_sin_approx(ph3 + fb) * lvl[3]) >> FX_SHIFT);
                fixed16 s3m = (fixed16)(((int64_t)s3 * mi) / 255);
                fixed16 r2 = birb_sin_approx(ph2 + s3m);
                fixed16 r1 = birb_sin_approx(ph1);
                raw = birb_sin_approx(ph0);
                int64_t sum = (int64_t)r2 * lvl[2] + (int64_t)r1 * lvl[1] + (int64_t)raw * lvl[0];
                s = (fixed16)((sum >> FX_SHIFT) / 3);
                break;
            }
            case 7: default: {
                fixed16 r3 = birb_sin_approx(ph3 + fb);
                fixed16 r2 = birb_sin_approx(ph2);
                fixed16 r1 = birb_sin_approx(ph1);
                raw = birb_sin_approx(ph0);
                int64_t sum = (int64_t)r3 * lvl[3] + (int64_t)r2 * lvl[2]
                            + (int64_t)r1 * lvl[1] + (int64_t)raw * lvl[0];
                s = (fixed16)((sum >> FX_SHIFT) >> 2);
                break;
            }
        }
    } else {
        fixed16 mod_raw = birb_sin_approx(ch->u.fm.op_phase[1]);
        int64_t mod = ((int64_t)mod_raw * mi) / 255;
        mod = (mod * lvl[1]) >> FX_SHIFT;
        if (ch->u.fm.feedback) mod += fb;
        raw = birb_sin_approx(ch->u.fm.op_phase[0] + (fixed16)mod);
        s = (fixed16)(((int64_t)raw * lvl[0]) >> FX_SHIFT);
    }
    ch->u.fm.prev_out = raw;
    for (int i = 0; i < 4; i++)
        ch->u.fm.op_phase[i] = (ch->u.fm.op_phase[i] + ch->u.fm.op_freq[i]) & FX_MASK;
    int32_t out = ((int32_t)s * 32767) >> FX_SHIFT;
    if (out > 32767) out = 32767;
    if (out < -32767) out = -32767;
    return (int16_t)out;
}
#endif

#ifndef BIRB_NO_KS
static int16_t generate_ks(birb_channel *ch) {
    uint16_t len = ch->u.ks.buf_len;
    if (len < 2) return 0;
    uint16_t pos = ch->u.ks.buf_pos;
    uint16_t next = pos + 1; if (next >= len) next = 0;
    int16_t out = ch->u.ks.buf[pos];
    int32_t avg = ((int32_t)out + ch->u.ks.buf[next]) >> 1;
    int32_t damped = (avg * (255 - ch->u.ks.damping)) >> 8;
    ch->u.ks.buf[pos] = (int16_t)damped;
    ch->u.ks.buf_pos = next;
    return out;
}
#endif

#ifndef BIRB_NO_DRUM
static inline uint16_t drum_noise_m(birb_channel *ch) {
    uint16_t v = ch->u.drum.noise_lfsr;
    uint16_t bit = (v ^ (v >> 1)) & 1;
    v = (v >> 1) | (bit << 14);
    if (!v) v = 0x7FFF;
    ch->u.drum.noise_lfsr = v;
    return v;
}

/* Size-optimized drum generator for 4K. CLAP is one-stage (plain BP noise),
 * CRASH drops the shimmer LFO. Everything else matches birb_synth.c. */
static int16_t generate_drum(birb_channel *ch) {
    uint32_t ttl = ((uint32_t)ch->u.drum.ttl_hi << 16) | ch->u.drum.ttl_lo;
    if (ttl == 0) { ch->env_level = 0; ch->env_stage = ENV_OFF; return 0; }
    ttl--;
    ch->u.drum.ttl_hi = (uint8_t)(ttl >> 16);
    ch->u.drum.ttl_lo = (uint16_t)(ttl & 0xFFFF);
    uint8_t dt = ch->u.drum.drum_type;
    int32_t out = 0;
    if (dt == 0) {
        /* KICK: triangle body + pitch sweep + click */
        fixed16 p = ch->u.drum.pitch_env, t = ch->u.drum.pitch_env_target;
        int32_t gap = p - t;
        fixed16 rate = ch->base_duty; if (rate < 16) rate = 16;
        p -= (fixed16)(((int64_t)gap * rate) >> 20);
        ch->u.drum.pitch_env = p;
        ch->phase = (ch->phase + p) & FX_MASK;
        int32_t tri = (ch->phase < FX_HALF)
            ? ((int32_t)ch->phase * 4 - FX_ONE)
            : (FX_ONE * 3 - (int32_t)ch->phase * 4);
        out = ((int32_t)tri * 28000) >> FX_SHIFT;
        if (ch->u.drum.stage_tick) {
            uint16_t n = drum_noise_m(ch);
            int32_t peak = (int32_t)ch->u.drum.stage * 128;
            int32_t amp = peak * ch->u.drum.stage_tick / 384;
            out += (n & 1) ? amp : -amp;
            ch->u.drum.stage_tick--;
        }
    } else if (dt == 1) {
        /* SNARE: noise + short body pulse, no biquad */
        uint16_t n = drum_noise_m(ch);
        int32_t noise = (n & 1) ? 26000 : -26000;
        ch->phase = (ch->phase + ch->freq) & FX_MASK;
        int32_t body = (ch->phase < FX_HALF) ? 22000 : -22000;
        if (ch->u.drum.stage_tick) {
            body = (body * (int32_t)ch->u.drum.stage_tick) / 256;
            if (ch->u.drum.stage_tick > 1) ch->u.drum.stage_tick--;
            else ch->u.drum.stage_tick = 0;
        } else body = 0;
        out = (noise * (255 - ch->u.drum.stage) + body * ch->u.drum.stage) / 255;
    } else if (dt == 3) {
        /* CLAP: 3 bursts + tail */
        int32_t amp = 0;
        if (ch->u.drum.stage < 3) {
            int16_t burst_len = (int16_t)(ch->u.drum.bq_z2[1] & 0xFF);
            if (burst_len < 8) burst_len = 8;
            int32_t into = burst_len - (int16_t)ch->u.drum.stage_tick;
            int32_t half = burst_len / 2;
            int32_t env = (into < half) ? (into * 256 / half) : ((burst_len - into) * 256 / half);
            if (env < 0) env = 0; if (env > 256) env = 256;
            uint16_t n = drum_noise_m(ch);
            amp = ((n & 1) ? 26000 : -26000) * env >> 8;
        } else {
            uint16_t n = drum_noise_m(ch);
            amp = (n & 1) ? 9000 : -9000;
        }
        if (ch->u.drum.stage_tick > 0) ch->u.drum.stage_tick--;
        else if (ch->u.drum.stage < 3) {
            ch->u.drum.stage++;
            ch->u.drum.stage_tick = (ch->u.drum.stage == 3)
                ? 0xFFFF
                : (ch->u.drum.bq_z2[1] & 0xFF);
        }
        out = amp;
    } else {
        /* HAT/CRASH: HP-filtered noise */
        uint16_t n = drum_noise_m(ch);
        int32_t src = (n & 1) ? 24000 : -24000;
        fixed16 hp = ch->u.drum.pitch_env_target;
        int32_t y = src - ch->u.drum.bq_z1[0];
        ch->u.drum.bq_z1[0] += (int32_t)(((int64_t)y * hp) >> FX_SHIFT);
        out = y;
    }
    if (out > 32767) out = 32767; else if (out < -32767) out = -32767;
    if (out > 32767) out = 32767; else if (out < -32767) out = -32767;
    return (int16_t)out;
}
#endif

#ifndef BIRB_NO_FORMANT
/* Vowel formant frequency table — coefficients computed at runtime so the
 * resonance slider steers Q (mirrors birb_synth.c full build). */
static const uint16_t formant_freqs_m[5][3] = {
    /* A */ { 730, 1090, 2440 },
    /* E */ { 530, 1840, 2480 },
    /* I */ { 270, 2290, 3010 },
    /* O */ { 570,  840, 2410 },
    /* U */ { 300,  870, 2240 },
};
static const fixed16 formant_gains_m[3] = { FX_ONE, FX_ONE * 7 / 10, FX_ONE * 4 / 10 };

/* fixed-point sin/cos, no libm. Domain restricted to [0, π/2]. */
static fixed16 fix_sin_m(fixed16 x) {
    int64_t x2 = ((int64_t)x * x) >> FX_SHIFT;
    int64_t x3 = (x2 * x) >> FX_SHIFT;
    int64_t x5 = (x3 * x2) >> FX_SHIFT;
    return (fixed16)(x - x3 / 6 + x5 / 120);
}
static fixed16 fix_cos_m(fixed16 x) {
    int64_t x2 = ((int64_t)x * x) >> FX_SHIFT;
    int64_t x4 = (x2 * x2) >> FX_SHIFT;
    return (fixed16)(FX_ONE - x2 / 2 + x4 / 24);
}

static void formant_calc_coeffs_m(uint8_t vowel, fixed16 q, fixed16 dst[3][3]) {
    if (vowel > 4) vowel = 0;
    if (q < FX_ONE / 2) q = FX_ONE / 2;
    for (int i = 0; i < 3; i++) {
        fixed16 omega = (fixed16)(((int32_t)formant_freqs_m[vowel][i] * 19) >> 1);
        fixed16 sn = fix_sin_m(omega);
        fixed16 cs = fix_cos_m(omega);
        fixed16 alpha = (fixed16)(((int64_t)sn << FX_SHIFT) / (2 * (int64_t)q));
        fixed16 denom = FX_ONE + alpha;
        if (denom < 1) denom = 1;
        fixed16 inv = (fixed16)(((int64_t)FX_ONE << FX_SHIFT) / denom);
        int64_t b0 = ((int64_t)alpha * inv) >> FX_SHIFT;
        b0 = (b0 * formant_gains_m[i]) >> FX_SHIFT;
        dst[i][0] = (fixed16)b0;
        int64_t a1 = (int64_t)(-2) * cs;
        a1 = (a1 * inv) >> FX_SHIFT;
        dst[i][1] = (fixed16)a1;
        int64_t a2 = ((int64_t)(FX_ONE - alpha) * inv) >> FX_SHIFT;
        dst[i][2] = (fixed16)a2;
    }
}

static void formant_interp_m(birb_channel *ch) {
    uint8_t va = ch->u.formant.vowel_a; if (va > 4) va = 0;
    uint8_t vb = ch->u.formant.vowel_b; if (vb > 4) vb = 0;
    fixed16 q = FX_ONE * 2 + (fixed16)(((int64_t)FX_ONE * 30 * ch->u.formant.resonance) / 255);
    fixed16 cA[3][3], cB[3][3];
    formant_calc_coeffs_m(va, q, cA);
    formant_calc_coeffs_m(vb, q, cB);
    int32_t t = ch->u.formant.sweep_pos;
    int32_t omt = 255 - t;
    for (int i = 0; i < 3; i++) {
        ch->u.formant.bq_b0[i] = (fixed16)((cA[i][0] * omt + cB[i][0] * t) / 255);
        ch->u.formant.bq_a1[i] = (fixed16)((cA[i][1] * omt + cB[i][1] * t) / 255);
        ch->u.formant.bq_a2[i] = (fixed16)((cA[i][2] * omt + cB[i][2] * t) / 255);
    }
}

static inline int32_t biquad_bp_step_m(int32_t x, fixed16 b0, fixed16 a1, fixed16 a2,
                                        int32_t *z1, int32_t *z2) {
    int32_t y = (int32_t)((((int64_t)b0 * x) >> FX_SHIFT) + *z1);
    *z1 = (int32_t)(-(((int64_t)a1 * y) >> FX_SHIFT) + *z2);
    *z2 = (int32_t)((-(int64_t)b0 * x) >> FX_SHIFT) - (int32_t)(((int64_t)a2 * y) >> FX_SHIFT);
    return y;
}

static int16_t generate_formant(birb_channel *ch) {
    fixed16 p = ch->phase;
    int16_t src;
    if (ch->u.formant.src_wave == WAVE_NOISE) {
        uint16_t v = ch->u.formant.src_lfsr;
        uint16_t bit = (v ^ (v >> 1)) & 1;
        v = (v >> 1) | (bit << 14); if (!v) v = 0x7FFF;
        ch->u.formant.src_lfsr = v;
        src = (v & 1) ? 16383 : -16383;
    } else if (ch->u.formant.src_wave == WAVE_PULSE) {
        src = p < ch->base_duty ? 16383 : -16383;
    } else {
        src = (int16_t)(((int32_t)p * 2 - FX_ONE) * 32767 / FX_ONE);
    }
    int32_t y0 = biquad_bp_step_m(src, ch->u.formant.bq_b0[0], ch->u.formant.bq_a1[0], ch->u.formant.bq_a2[0], &ch->u.formant.bq_z1[0], &ch->u.formant.bq_z2[0]);
    int32_t y1 = biquad_bp_step_m(src, ch->u.formant.bq_b0[1], ch->u.formant.bq_a1[1], ch->u.formant.bq_a2[1], &ch->u.formant.bq_z1[1], &ch->u.formant.bq_z2[1]);
    int32_t y2 = biquad_bp_step_m(src, ch->u.formant.bq_b0[2], ch->u.formant.bq_a1[2], ch->u.formant.bq_a2[2], &ch->u.formant.bq_z1[2], &ch->u.formant.bq_z2[2]);
    int32_t out = y0 + y1 + y2;
    if (ch->u.formant.sweep_speed) {
        uint8_t r = ++ch->u.formant.recalc;
        if ((r & 0x1F) == 0) {
            uint8_t step = ch->u.formant.sweep_speed >> 3; if (!step) step = 1;
            int16_t sp = (int16_t)ch->u.formant.sweep_pos + ch->u.formant.sweep_dir * step;
            if (sp >= 255) { sp = 255; ch->u.formant.sweep_dir = -1; }
            else if (sp <= 0) { sp = 0; ch->u.formant.sweep_dir = +1; }
            ch->u.formant.sweep_pos = (uint8_t)sp;
            formant_interp_m(ch);
        }
    }
    if (out > 32767) out = 32767; else if (out < -32767) out = -32767;
    return (int16_t)out;
}
#endif

#ifndef BIRB_NO_SAMPLES
static int16_t gen_sample_playback_mini(birb_channel *ch, birb_song *song) {
    if (!ch->u.sample.sample_active || ch->u.sample.sample_idx >= song->num_samples) return 0;
    birb_sample_meta *m = &song->samples[ch->u.sample.sample_idx];
    uint32_t pos = ch->u.sample.sample_pos >> FX_SHIFT;
    if (pos >= m->length) {
        if (m->loop_start != 0xFFFFFFFFu && m->loop_end > m->loop_start) {
            uint32_t loop_len = m->loop_end - m->loop_start;
            pos = m->loop_start + ((pos - m->loop_start) % loop_len);
            ch->u.sample.sample_pos = ((uint32_t)pos << FX_SHIFT) | (ch->u.sample.sample_pos & FX_MASK);
        } else {
            ch->u.sample.sample_active = 0;
            return 0;
        }
    }
    int16_t s = song->sample_pool[m->offset + pos];
    ch->u.sample.sample_pos += ch->u.sample.sample_speed;
    return s;
}
#endif

static int16_t generate_sample(birb_channel *ch, birb_song *song) {
    switch (ch->synth_type) {
        case SYNTH_BASIC:  return gen_basic(ch);
#ifndef BIRB_NO_SAMPLES
        case SYNTH_SAMPLE: return gen_sample_playback_mini(ch, song);
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

/* ---------- envelope ---------- */

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
        case ENV_DECAY: {
            fixed16 target = FX_ONE * ch->adsr.sustain / 255;
            if (ch->adsr.decay == 0) {
                ch->env_level = target;
                ch->env_stage = ENV_SUSTAIN;
            } else {
                ch->env_level -= (FX_ONE - target) / (ch->adsr.decay + 1);
                if (ch->env_level <= target) {
                    ch->env_level = target;
                    ch->env_stage = ENV_SUSTAIN;
                }
            }
            break;
        }
        case ENV_SUSTAIN:
            break;
        case ENV_RELEASE:
            if (ch->adsr.release == 0 || ch->env_level < 64) {
                ch->env_level = 0;
                ch->env_stage = ENV_OFF;
            } else {
                ch->env_level -= ch->env_level / (ch->adsr.release + 1);
            }
            break;
        default:
            ch->env_level = 0;
            break;
    }
}

/* ---------- note helpers ---------- */

static void trigger_note(birb_channel *ch, uint8_t note, birb_instrument *inst, birb_song *song) {
    int semi = note - BIRB_NOTE_C0;
    ch->base_note = (uint8_t)semi;
    ch->base_freq = note_to_freq(semi);
    ch->freq = ch->base_freq;
    ch->phase = 0;
    ch->synth_type = inst->synth_type;
    ch->base_duty = inst->duty;
    ch->adsr = inst->envelope;
    if (ch->adsr.attack == 0) {
        ch->env_level = FX_ONE;
        ch->env_stage = ENV_DECAY;
    } else {
        ch->env_level = 0;
        ch->env_stage = ENV_ATTACK;
    }
    ch->pitch_env = inst->pitch_env;
    ch->pitch_env_ticks = inst->pitch_env_len;
    ch->arp_note1 = inst->arp_note1;
    ch->arp_note2 = inst->arp_note2;
    ch->volume = inst->volume;
    ch->row_vol = 255;
    ch->arp_tick = 0;
    ch->pitch_slide = 0;

    if (inst->synth_type == SYNTH_BASIC) {
        ch->u.basic.waveform = inst->waveform;
        ch->u.basic.duty = inst->duty;
        if (inst->waveform == WAVE_NOISE) {
            ch->u.basic.lfsr = 0x7FFF;
            ch->u.basic.lfsr_count = 0;
            ch->u.basic.lfsr_period = (uint16_t)(256 >> (semi / 12));
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
            uint32_t ri = inst->fm.ops[i].ratio_i;
            uint32_t rf = inst->fm.ops[i].ratio_f;
            if (i < nops && (ri | rf) == 0) { ri = 1; rf = 0; }
            ch->u.fm.op_freq[i] = (fixed16)(((uint64_t)ch->base_freq * ((ri << 4) | (rf & 0xF))) >> 4);
            ch->u.fm.op_lvl[i] = (fixed16)(FX_ONE * (int32_t)inst->fm.ops[i].level / 255);
            if (inst->fm.ops[i].adsr.attack == 0) {
                ch->u.fm.op_env[i]   = FX_ONE;
                ch->u.fm.op_stage[i] = ENV_DECAY;
            } else {
                ch->u.fm.op_env[i]   = 0;
                ch->u.fm.op_stage[i] = ENV_ATTACK;
            }
        }
    }
#endif
#ifndef BIRB_NO_KS
    else if (inst->synth_type == SYNTH_KS) {
        uint32_t len = ch->freq > 0 ? (uint32_t)(FX_ONE / ch->freq) : 0;
        if (len < 4) len = 4;
        if (len > BIRB_KS_BUF_SIZE) len = BIRB_KS_BUF_SIZE;
        ch->u.ks.buf_len = (uint16_t)len;
        ch->u.ks.buf_pos = 0;
        ch->u.ks.damping = inst->ks_damping;
        uint16_t lfsr = (uint16_t)(0x7FFF ^ ((uint16_t)semi * 0x1D79));
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
        uint8_t dt = inst->drum_type; if (dt > 5) dt = 0;
        uint8_t algo = (dt == 4) ? 0 : (dt == 5) ? 2 : dt;
        ch->u.drum.drum_type = algo;
        ch->u.drum.stage = 0;
        ch->u.drum.stage_tick = 0;
        ch->u.drum.phase2 = 0;
        ch->u.drum.bq_z1[0] = ch->u.drum.bq_z1[1] = 0;
        ch->u.drum.bq_z2[0] = ch->u.drum.bq_z2[1] = 0;
        ch->u.drum.noise_lfsr = (uint16_t)(0x7FFF ^ ((uint16_t)semi * 0x3D7F));
        if (!ch->u.drum.noise_lfsr) ch->u.drum.noise_lfsr = 0x7FFF;
        int dn = semi + (int)inst->drum_tune;
        if (dn < 0) dn = 0; if (dn > 95) dn = 95;
        fixed16 dfreq = note_to_freq(dn);
        uint32_t ttl;
        if (algo == 0) {
            ch->u.drum.pitch_env = dfreq << 1;
            ch->u.drum.pitch_env_target = dfreq >> 1;
            ch->base_duty = (fixed16)((uint32_t)inst->drum_tone * 256);
            if (ch->base_duty < 16) ch->base_duty = 16;
            ch->u.drum.stage = inst->drum_snap;
            ch->u.drum.stage_tick = 384;
            ttl = (uint32_t)inst->drum_decay * 200 + 1024;
            if (dt == 4) ttl *= 2;
        } else if (algo == 1) {
            ch->freq = dfreq > 0 ? dfreq : base_freq[2] << 2;
            ch->u.drum.stage = inst->drum_snap;
            ch->u.drum.stage_tick = 64 + (inst->drum_tone >> 1);
            ttl = (uint32_t)inst->drum_decay * 120 + 1024;
        } else if (algo == 2) {
            fixed16 hp = (fixed16)((uint32_t)inst->drum_snap * (FX_ONE * 15 / 16) / 255);
            if (hp < FX_ONE / 16) hp = FX_ONE / 16;
            ch->u.drum.pitch_env_target = hp;
            ttl = (uint32_t)inst->drum_decay * 180 + 1024;
            if (dt == 5) ttl = 90000 + (uint32_t)inst->drum_decay * 400;
        } else {
            ch->u.drum.bq_z2[1] = 80 + (inst->drum_snap >> 1);
            ch->u.drum.stage = 0;
            ch->u.drum.stage_tick = (uint16_t)ch->u.drum.bq_z2[1];
            ttl = (uint32_t)inst->drum_decay * 160 + 2048;
        }
        if (ttl > 0xFFFFFFu) ttl = 0xFFFFFFu;
        ch->u.drum.ttl_hi = (uint8_t)(ttl >> 16);
        ch->u.drum.ttl_lo = (uint16_t)(ttl & 0xFFFF);
    }
#endif
#ifndef BIRB_NO_FORMANT
    else if (inst->synth_type == SYNTH_FORMANT) {
        for (int i = 0; i < 3; i++) { ch->u.formant.bq_z1[i] = 0; ch->u.formant.bq_z2[i] = 0; }
        uint8_t sw = inst->formant_source_wave;
        if (sw != WAVE_PULSE && sw != WAVE_SAWTOOTH && sw != WAVE_NOISE) sw = WAVE_SAWTOOTH;
        ch->u.formant.src_wave = sw;
        ch->u.formant.src_lfsr = (uint16_t)(0x7FFF ^ ((uint16_t)semi * 0x2BCD));
        if (!ch->u.formant.src_lfsr) ch->u.formant.src_lfsr = 0x7FFF;
        ch->u.formant.vowel_a = inst->formant_vowel_a > 4 ? 0 : inst->formant_vowel_a;
        ch->u.formant.vowel_b = inst->formant_vowel_b > 4 ? 0 : inst->formant_vowel_b;
        ch->u.formant.sweep_speed = inst->formant_sweep_speed;
        ch->u.formant.sweep_pos = 0;
        ch->u.formant.sweep_dir = +1;
        ch->u.formant.recalc = 0;
        ch->u.formant.resonance = inst->formant_resonance;
        switch (inst->formant_duty) {
            case 0: ch->base_duty = DUTY_12; break;
            case 1: ch->base_duty = DUTY_25; break;
            case 3: ch->base_duty = DUTY_75; break;
            default: ch->base_duty = DUTY_50; break;
        }
        formant_interp_m(ch);
    }
#endif
#ifndef BIRB_NO_SAMPLES
    else if (inst->synth_type == SYNTH_SAMPLE) {
        ch->u.sample.sample_active = 0;
        if (song && inst->sample_idx < song->num_samples) {
            birb_sample_meta *m = &song->samples[inst->sample_idx];
            int base = m->base_note; if (base > 95) base = 95;
            fixed16 note_f = note_to_freq(semi);
            fixed16 base_f = note_to_freq(base);
            ch->u.sample.sample_idx = inst->sample_idx;
            ch->u.sample.sample_pos = 0;
            ch->u.sample.sample_speed = base_f ? (uint32_t)(((uint64_t)note_f << FX_SHIFT) / base_f) : FX_ONE;
            ch->u.sample.sample_active = 1;
        }
    }
#endif
}

/* ---------- tick effects (streamlined) ---------- */

#ifndef BIRB_NO_FM
/* Per-op A/D/R envelope ticking + live op_lvl refresh. Mirrors editor doTick. */
static void fm_op_envelope_tick_m(birb_channel *ch, birb_instrument *inst) {
    if (!inst) return;
    ch->u.fm.algorithm = inst->fm.algorithm;
    for (int i = 0; i < 4; i++) {
        birb_fm_op *op = &inst->fm.ops[i];
        ch->u.fm.op_lvl[i] = (fixed16)(FX_ONE * (int32_t)op->level / 255);
        birb_env_stage st = ch->u.fm.op_stage[i];
        fixed16 en = ch->u.fm.op_env[i];
        if (st == ENV_ATTACK) {
            en += FX_ONE / (op->adsr.attack + 1);
            if (en >= FX_ONE) { en = FX_ONE; st = ENV_DECAY; }
        } else if (st == ENV_DECAY) {
            fixed16 target = FX_ONE * op->adsr.sustain / 255;
            en -= (FX_ONE - target) / (op->adsr.decay + 1);
            if (en <= target) { en = target; st = ENV_SUSTAIN; }
        } else if (st == ENV_RELEASE) {
            en -= en / (op->adsr.release + 1);
            if (en < 64) { en = 0; st = ENV_OFF; }
        }
        ch->u.fm.op_stage[i] = st;
        ch->u.fm.op_env[i] = en;
    }
}
#endif

#ifndef BIRB_NO_FORMANT
static void formant_live_tick_m(birb_channel *ch, birb_instrument *inst) {
    if (!inst) return;
    ch->u.formant.vowel_a     = inst->formant_vowel_a > 4 ? 0 : inst->formant_vowel_a;
    ch->u.formant.vowel_b     = inst->formant_vowel_b > 4 ? 0 : inst->formant_vowel_b;
    ch->u.formant.sweep_speed = inst->formant_sweep_speed;
    ch->u.formant.resonance   = inst->formant_resonance;
    uint8_t sw = inst->formant_source_wave;
    if (sw == WAVE_PULSE || sw == WAVE_SAWTOOTH || sw == WAVE_NOISE)
        ch->u.formant.src_wave = sw;
}
#endif

static void tick_effects(birb_channel *ch, birb_song *song) {
    /* pitch envelope */
    if (ch->pitch_env_ticks > 0) {
        ch->base_freq += (fixed16)ch->pitch_env << 2;
        if (ch->base_freq < 1) ch->base_freq = 1;
        ch->pitch_env_ticks--;
    }

    /* pitch slide */
    if (ch->pitch_slide) {
        ch->base_freq += ch->pitch_slide;
        if (ch->base_freq < 1) ch->base_freq = 1;
    }

    /* tone portamento */
    if (ch->porta_target > 0 && ch->porta_speed > 0) {
        if (ch->base_freq < ch->porta_target) {
            ch->base_freq += ch->porta_speed;
            if (ch->base_freq > ch->porta_target) ch->base_freq = ch->porta_target;
        } else if (ch->base_freq > ch->porta_target) {
            ch->base_freq -= ch->porta_speed;
            if (ch->base_freq < ch->porta_target) ch->base_freq = ch->porta_target;
        }
    }

    /* arpeggio */
    if (ch->arp_note1 | ch->arp_note2) {
        int note = ch->base_note;
        int t = ch->arp_tick % 3;
        if (t == 1) note += ch->arp_note1;
        else if (t == 2) note += ch->arp_note2;
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

    /* tremolo */
    if (ch->tremolo_depth > 0) {
        fixed16 tr = birb_sin_approx(ch->tremolo_phase);
        ch->tremolo_mod = FX_MUL(tr, ch->tremolo_depth);
        ch->tremolo_phase += ch->tremolo_speed;
    } else ch->tremolo_mod = 0;

    envelope_tick(ch);

#ifndef BIRB_NO_FM
    if (ch->synth_type == SYNTH_FM && ch->base_freq > 0
        && song && ch->cur_instrument < BIRB_MAX_INSTRUMENTS) {
        birb_instrument *inst = &song->instruments[ch->cur_instrument];
        for (int i = 0; i < 4; i++) {
            uint32_t ri = inst->fm.ops[i].ratio_i;
            uint32_t rf = inst->fm.ops[i].ratio_f;
            ch->u.fm.op_freq[i] = (fixed16)(((uint64_t)ch->freq * ((ri << 4) | (rf & 0xF))) >> 4);
        }
        fm_op_envelope_tick_m(ch, inst);
    }
#endif
#ifndef BIRB_NO_FORMANT
    if (ch->synth_type == SYNTH_FORMANT && song && ch->cur_instrument < BIRB_MAX_INSTRUMENTS) {
        formant_live_tick_m(ch, &song->instruments[ch->cur_instrument]);
    }
#endif
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
#ifndef BIRB_NO_SAMPLES
        case FX_SAMPLE_OFFSET:
            if (ch->synth_type == SYNTH_SAMPLE)
                ch->u.sample.sample_pos = ((uint32_t)param << 8) << FX_SHIFT;
            break;
#endif
        case FX_POS_JUMP:
            state->jump_order = param;
            state->jump_row = 0;
            break;
        case FX_PAT_BREAK:
            if (state->jump_order < 0) state->jump_order = state->order_pos + 1;
            state->jump_row = param;
            break;
        case FX_SET_SPEED:
            if (!param) break;
            if (param < 0x20) state->song->ticks_per_row = param;
            else {
                state->song->bpm = param;
                state->samples_per_tick = BIRB_SAMPLE_RATE * 5 / (param * 2);
            }
            break;
        default:
            break;
    }
}

/* ---------- sequencer ---------- */

static void process_row(birb_state *state) {
    birb_song *song = state->song;
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
        birb_channel *ch = &state->channels[c];
        int pat_idx = song->order[state->order_pos][c];
        if (pat_idx >= song->num_patterns) continue;
        birb_row *r = &song->patterns[pat_idx][state->current_row][c];

        ch->retrig_interval = 0;
        ch->note_cut_tick = 0;
        ch->note_delay_tick = 0;

        int is_tone_porta = (r->effect == FX_TONE_PORTA);
        int is_note_delay = (r->effect == FX_EXTENDED && ((r->param >> 4) & 0xF) == 0xD);

        if (is_note_delay && r->note >= BIRB_NOTE_C0) {
            ch->delayed_note = r->note;
            ch->delayed_inst = (r->instrument != 0xFF) ? r->instrument : ch->cur_instrument;
            ch->note_delay_tick = r->param & 0x0F;
        } else if (r->note == BIRB_NOTE_OFF) {
            ch->env_stage = ENV_RELEASE;
#ifndef BIRB_NO_FM
            if (ch->synth_type == SYNTH_FM) {
                for (int i = 0; i < 4; i++)
                    if (ch->u.fm.op_stage[i] != ENV_OFF)
                        ch->u.fm.op_stage[i] = ENV_RELEASE;
            }
#endif
        } else if (r->note >= BIRB_NOTE_C0) {
            if (is_tone_porta) {
                ch->porta_target = note_to_freq(r->note - BIRB_NOTE_C0);
            } else {
                uint8_t inst_idx = r->instrument;
                if (inst_idx == 0xFF) inst_idx = ch->cur_instrument;
                if (inst_idx < song->num_instruments) {
                    ch->cur_instrument = inst_idx;
                    trigger_note(ch, r->note, &song->instruments[inst_idx], song);
                }
            }
        }

        if (r->volume) ch->row_vol = r->volume;
        if (r->effect) process_effects(ch, r->effect, r->param, state);
    }
}

static void advance_tick(birb_state *state) {
    birb_song *song = state->song;
    state->current_tick++;
    if (state->current_tick >= song->ticks_per_row) {
        state->current_tick = 0;
        if (state->jump_order >= 0) {
            state->order_pos = state->jump_order;
            if (state->order_pos >= song->order_length) state->order_pos = 0;
            state->current_row = state->jump_row;
            state->jump_order = -1; state->jump_row = 0;
        } else {
            state->current_row++;
            int pat_len = song->pattern_lengths[song->order[state->order_pos][0]];
            if (!pat_len) pat_len = BIRB_MAX_ROWS;
            if (state->current_row >= pat_len) {
                state->current_row = 0;
                state->order_pos++;
                if (state->order_pos >= song->order_length) state->order_pos = 0;
            }
        }
        process_row(state);
        state->row_out = state->current_row;
        state->pattern_out = state->order_pos;
    }
    int tick = state->current_tick;
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
        birb_channel *ch = &state->channels[c];
        if (ch->note_delay_tick > 0 && tick == ch->note_delay_tick) {
            if (ch->delayed_inst < state->song->num_instruments) {
                ch->cur_instrument = ch->delayed_inst;
                trigger_note(ch, ch->delayed_note, &state->song->instruments[ch->delayed_inst], state->song);
            }
            ch->note_delay_tick = 0;
        }
        if (ch->note_cut_tick > 0 && tick == ch->note_cut_tick) {
            ch->env_level = 0; ch->env_stage = ENV_OFF;
        }
        if (ch->retrig_interval > 0 && tick > 0 && (tick % ch->retrig_interval) == 0) {
            ch->phase = 0; ch->env_stage = ENV_ATTACK; ch->env_level = 0;
            if (ch->synth_type == SYNTH_BASIC && ch->u.basic.waveform == WAVE_NOISE) {
                ch->u.basic.lfsr = 0x7FFF; ch->u.basic.lfsr_count = 0;
            }
        }
        tick_effects(ch, state->song);
    }
}

/* ---------- public API ---------- */

void birb_init(birb_state *state, birb_song *song) {
    for (int i = 0; i < (int)sizeof(birb_state); i++)
        ((uint8_t *)state)[i] = 0;
    state->song = song;
    int bpm = song->bpm ? song->bpm : 125;
    state->samples_per_tick = BIRB_SAMPLE_RATE * 5 / (bpm * 2);
    state->current_tick = 0;
    state->jump_order = -1;
    state->jump_row = 0;
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
        state->channels[c].u.basic.lfsr = 0x7FFF;
        state->channels[c].u.basic.lfsr_period = 16;
    }
    process_row(state);
}

void birb_render(birb_state *state, int16_t *output, int num_samples) {
    for (int i = 0; i < num_samples; i++) {
        if (state->tick_counter <= 0) {
            advance_tick(state);
            state->tick_counter = state->samples_per_tick;
        }
        state->tick_counter--;
        int32_t mix = 0;
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
            birb_channel *ch = &state->channels[c];
            if (ch->env_stage == ENV_OFF && ch->env_level == 0) continue;
            int32_t s = generate_sample(ch, state->song);
            fixed16 env = ch->env_level;
            if (ch->tremolo_mod) {
                env += FX_MUL(env, ch->tremolo_mod);
                if (env < 0) env = 0;
                if (env > FX_ONE) env = FX_ONE;
            }
            int32_t vol = ch->volume ? ch->volume : 255;
            int32_t rvol = ch->row_vol;
            int32_t out = (s * FX_TO_INT(env * 256)) >> 8;
            out = out * vol / 255;
            out = out * rvol / 255;
            mix += out;
            ch->phase += ch->freq;
            if (ch->phase >= FX_ONE) ch->phase -= FX_ONE;
        }
        if (mix > 32767) mix = 32767;
        if (mix < -32767) mix = -32767;
        output[i] = (int16_t)mix;
    }
}

int birb_get_row(birb_state *state) { return state->row_out; }
int birb_get_pattern(birb_state *state) { return state->pattern_out; }

/* provide the symbol for non-mini builds that reference it */
const fixed16 birb_note_freq[96] = {0};
