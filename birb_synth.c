/*
 * birb_synth.c — minimal chiptune synth engine
 * No stdlib, no malloc, no floats.
 */
#include "birb_synth.h"

/* ---------- note frequency lookup ---------- *
 * Phase increment per sample at BIRB_SAMPLE_RATE=44100, as a 12-entry
 * octave-base table shifted by octave: freq = base[n%12] << (n/12).
 *
 * This is the ONE canonical table. The editor (nf()), the birbc/editor JS
 * emit, and the 4K players all compute pitch exactly this way, so the full
 * and 4K engines are bit-identical. A precise 96-entry table was tried here
 * once; it disagreed with the editor's tuning by ~0.26%/note, which drifts
 * oscillators to opposite phase within a second. Do NOT reintroduce a
 * per-build note table — parity depends on there being only this one.
 */
static const fixed16 octave_base[12] = {
    24, 26, 27, 29, 31, 32, 34, 36, 38, 41, 43, 46,
};
fixed16 birb_note_to_freq(int note) {
    if (note < 0) note = 0;
    if (note > 95) note = 95;
    return octave_base[note % 12] << (note / 12);
}

/* ---------- sine approximation ---------- *
 * 5th-order minimax polynomial — Horner form:
 *     y = x · (c1 - x² · (c3 - x² · c5))
 * where c1, c3, c5 are halved Remez coefficients for sin(π/2 · x) on [0, 1],
 * and the final result is doubled. The halving keeps every intermediate
 * value inside the fixed16 [0, FX_ONE] range so int32 multiplies don't
 * need extra headroom.
 *
 * Peak error ≈ 0.05% vs ≈ 5% for the old parabolic approximation. Buys
 * ~18 dB headroom before FM aliasing turns into audible noise hash on
 * top of intended timbres. About 10 ops per call, no static data.
 *
 * Reduce-to-first-quadrant via two folds:
 *   second half (phase ≥ FX_HALF)  →  negate output
 *   second quarter (t > FX_ONE/4)  →  mirror t = FX_HALF - t
 * That leaves t in [0, FX_ONE/4]; scaling t<<2 maps to x ∈ [0, FX_ONE]
 * which represents the polynomial input on [0, 1]. */
fixed16 birb_sin_approx(fixed16 phase) {
    phase &= FX_MASK;
    int neg = (phase >= FX_HALF);
    fixed16 t = neg ? phase - FX_HALF : phase;
    if (t > FX_ONE / 4) t = FX_HALF - t;
    fixed16 x = t << 2;
    if (x > FX_ONE) x = FX_ONE;
    fixed16 x2 = (fixed16)(((int64_t)x * x) >> FX_SHIFT);
    /* Halved Remez coefficients: π/4, 0.3216146, 0.0363889 (in fixed16) */
    const fixed16 c1 = 51472;
    const fixed16 c3 = 21080;
    const fixed16 c5 = 2385;
    fixed16 inner = c3 - (fixed16)(((int64_t)x2 * c5) >> FX_SHIFT);
    fixed16 mid = c1 - (fixed16)(((int64_t)x2 * inner) >> FX_SHIFT);
    int32_t y = (int32_t)(((int64_t)mid * x) >> FX_SHIFT);
    y <<= 1;
    if (y > FX_ONE) y = FX_ONE;
    return neg ? -(fixed16)y : (fixed16)y;
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
/* 4-op FM generator — mirrors fmRender4() in editor.html. 8 algorithms.
 * Conventions:
 *   op0 = output (in linear chain), op3 = deepest mod
 *   each op's effective level lN = op_lvl[N] * op_env[N] / FX_ONE  (in [0..FX_ONE])
 *   mod_index scales modulation entering any carrier (encoded /255)
 *   feedback wraps prev_out (last raw carrier sine ×FX_ONE) into op3's phase
 *
 * Sub-expression sN holds modulator i's pre-mi phase contribution scaled by lN
 * (same maths as JS: `sinApprox(...) * lN`). Multi-carrier algos (5/6/7) sum
 * carriers and divide by carrier count to match the JS averaging.
 *
 * raw is the carrier op0's pre-level sine ×FX_ONE; it goes into prev_out for
 * feedback. The 2-op path (num_ops < 4) is the simpler op1→op0 with feedback. */
/* Float sine matching the editor's sinApprox() bit-for-bit (same Remez
 * coefficients, same first-quadrant folds). One cycle = FX_ONE (== editor F).
 * Returns [-1, 1]. Range-reduces any real phase to [0, FX_ONE) without libm:
 * p = phase - FX_ONE*floor(phase/FX_ONE), floor done via int cast + adjust. */
static double fm_sin(double phase) {
    double q = phase * (1.0 / (double)FX_ONE);
    int fq = (int)q;
    if (q < 0.0) fq -= 1;                   /* floor for negative q */
    double p = phase - (double)fq * (double)FX_ONE;
    int neg = p >= (double)FX_ONE * 0.5;
    double t = neg ? p - (double)FX_ONE * 0.5 : p;
    if (t > (double)FX_ONE * 0.25) t = (double)FX_ONE * 0.5 - t;
    double x = t * (4.0 / (double)FX_ONE);
    if (x > 1.0) x = 1.0;
    double x2 = x * x;
    double y = x * (1.5707288 - x2 * (0.6432292 - x2 * 0.0727778));
    if (y > 1.0) y = 1.0;
    return neg ? -y : y;
}

/* FM is computed in float, not fixed-point. High-feedback 4-op timbres (the
 * braaams) form a nonlinear feedback loop that a fixed16 rounding budget pushes
 * into a Nyquist limit-cycle the editor's float math never enters — so the C
 * engine sounded audibly different. Floats keep the loop bit-close to the
 * editor. Levels lN are in [0, FX_ONE] (== editor fmL*fmEnv/F); carrier sine is
 * [-1,1], scaled by lN/FX_ONE for audio. prev_out holds raw*FX_ONE for feedback. */
static int16_t gen_fm(birb_channel *ch) {
    double l0 = (double)ch->u.fm.op_lvl[0] * (double)ch->u.fm.op_env[0] / (double)FX_ONE;
    double l1 = (double)ch->u.fm.op_lvl[1] * (double)ch->u.fm.op_env[1] / (double)FX_ONE;
    double l2 = (double)ch->u.fm.op_lvl[2] * (double)ch->u.fm.op_env[2] / (double)FX_ONE;
    double l3 = (double)ch->u.fm.op_lvl[3] * (double)ch->u.fm.op_env[3] / (double)FX_ONE;
    double mi = (double)ch->u.fm.mod_index / 255.0;
    double fb = ch->u.fm.feedback
        ? ch->u.fm.prev_out * (double)ch->u.fm.feedback / 256.0
        : 0.0;

    double ph0 = (double)ch->u.fm.op_phase[0];
    double ph1 = (double)ch->u.fm.op_phase[1];
    double ph2 = (double)ch->u.fm.op_phase[2];
    double ph3 = (double)ch->u.fm.op_phase[3];

    double raw, s;
    int nops = ch->u.fm.num_ops < 4 ? 2 : 4;

    if (nops == 4) {
        double s1, s2, s3, r1, r2, r3;
        switch (ch->u.fm.algorithm & 7) {
            case 0: /* 3->2->1->0 */
                s3 = fm_sin(ph3 + fb) * l3;
                s2 = fm_sin(ph2 + s3) * l2;
                s1 = fm_sin(ph1 + s2) * l1;
                raw = fm_sin(ph0 + s1 * mi);
                s = raw * l0 / (double)FX_ONE;
                break;
            case 1: /* 3+2 -> 1 -> 0 */
                s3 = fm_sin(ph3 + fb) * l3;
                s2 = fm_sin(ph2) * l2;
                s1 = fm_sin(ph1 + s3 + s2) * l1;
                raw = fm_sin(ph0 + s1 * mi);
                s = raw * l0 / (double)FX_ONE;
                break;
            case 2: /* 3->2->0, 1->0 */
                s3 = fm_sin(ph3 + fb) * l3;
                s2 = fm_sin(ph2 + s3) * l2;
                s1 = fm_sin(ph1) * l1;
                raw = fm_sin(ph0 + (s2 + s1) * mi);
                s = raw * l0 / (double)FX_ONE;
                break;
            case 3: /* 3->1->0, 2->0 */
                s3 = fm_sin(ph3 + fb) * l3;
                s1 = fm_sin(ph1 + s3) * l1;
                s2 = fm_sin(ph2) * l2;
                raw = fm_sin(ph0 + (s1 + s2) * mi);
                s = raw * l0 / (double)FX_ONE;
                break;
            case 4: /* 3,2,1 -> 0 (three modulators on one carrier) */
                s3 = fm_sin(ph3 + fb) * l3;
                s2 = fm_sin(ph2) * l2;
                s1 = fm_sin(ph1) * l1;
                raw = fm_sin(ph0 + (s3 + s2 + s1) * mi);
                s = raw * l0 / (double)FX_ONE;
                break;
            case 5: /* 3->2 (carrier), 1->0 (carrier). Sum then / 2. */
                s3 = fm_sin(ph3 + fb) * l3;
                s1 = fm_sin(ph1) * l1;
                r2 = fm_sin(ph2 + s3 * mi);
                raw = fm_sin(ph0 + s1 * mi);
                s = (r2 * l2 + raw * l0) / (double)FX_ONE * 0.5;
                break;
            case 6: /* 3->2 (carrier), 1, 0 (3 carriers). Sum / 3. */
                s3 = fm_sin(ph3 + fb) * l3;
                r2 = fm_sin(ph2 + s3 * mi);
                r1 = fm_sin(ph1);
                raw = fm_sin(ph0);
                s = (r2 * l2 + r1 * l1 + raw * l0) / (double)FX_ONE / 3.0;
                break;
            case 7:
            default: /* all 4 parallel carriers. Sum / 4. */
                r3 = fm_sin(ph3 + fb);
                r2 = fm_sin(ph2);
                r1 = fm_sin(ph1);
                raw = fm_sin(ph0);
                s = (r3 * l3 + r2 * l2 + r1 * l1 + raw * l0) / (double)FX_ONE * 0.25;
                break;
        }
    } else {
        /* 2-op: op1 -> op0 with feedback. Matches editor's 2-op path:
         *   mo = sin(ph1) * (mod_index * l1 / 255);  if (fb) mo += prev*fb/256
         *   raw = sin(ph0 + mo);   prev = raw*FX_ONE;   s = raw*l0/FX_ONE */
        double mo = fm_sin(ph1) * ((double)ch->u.fm.mod_index * l1 / 255.0);
        if (ch->u.fm.feedback) mo += fb;
        raw = fm_sin(ph0 + mo);
        s = raw * l0 / (double)FX_ONE;
    }

    /* feedback memory: raw carrier sine * FX_ONE (editor `C.fmPrev = raw * F`) */
    ch->u.fm.prev_out = raw * (double)FX_ONE;

    /* advance integer phase accumulators for all four ops (mirrors the editor's
     * unconditional 4-iter loop; op2/op3 freqs are 0 or unused in 2-op) */
    for (int i = 0; i < 4; i++)
        ch->u.fm.op_phase[i] = (ch->u.fm.op_phase[i] + ch->u.fm.op_freq[i]) & FX_MASK;

    /* quantise s (in [-1, 1]) to int16; the mixer applies envelope/volume */
    int32_t out = (int32_t)(s * 32767.0 + (s >= 0.0 ? 0.5 : -0.5));
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

/* Replicate JavaScript's `(prod) >> sh`: the editor computes drum filter/sweep
 * steps in JS, where `>>` first coerces its operand to a 32-bit signed int
 * (ToInt32 — wraps mod 2^32). For large products (e.g. the hat HP filter's
 * y*coeff can exceed 2^31) that wrap is audible, and it IS the benchmark
 * sound. An int64 shift would not wrap, so drums drifted from the editor —
 * match the wrap exactly. */
static inline int32_t js_shr(int64_t prod, int sh) {
    return (int32_t)(uint32_t)prod >> sh;
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
        /* KICK / TOM — pitch-swept TRIANGLE body + noise click.
         * Triangle packs harmonics (odd harmonics rolling off at 1/n²) so the
         * body sounds meaty by itself, not thin like a sine. The pitch sweep
         * from pitch_env → pitch_env_target gives the "thump" character;
         * base_duty carries the decay rate. */
        fixed16 p = ch->u.drum.pitch_env;
        fixed16 t = ch->u.drum.pitch_env_target;
        int32_t gap = p - t;
        fixed16 rate = ch->base_duty;
        if (rate < 16) rate = 16;
        p -= js_shr((int64_t)gap * rate, 20);   /* JS >> semantics (see js_shr) */
        ch->u.drum.pitch_env = p;
        ch->phase += p;
        ch->phase &= FX_MASK;
        /* Triangle wave at ±28000, tracking ch->phase through its full cycle. */
        int32_t tri;
        if (ch->phase < FX_HALF) {
            tri = ((int32_t)ch->phase * 4) - FX_ONE;      /* -FX_ONE → +FX_ONE */
        } else {
            tri = FX_ONE * 3 - ((int32_t)ch->phase * 4);  /* +FX_ONE → -FX_ONE */
        }
        out = ((int32_t)tri * 28000) >> FX_SHIFT;
        /* Click: decaying noise burst, snap-controlled amplitude. */
        if (ch->u.drum.stage_tick > 0) {
            uint16_t n = drum_noise(ch);
            int32_t peak = (int32_t)ch->u.drum.stage * 128;
            int32_t click_amp = peak * ch->u.drum.stage_tick / 384;
            int32_t click = (n & 1) ? click_amp : -click_amp;
            out += click;
            ch->u.drum.stage_tick--;
        }
    } else if (dt == 1) {
        /* SNARE — LOUD noise + short tonal body pulse. No biquad — the old
         * resonator rang like a bell. stage holds the noise/body mix balance
         * (0..255, high = body-heavy). stage_tick counts down a short body
         * envelope for the drumhead "crack". */
        uint16_t n = drum_noise(ch);
        int32_t noise = (n & 1) ? 26000 : -26000;
        ch->phase += ch->freq;
        ch->phase &= FX_MASK;
        /* Square-ish body: phase < FX_HALF → +, else −. Harder edge than sine. */
        int32_t body = (ch->phase < FX_HALF) ? 22000 : -22000;
        /* Body fades fast via stage_tick envelope (~5 ms). */
        if (ch->u.drum.stage_tick > 0) {
            body = (body * ch->u.drum.stage_tick) / 256;
            if (ch->u.drum.stage_tick > 1) ch->u.drum.stage_tick--;
            else ch->u.drum.stage_tick = 0;
        } else {
            body = 0;
        }
        /* stage = body/noise mix: 0 = all noise, 255 = all body. */
        int32_t mix_b = ch->u.drum.stage;
        int32_t mix_n = 255 - mix_b;
        out = (noise * mix_n + body * mix_b) / 255;
    } else if (dt == 2 || dt == 5) {
        /* HAT / CRASH — pure noise with a one-pole highpass. The old FM at
         * 1:17 ratio made metallic tones; real hats are nearly all noise
         * energy at high frequency. snap controls the HP cutoff (pitch_env_target
         * from trigger), tone scales initial brightness (pitch_env). */
        uint16_t n = drum_noise(ch);
        int32_t src = (n & 1) ? 24000 : -24000;
        /* One-pole HP: y = x - state, state += coeff*y */
        fixed16 hp = ch->u.drum.pitch_env_target;
        int32_t y = src - ch->u.drum.bq_z1[0];
        ch->u.drum.bq_z1[0] += js_shr((int64_t)y * hp, FX_SHIFT);   /* JS >> wrap */
        out = y;
        if (dt == 5) {
            /* CRASH amp shimmer via slow LFO. Uses ttl as phase. */
            fixed16 lfo = birb_sin_approx((fixed16)((uint32_t)ttl << 3));
            int32_t mul = FX_ONE + (lfo >> 1);
            out = (int32_t)(((int64_t)out * mul) >> FX_SHIFT);
        }
    } else {
        /* CLAP — 3 quick noise bursts + longer tail. Each burst is a bit of
         * envelope-shaped noise; gaps between bursts give the "pa-ta-ta-tack"
         * character. No biquad. stage runs 0..3, stage_tick counts samples
         * within each stage. bq_z2[1] holds the per-burst length. */
        int32_t amp = 0;
        if (ch->u.drum.stage < 3) {
            /* Triangle envelope within each burst: ramp up then down. */
            int16_t burst_len = (int16_t)(ch->u.drum.bq_z2[1] & 0xFF);
            if (burst_len < 8) burst_len = 8;
            int32_t into = burst_len - (int16_t)ch->u.drum.stage_tick;
            int32_t half = burst_len / 2;
            int32_t env = (into < half) ? (into * 256 / half) : ((burst_len - into) * 256 / half);
            if (env < 0) env = 0; if (env > 256) env = 256;
            uint16_t n = drum_noise(ch);
            int32_t src = (n & 1) ? 26000 : -26000;
            amp = (src * env) >> 8;
        } else {
            /* Tail: low-level noise fading out over remaining ttl. */
            uint16_t n = drum_noise(ch);
            amp = (n & 1) ? 9000 : -9000;
        }
        /* Advance stage machine: bursts separated by their own length (gap). */
        if (ch->u.drum.stage_tick > 0) {
            ch->u.drum.stage_tick--;
        } else if (ch->u.drum.stage < 3) {
            ch->u.drum.stage++;
            ch->u.drum.stage_tick = (ch->u.drum.stage == 3)
                ? 0xFFFF
                : (ch->u.drum.bq_z2[1] & 0xFF);
        }
        out = amp;
    }

    if (out > 32767) out = 32767; else if (out < -32767) out = -32767;
    return (int16_t)out;
}
#endif /* BIRB_NO_DRUM */

#ifndef BIRB_NO_FORMANT
/* ---------- vowel formant frequency table ----------
 * Vowel formant centre frequencies (Peterson & Barney) in Hz; coefficients are
 * computed at runtime from these so the resonance slider can actually steer Q.
 * Per-formant gain bakes into b0: F1=1.0, F2=0.7, F3=0.4 (formant_gains[]). */
static const uint16_t formant_freqs[5][3] = {
    /* A */ { 730, 1090, 2440 },
    /* E */ { 530, 1840, 2480 },
    /* I */ { 270, 2290, 3010 },
    /* O */ { 570,  840, 2410 },
    /* U */ { 300,  870, 2240 },
};
static const fixed16 formant_gains[3] = { FX_ONE, FX_ONE * 7 / 10, FX_ONE * 4 / 10 };

/* ---------- fixed-point sin / cos (no libm) ----------
 * Inputs in fixed16 with FX_ONE = 1.0 radian. Domain restricted to [0, π/2]
 * (about 0..103000 in fixed16 units, comfortably under 2π). 5th-order Taylor
 * for sin, 4th-order for cos. Used only for biquad coefficient computation
 * at vowel formant frequencies (≤ 3010 Hz → ω ≤ 0.43 rad ≈ 28000), which is
 * deep inside the convergence radius. Intermediates use int64 to avoid
 * fixed-point overflow on x², x³, etc. */
static fixed16 fix_sin(fixed16 x) {
    int64_t x2 = ((int64_t)x * x) >> FX_SHIFT;
    int64_t x3 = (x2 * x) >> FX_SHIFT;
    int64_t x5 = (x3 * x2) >> FX_SHIFT;
    return (fixed16)(x - x3 / 6 + x5 / 120);
}
static fixed16 fix_cos(fixed16 x) {
    int64_t x2 = ((int64_t)x * x) >> FX_SHIFT;
    int64_t x4 = (x2 * x2) >> FX_SHIFT;
    return (fixed16)(FX_ONE - x2 / 2 + x4 / 24);
}

/* Compute biquad bandpass coefficients (RBJ constant-skirt form):
 *   ω = 2π·f / SR;   α = sin(ω) / (2Q)
 *   b0 =  α / (1+α);   a1 = -2·cos(ω) / (1+α);   a2 = (1-α) / (1+α)
 * (b1 = 0, b2 = -b0 — both folded into the step function.)
 * Frequencies are in Hz, sample rate is BIRB_SAMPLE_RATE (44100). The
 * conversion 2π/44100 is approximated as multiply-by-19 then shift right 1
 * (≈ 0.000145), which is ~1.7% high vs the true 0.0001425 — close enough
 * that vowel character is unchanged but the Q control works as intended.
 * Output is written into dst[3][3] as fixed16. */
static void formant_calc_coeffs(uint8_t vowel, fixed16 q, fixed16 dst[3][3]) {
    if (vowel > 4) vowel = 0;
    if (q < FX_ONE / 2) q = FX_ONE / 2;
    for (int i = 0; i < 3; i++) {
        /* ω in fixed16. 19/2 ≈ 9.5 ≈ FX_ONE * 2π/44100. */
        fixed16 omega = (fixed16)(((int32_t)formant_freqs[vowel][i] * 19) >> 1);
        fixed16 sn = fix_sin(omega);
        fixed16 cs = fix_cos(omega);
        /* α = sn / (2Q) — q is fixed16, so divide as (sn << FX_SHIFT) / (2q). */
        fixed16 alpha = (fixed16)(((int64_t)sn << FX_SHIFT) / (2 * (int64_t)q));
        /* inv = 1 / (1 + α) in fixed16 → (FX_ONE << FX_SHIFT) / (FX_ONE + α). */
        fixed16 denom = FX_ONE + alpha;
        if (denom < 1) denom = 1;
        fixed16 inv = (fixed16)(((int64_t)FX_ONE << FX_SHIFT) / denom);
        /* b0 = α · inv · gain_i */
        int64_t b0 = ((int64_t)alpha * inv) >> FX_SHIFT;
        b0 = (b0 * formant_gains[i]) >> FX_SHIFT;
        dst[i][0] = (fixed16)b0;
        /* a1 = -2·cos(ω)·inv */
        int64_t a1 = (int64_t)(-2) * cs;
        a1 = (a1 * inv) >> FX_SHIFT;
        dst[i][1] = (fixed16)a1;
        /* a2 = (1 - α) · inv */
        int64_t a2 = ((int64_t)(FX_ONE - alpha) * inv) >> FX_SHIFT;
        dst[i][2] = (fixed16)a2;
    }
}

/* Interpolate vowel A and B coefficients (computed at the channel's current
 * Q from `resonance`) into the working set at sweep position t (0..255).
 * Recomputes both vowels every call; with the 32-sample re-interp gate in
 * generate_formant() this still amortises to ~0.5 trig calls per sample.
 * Signed intermediates so negative a1 coefficients don't promote to uint32_t. */
static void formant_interp(birb_channel *ch) {
    uint8_t va = ch->u.formant.vowel_a;
    uint8_t vb = ch->u.formant.vowel_b;
    if (va > 4) va = 0;
    if (vb > 4) vb = 0;
    /* Resonance 0..255 → Q 2..32, fixed16. q = 2 + (res/255)*30. */
    fixed16 q = FX_ONE * 2 + (fixed16)(((int64_t)FX_ONE * 30 * ch->u.formant.resonance) / 255);
    fixed16 coefA[3][3], coefB[3][3];
    formant_calc_coeffs(va, q, coefA);
    formant_calc_coeffs(vb, q, coefB);
    int32_t t = ch->u.formant.sweep_pos;          /* 0..255 */
    int32_t omt = 255 - t;
    for (int i = 0; i < 3; i++) {
        ch->u.formant.bq_b0[i] = (fixed16)((coefA[i][0] * omt + coefB[i][0] * t) / 255);
        ch->u.formant.bq_a1[i] = (fixed16)((coefA[i][1] * omt + coefB[i][1] * t) / 255);
        ch->u.formant.bq_a2[i] = (fixed16)((coefA[i][2] * omt + coefB[i][2] * t) / 255);
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

/* ---------- note helpers ---------- *
 * Internal alias so the rest of this file is independent of which note-table
 * variant got compiled (full 96-entry static or BIRB_TINY_NOTE_TABLE). */
#define note_to_freq(n) birb_note_to_freq(n)

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
    /* Attack=0 means the note must ring out at full volume on tick 0 — start
     * in DECAY at peak level rather than ramping from 0 over the first tick.
     * Matters most for drums (a=0 is the default there). */
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
#ifndef BIRB_NO_REVERB
    ch->reverb_send = inst->reverb_send;
#endif
    ch->arp_tick = 0;
    ch->vibrato_phase = 0;
    ch->vibrato_speed = 0;
    ch->vibrato_depth = 0;
    ch->pitch_slide = 0;
    ch->duty_sweep = 0;
    /* Clear tone-portamento speed on a normal trigger — the editor/emit do
     * this (TR resets C.ps=0). Without it, once any note used 3xx the stale
     * porta keeps sliding every following note toward the old target, which
     * is the kick/bass "rubberbanding". Porta notes skip trigger_note, so
     * this never clobbers a real slide. */
    ch->porta_speed = 0;

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
            /* Round (+8 before >>4), NOT floor — the editor/emit use
             * Math.round(base*ratio); a floor here is 1 unit low for non-integer
             * ratios and drifts the FM operators out of phase over time. */
            ch->u.fm.op_freq[i] = (fixed16)((((uint64_t)ch->base_freq * ((ri << 4) | (rf & 0xF))) + 8) >> 4);
            /* Static per-op level (refreshed live in fm_op_envelope_tick).
             * Round (+127 before /255) to match the editor's Math.round(F*lv/255);
             * a floor here is 1 unit low and desyncs high-feedback FM. */
            ch->u.fm.op_lvl[i] = (fixed16)((FX_ONE * (int32_t)inst->fm.ops[i].level + 127) / 255);
            /* Per-op ADSR start: a==0 jumps to DECAY at peak so the first
             * tick isn't silent (mirrors fmRender4 trigger in editor). */
            if (inst->fm.ops[i].adsr.attack == 0) {
                ch->u.fm.op_env[i]   = (float)FX_ONE;
                ch->u.fm.op_stage[i] = ENV_DECAY;
            } else {
                ch->u.fm.op_env[i]   = 0.0f;
                ch->u.fm.op_stage[i] = ENV_ATTACK;
            }
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
            /* KICK / TOM. Start freq is 2× dfreq for the initial thwack,
             * body settles at dfreq/2 (≈ 65 Hz at C3, audible). Transient
             * burst lives in stage_tick (384 samples ≈ 8.7 ms). */
            fixed16 start_f = dfreq;
            ch->u.drum.pitch_env = start_f << 1;
            ch->u.drum.pitch_env_target = start_f >> 1;
            /* Decay rate: higher `tone` = faster pitch decay. */
            ch->base_duty = (fixed16)((uint32_t)inst->drum_tone * 256);
            if (ch->base_duty < 16) ch->base_duty = 16;
            ch->u.drum.stage = inst->drum_snap;
            ch->u.drum.stage_tick = 384;
            /* lifetime: decay param maps ~8..200ms at 44.1kHz */
            ttl = (uint32_t)inst->drum_decay * 200 + 1024;
            if (dt == 4) ttl = ttl * 2; /* TOM holds longer */
        } else if (algo == 1) {
            /* SNARE — noise + body pulse. stage = body/noise mix (snap),
             * stage_tick = body envelope length in samples (tone-controlled,
             * shorter = crisper "tok", longer = more "thud"). */
            ch->freq = dfreq > 0 ? dfreq : note_to_freq(26); /* D3 fallback */
            ch->u.drum.stage = inst->drum_snap;
            ch->u.drum.stage_tick = 64 + (inst->drum_tone >> 1); /* ~1.5..4 ms body */
            ttl = (uint32_t)inst->drum_decay * 120 + 1024;
        } else if (algo == 2) {
            /* HAT / CRASH — noise through one-pole HP. snap controls HP
             * cutoff (higher = brighter hat), tone unused here but kept for
             * future. pitch_env_target holds the HP coefficient. */
            fixed16 hp = (fixed16)((uint32_t)inst->drum_snap * (FX_ONE * 15 / 16) / 255);
            if (hp < FX_ONE / 16) hp = FX_ONE / 16;
            ch->u.drum.pitch_env_target = hp;
            ttl = (uint32_t)inst->drum_decay * 180 + 1024;
            if (dt == 5) ttl = 90000 + (uint32_t)inst->drum_decay * 400; /* CRASH ~2s+ */
        } else {
            /* CLAP — 3 quick bursts + tail. bq_z2[1] = per-burst length in
             * samples; snap widens the burst spacing. stage starts at 0, runs
             * through 0..3 as the stage counter fires. */
            ch->u.drum.bq_z2[1] = 80 + (inst->drum_snap >> 1); /* ~2..5 ms per burst */
            ch->u.drum.stage = 0;
            ch->u.drum.stage_tick = (uint16_t)ch->u.drum.bq_z2[1];
            ttl = (uint32_t)inst->drum_decay * 160 + 2048;
        }
        /* Clamp ttl to 24-bit. Drum TTL is a hard cap on audible lifetime;
         * the instrument's ADSR shapes amplitude within that window. Typical
         * drum ADSR: a=0, d tuned for punch, s=0, r unused (no note-off). */
        if (ttl > 0xFFFFFFu) ttl = 0xFFFFFFu;
        ch->u.drum.ttl_hi = (uint8_t)(ttl >> 16);
        ch->u.drum.ttl_lo = (uint16_t)(ttl & 0xFFFF);
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
        ch->u.formant.resonance = inst->formant_resonance;
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
#ifndef BIRB_NO_FM
    /* Per-op envelopes need to be released too; otherwise modulators keep
     * sustaining and the sound never decays even after note-off. Editor JS
     * does this in the note==1 handler (`if (C.fmStg[i]) C.fmStg[i] = 4`). */
    if (ch->synth_type == SYNTH_FM) {
        for (int i = 0; i < 4; i++) {
            if (ch->u.fm.op_stage[i] != ENV_OFF) {
                ch->u.fm.op_stage[i] = ENV_RELEASE;
            }
        }
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

#ifndef BIRB_NO_FM
/* Per-op A/D/R envelope ticking + live refresh of op_lvl from instrument.
 * Mirrors the editor's per-tick loop: each op runs an independent A/D/R state
 * machine using the instrument's per-op ADSR values (read live, not cached at
 * trigger), and op_lvl is also refreshed from inst->fm.ops[i].level so editor
 * tweaks while a note is held take effect on the next tick. */
static void fm_op_envelope_tick(birb_channel *ch, birb_instrument *inst) {
    if (!inst) return;
    /* Algorithm + feedback + mod_index are live too (matches editor behaviour). */
    ch->u.fm.algorithm = inst->fm.algorithm;
    /* (feedback / mod_index could also be live-refreshed; conservative: leave
     * them set at trigger time so existing songs don't change pitch.) */
    for (int i = 0; i < 4; i++) {
        birb_fm_op *op = &inst->fm.ops[i];
        /* round (not floor) to match editor Math.round(F*level/255) */
        ch->u.fm.op_lvl[i] = (fixed16)((FX_ONE * (int32_t)op->level + 127) / 255);
        birb_env_stage st = ch->u.fm.op_stage[i];
        /* double en, double increments — the editor accumulates F/(a+1) etc. as
         * float64; a floored integer ramp (or even float32 here) drifts op
         * levels by ~1 ULP that high-feedback FM amplifies into an audible spike
         * on the lowest notes. Keep every step in double to match the editor. */
        double en = ch->u.fm.op_env[i];
        switch (st) {
            case ENV_ATTACK:
                en += (double)FX_ONE / (op->adsr.attack + 1);
                if (en >= (double)FX_ONE) { en = (double)FX_ONE; st = ENV_DECAY; }
                break;
            case ENV_DECAY: {
                /* decay target is floored (editor `(F*os/255)|0`), then approached
                 * with a double step */
                int32_t tgt = FX_ONE * (int32_t)op->adsr.sustain / 255;
                double target = (double)tgt;
                en -= ((double)FX_ONE - target) / (op->adsr.decay + 1);
                if (en <= target) { en = target; st = ENV_SUSTAIN; }
                break;
            }
            case ENV_RELEASE:
                en -= en / (op->adsr.release + 1);
                if (en < 64.0) { en = 0.0; st = ENV_OFF; }
                break;
            case ENV_SUSTAIN:
            case ENV_OFF:
            default:
                break;
        }
        ch->u.fm.op_stage[i] = st;
        ch->u.fm.op_env[i] = en;
    }
}
#endif

#ifndef BIRB_NO_FORMANT
/* Live refresh of formant params from instrument so editor tweaks (vowel A/B,
 * sweep speed, source wave, resonance) take effect on the next tick. Coeffs
 * are recomputed by formant_interp on its normal 32-sample cadence. */
static void formant_live_tick(birb_channel *ch, birb_instrument *inst) {
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
    if (ch->pitch_slide != 0) {
        ch->base_freq += ch->pitch_slide;
        if (ch->base_freq < 1) ch->base_freq = 1;
    }

    /* tone portamento — slide base toward target. MUST run before arp/vibrato:
     * the editor updates C.b here, then derives C.f (and the FM op freqs) from
     * it. Doing arp/vibrato first and then re-assigning freq from the slid base
     * (the old order) discarded vibrato and desynced porta+vibrato FM notes. */
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

    /* vibrato — small sawtooth LFO, matching the editor exactly:
     *   C.f += ((C.vp & 0xFFFF)*4 - 2*F  >> 8) * depth / F
     * The LFO spans only [-512, 511] before the depth scale (the >>8), so the
     * pitch wobble is subtle (< ~2 units). The old code used a full-scale
     * birb_sin_approx (~128x too deep, wrong shape), which turned vibratoed FM
     * notes into audible "wubwub". freq_f keeps the fractional result so the FM
     * op-freq round matches the editor's round(C.f * ratio); basic-wave phase
     * takes the integer part. */
    double freq_f = (double)ch->freq;
    if (ch->vibrato_depth > 0) {
        int32_t lfo = (((int32_t)(ch->vibrato_phase & 0xFFFF) * 4) - (FX_ONE * 2)) >> 8;
        double vib = (double)lfo * (double)ch->vibrato_depth / (double)FX_ONE;
        freq_f += vib;
        ch->freq += (fixed16)vib;
        ch->vibrato_phase += ch->vibrato_speed;
    }

    /* tremolo — same small sawtooth LFO into the mixer's amplitude mod (editor:
     * C.tm = LFO, then en = C.e*(1 + C.tm/F)). Tiny by construction. */
    if (ch->tremolo_depth > 0) {
        int32_t lfo = (((int32_t)(ch->tremolo_phase & 0xFFFF) * 4) - (FX_ONE * 2)) >> 8;
        ch->tremolo_mod = (fixed16)((int64_t)lfo * ch->tremolo_depth / FX_ONE);
        ch->tremolo_phase += ch->tremolo_speed;
    } else {
        ch->tremolo_mod = 0;
    }

    /* envelope */
    envelope_tick(ch);

#ifndef BIRB_NO_FM
    /* FM: derive per-operator frequencies from the current carrier freq.
     * The op_freq array is rebuilt each tick so arpeggio/vibrato/porta/slide
     * all affect the FM note pitch without extra plumbing. We read the ratio
     * live from the instrument (matches editor: `for i: C.fmf[i] = C.f * C.fmR[i]`).
     * Then advance per-op A/D/R envelopes with `fm_op_envelope_tick`. */
    if (ch->synth_type == SYNTH_FM && ch->base_freq > 0
        && song && ch->cur_instrument < BIRB_MAX_INSTRUMENTS) {
        birb_instrument *inst = &song->instruments[ch->cur_instrument];
        for (int i = 0; i < 4; i++) {
            uint32_t ri = inst->fm.ops[i].ratio_i;
            uint32_t rf = inst->fm.ops[i].ratio_f;
            /* round (match editor's Math.round(C.f * C.fmR[i])), computed from
             * the fractional freq_f so vibrato reaches the operators exactly as
             * in the editor. fmR = (ri*16+rf)/16 (exact, /16 is a power of two). */
            double fmR = (double)((ri << 4) | (rf & 0xF)) / 16.0;
            ch->u.fm.op_freq[i] = (fixed16)(freq_f * fmR + 0.5);
        }
        fm_op_envelope_tick(ch, inst);
    }
#endif
#ifndef BIRB_NO_FORMANT
    if (ch->synth_type == SYNTH_FORMANT && song && ch->cur_instrument < BIRB_MAX_INSTRUMENTS) {
        formant_live_tick(ch, &song->instruments[ch->cur_instrument]);
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

        tick_effects(ch, state->song);
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

#ifndef BIRB_NO_REVERB
/* ---------- reverb send bus (see birb_synth.h) ----------
 * Float DSP, active only in the reverb build; the lean build keeps the
 * "no floats / no libm" guarantee. Mirrors the web editor's makeReverb().tick()
 * exactly, operating in the ±1 sample domain. */
static const int birb_rev_comb_len[BIRB_REV_NCOMB] = { 1116, 1188, 1277, 1356 };
static const int birb_rev_ap_len[BIRB_REV_NAP]     = { 556, 441 };

/* Rational tanh approximation — bounded soft saturation, no libm. Matches
 * JS Math.tanh to <2% over the working range (imperceptible on a master
 * saturator). Input clamped to the monotonic ±3 window where it reaches ±1. */
static float birb_soft_sat(float x) {
    if (x < -3.0f) x = -3.0f; else if (x > 3.0f) x = 3.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static float birb_reverb_tick(birb_state *st, float x, float size, float damp, float wet) {
    float fb = 0.7f + 0.28f * size;
    float dc = 0.4f * damp;
    float o = 0.0f;
    for (int k = 0; k < BIRB_REV_NCOMB; k++) {
        int L = birb_rev_comb_len[k];
        int p = st->rev_comb_pos[k];
        float y = st->rev_comb[k][p];
        st->rev_comb_lp[k] = y * (1.0f - dc) + st->rev_comb_lp[k] * dc;
        st->rev_comb[k][p] = x + st->rev_comb_lp[k] * fb;
        st->rev_comb_pos[k] = (p + 1 < L) ? p + 1 : 0;
        o += y;
    }
    o *= (1.0f - fb) * 5.5f;   /* normalize level off feedback + makeup gain */
    for (int k = 0; k < BIRB_REV_NAP; k++) {
        int L = birb_rev_ap_len[k];
        int p = st->rev_ap_pos[k];
        float y = st->rev_ap[k][p];
        float out = -o + y;
        st->rev_ap[k][p] = o + y * 0.5f;
        st->rev_ap_pos[k] = (p + 1 < L) ? p + 1 : 0;
        o = out;
    }
    return o * wet;
}
#endif /* BIRB_NO_REVERB */

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
#ifndef BIRB_NO_REVERB
        float rev_in = 0.0f;   /* summed reverb send, ±1 domain */
#endif
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
#ifndef BIRB_NO_REVERB
            if (ch->reverb_send)
                rev_in += ((float)out / 32768.0f) * (float)ch->reverb_send / 255.0f;
#endif

            /* advance phase — but NOT for drums or samples, which own their
             * phase/position (the kick sweeps ch->phase itself, the snare adds
             * ch->freq itself, samples use sample_pos). Advancing here too would
             * double-step them. Mirrors the editor's `if (C.w!==5 && C.w!==8)`. */
            if (ch->synth_type != SYNTH_DRUM && ch->synth_type != SYNTH_SAMPLE) {
                ch->phase += ch->freq;
                if (ch->phase >= FX_ONE) {
                    ch->phase -= FX_ONE;
                }
            }
        }

#ifndef BIRB_NO_REVERB
        /* reverb send bus + tanh master saturation, in the ±1 domain — matches
         * the web editor. tanh is always applied (as in the editor), so a wet=0
         * song still gets the same gentle master softening. */
        {
            float vf = (float)mix / 32768.0f;
            birb_song *sg = state->song;
            if (sg->rev_wet)
                vf += birb_reverb_tick(state, rev_in,
                                       sg->rev_size / 255.0f,
                                       sg->rev_damp / 255.0f,
                                       sg->rev_wet  / 255.0f);
            vf = birb_soft_sat(vf);
            int32_t o = (int32_t)(vf * 32767.0f);
            if (o > 32767) o = 32767;
            if (o < -32767) o = -32767;
            output[i] = (int16_t)o;
        }
#else
        /* clamp */
        if (mix > 32767) mix = 32767;
        if (mix < -32767) mix = -32767;
        output[i] = (int16_t)mix;
#endif
    }
}

int birb_get_row(birb_state *state) {
    return state->row_out;
}

int birb_get_pattern(birb_state *state) {
    return state->pattern_out;
}
