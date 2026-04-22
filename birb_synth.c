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
    ch->lfsr_count++;
    if (ch->lfsr_count >= ch->lfsr_period) {
        ch->lfsr_count = 0;
        /* Galois LFSR, 15-bit (like NES) */
        uint16_t bit = (ch->lfsr ^ (ch->lfsr >> 1)) & 1;
        ch->lfsr = (ch->lfsr >> 1) | (bit << 14);
    }
    return (ch->lfsr & 1) ? 16383 : -16383;
}

static int16_t gen_sine(fixed16 phase) {
    fixed16 s = birb_sin_approx(phase);
    return (int16_t)((int32_t)s * 32767 / FX_ONE);
}

static int16_t gen_sample_playback(birb_channel *ch, birb_song *song) {
    if (!ch->sample_active || ch->sample_idx >= song->num_samples) return 0;
    birb_sample_meta *m = &song->samples[ch->sample_idx];
    uint32_t pos = ch->sample_pos >> FX_SHIFT;
    if (pos >= m->length) {
        if (m->loop_start != 0xFFFFFFFFu && m->loop_end > m->loop_start) {
            /* wrap back into loop region */
            uint32_t loop_len = m->loop_end - m->loop_start;
            pos = m->loop_start + ((pos - m->loop_start) % loop_len);
            ch->sample_pos = ((uint32_t)pos << FX_SHIFT) | (ch->sample_pos & FX_MASK);
        } else {
            ch->sample_active = 0;
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
    fixed16 frac = ch->sample_pos & FX_MASK;
    int32_t out = s0 + ((int32_t)(s1 - s0) * frac >> FX_SHIFT);
    ch->sample_pos += ch->sample_speed;
    return (int16_t)out;
}

static int16_t generate_sample(birb_channel *ch, birb_song *song) {
    switch (ch->waveform) {
        case WAVE_PULSE:    return gen_pulse(ch->phase, ch->duty);
        case WAVE_TRIANGLE: return gen_triangle(ch->phase);
        case WAVE_SAWTOOTH: return gen_sawtooth(ch->phase);
        case WAVE_NOISE:    return gen_noise(ch);
        case WAVE_SINE:     return gen_sine(ch->phase);
        case WAVE_SAMPLE:   return gen_sample_playback(ch, song);
        default:            return 0;
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
    int semitone = note - BIRB_NOTE_C0;
    ch->base_note = (uint8_t)semitone;
    ch->base_freq = note_to_freq(semitone);
    ch->freq = ch->base_freq;
    ch->phase = 0;
    ch->waveform = inst->waveform;
    ch->duty = inst->duty;
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
    /* noise init */
    if (inst->waveform == WAVE_NOISE) {
        ch->lfsr = 0x7FFF;
        ch->lfsr_count = 0;
        /* higher notes = shorter period = higher pitch noise */
        ch->lfsr_period = (uint16_t)(256 >> (semitone / 12));
        if (ch->lfsr_period < 1) ch->lfsr_period = 1;
    }
    /* sample init */
    ch->sample_active = 0;
    if (inst->waveform == WAVE_SAMPLE && song && inst->sample_idx < song->num_samples) {
        birb_sample_meta *m = &song->samples[inst->sample_idx];
        ch->sample_idx = inst->sample_idx;
        ch->sample_pos = 0;
        /* Playback speed = note_freq / base_note_freq, in 16.16 */
        int base = m->base_note;
        if (base > 95) base = 95;
        fixed16 note_f = note_to_freq(semitone);
        fixed16 base_f = note_to_freq(base);
        if (base_f > 0) {
            ch->sample_speed = (uint32_t)(((uint64_t)note_f << FX_SHIFT) / base_f);
        } else {
            ch->sample_speed = FX_ONE;
        }
        ch->sample_active = 1;
    }
}

static void release_note(birb_channel *ch) {
    if (ch->env_stage != ENV_OFF) {
        ch->env_stage = ENV_RELEASE;
    }
}

/* ---------- effect processing ---------- */

static void process_effects(birb_channel *ch, uint8_t effect, uint8_t param) {
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
            process_effects(ch, r->effect, r->param);
        }
    }
}

static void advance_tick(birb_state *state) {
    birb_song *song = state->song;
    state->current_tick++;

    if (state->current_tick >= song->ticks_per_row) {
        state->current_tick = 0;
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
            if (ch->waveform == WAVE_NOISE) {
                ch->lfsr = 0x7FFF;
                ch->lfsr_count = 0;
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

    /* init noise LFSRs */
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
        state->channels[c].lfsr = 0x7FFF;
        state->channels[c].lfsr_period = 16;
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
            /* apply envelope, instrument volume, and row volume */
            int32_t vol = ch->volume ? ch->volume : 255;
            int32_t rvol = ch->row_vol;
            int32_t out = ((int32_t)sample * FX_TO_INT(ch->env_level * 256)) >> 8;
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
