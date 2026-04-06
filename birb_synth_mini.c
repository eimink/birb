/*
 * birb_synth_mini.c — size-optimized synth for 4K web demos
 * Same core as birb_synth.c but stripped for minimal WASM output.
 * No stored tables, fewer features, tighter code.
 */
#include "birb_synth.h"

/* ---------- sine approximation for vibrato ---------- */
fixed16 birb_sin_approx(fixed16 phase) {
    phase &= FX_MASK;
    fixed16 x;
    if (phase < FX_HALF) x = phase - (FX_ONE / 4);
    else x = (FX_ONE * 3 / 4) - phase;
    x <<= 2;
    fixed16 ax = x < 0 ? -x : x;
    return FX_MUL(x, FX_ONE - ax) * 4 / FX_ONE;
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
    ch->lfsr_count++;
    if (ch->lfsr_count >= ch->lfsr_period) {
        ch->lfsr_count = 0;
        uint16_t bit = (ch->lfsr ^ (ch->lfsr >> 1)) & 1;
        ch->lfsr = (ch->lfsr >> 1) | (bit << 14);
    }
    return (ch->lfsr & 1) ? 16383 : -16383;
}

static int16_t generate_sample(birb_channel *ch) {
    switch (ch->waveform) {
        case WAVE_PULSE:    return gen_pulse(ch->phase, ch->duty);
        case WAVE_TRIANGLE: return gen_triangle(ch->phase);
        case WAVE_SAWTOOTH: return gen_sawtooth(ch->phase);
        case WAVE_NOISE:    return gen_noise(ch);
        default:            return gen_triangle(ch->phase); /* sine→tri fallback */
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

static void trigger_note(birb_channel *ch, uint8_t note, birb_instrument *inst) {
    int semi = note - BIRB_NOTE_C0;
    ch->base_note = (uint8_t)semi;
    ch->base_freq = note_to_freq(semi);
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
    ch->arp_tick = 0;
    ch->pitch_slide = 0;
    if (inst->waveform == WAVE_NOISE) {
        ch->lfsr = 0x7FFF;
        ch->lfsr_count = 0;
        ch->lfsr_period = (uint16_t)(256 >> (semi / 12));
        if (ch->lfsr_period < 1) ch->lfsr_period = 1;
    }
}

/* ---------- tick effects (streamlined) ---------- */

static void tick_effects(birb_channel *ch) {
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

    envelope_tick(ch);
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
        } else if (r->note >= BIRB_NOTE_C0) {
            if (is_tone_porta) {
                ch->porta_target = note_to_freq(r->note - BIRB_NOTE_C0);
            } else {
                uint8_t inst_idx = r->instrument;
                if (inst_idx == 0xFF) inst_idx = ch->cur_instrument;
                if (inst_idx < song->num_instruments) {
                    ch->cur_instrument = inst_idx;
                    trigger_note(ch, r->note, &song->instruments[inst_idx]);
                }
            }
        }

        if (r->volume) ch->row_vol = r->volume;
        if (r->effect) process_effects(ch, r->effect, r->param);
    }
}

static void advance_tick(birb_state *state) {
    birb_song *song = state->song;
    state->current_tick++;
    if (state->current_tick >= song->ticks_per_row) {
        state->current_tick = 0;
        state->current_row++;
        int pat_len = song->pattern_lengths[song->order[state->order_pos][0]];
        if (!pat_len) pat_len = BIRB_MAX_ROWS;
        if (state->current_row >= pat_len) {
            state->current_row = 0;
            state->order_pos++;
            if (state->order_pos >= song->order_length) state->order_pos = 0;
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
                trigger_note(ch, ch->delayed_note, &state->song->instruments[ch->delayed_inst]);
            }
            ch->note_delay_tick = 0;
        }
        if (ch->note_cut_tick > 0 && tick == ch->note_cut_tick) {
            ch->env_level = 0; ch->env_stage = ENV_OFF;
        }
        if (ch->retrig_interval > 0 && tick > 0 && (tick % ch->retrig_interval) == 0) {
            ch->phase = 0; ch->env_stage = ENV_ATTACK; ch->env_level = 0;
            if (ch->waveform == WAVE_NOISE) { ch->lfsr = 0x7FFF; ch->lfsr_count = 0; }
        }
        tick_effects(ch);
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
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
        state->channels[c].lfsr = 0x7FFF;
        state->channels[c].lfsr_period = 16;
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
            int32_t s = generate_sample(ch);
            mix += (s * FX_TO_INT(ch->env_level * 256)) >> 8;
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
