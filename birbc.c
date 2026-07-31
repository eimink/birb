/*
 * birbc.c — birb song compiler
 *
 * Parses .birb text files, outputs:
 *   - .bin  binary (planar layout, Brotli-friendly)
 *   - .h    C header with embedded byte array
 *
 * Text format:
 *
 *   @bpm 140
 *   @ticks 6
 *
 *   @inst 0 pulse50  A:1 D:6 S:160 R:8
 *   @inst 1 tri      A:0 D:4 S:200 R:4
 *   @inst 2 saw      A:8 D:4 S:140 R:12
 *   @inst 3 sine     A:0 D:8 S:0   R:0   PE:-12,6
 *   @inst 4 noise    A:0 D:5 S:0   R:0
 *
 *   @order
 *   0 0 0 0
 *   1 1 1 1
 *
 *   @pattern 0 16
 *   C-4 00 --- | --- -- --- | E-3 02 --- | C-3 03 ---
 *   --- -- --- | --- -- --- | --- -- --- | --- -- ---
 *
 * Cell format: NNN II EFF
 *   NNN: C-4, C#4, D-4, ..., OFF, ---
 *   II:  instrument hex 00-0F, or -- for none
 *   EFF: effect letter + 2 hex param, or --- for none
 *     0xx = arpeggio
 *     1xx = pitch up
 *     2xx = pitch down
 *     3xx = vibrato
 *     4xx = duty sweep
 *     5xx = volume
 *     6xx = pitch set
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "birb_synth.h"
#include "birb_format.h"

#define MAX_LINE 1024

/* ---------- note parsing ---------- */

static const char *note_names[] = {
    "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"
};

static int parse_note(const char *s) {
    if (s[0] == '-' && s[1] == '-' && s[2] == '-') return BIRB_NOTE_EMPTY;
    if (s[0] == 'O' && s[1] == 'F' && s[2] == 'F') return BIRB_NOTE_OFF;

    int semi = -1;
    for (int i = 0; i < 12; i++) {
        if (s[0] == note_names[i][0] && s[1] == note_names[i][1]) {
            semi = i;
            break;
        }
    }
    if (semi < 0) return -1;

    int oct = s[2] - '0';
    if (oct < 0 || oct > 7) return -1;

    return BIRB_NOTE_C0 + oct * 12 + semi;
}

/* ---------- hex parsing ---------- */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex2(const char *s) {
    int h = hexval(s[0]);
    int l = hexval(s[1]);
    if (h < 0 || l < 0) return -1;
    return (h << 4) | l;
}

/* ---------- instrument waveform parsing ---------- */

typedef struct {
    const char *name;
    birb_wave wave;
    uint8_t duty_code;
} wave_name;

static const wave_name wave_names[] = {
    { "pulse12",  WAVE_PULSE,    BIRB_DUTY_12 },
    { "pulse25",  WAVE_PULSE,    BIRB_DUTY_25 },
    { "pulse50",  WAVE_PULSE,    BIRB_DUTY_50 },
    { "pulse75",  WAVE_PULSE,    BIRB_DUTY_75 },
    { "pulse",    WAVE_PULSE,    BIRB_DUTY_50 },
    { "tri",      WAVE_TRIANGLE, 0 },
    { "triangle", WAVE_TRIANGLE, 0 },
    { "saw",      WAVE_SAWTOOTH, 0 },
    { "sawtooth", WAVE_SAWTOOTH, 0 },
    { "noise",    WAVE_NOISE,    0 },
    { "sine",     WAVE_SINE,     0 },
    { NULL, 0, 0 }
};

/* ---------- skip whitespace ---------- */

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static const char *skip_to_next(const char *s) {
    /* skip whitespace and optional | separator */
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '|') s++;
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ---------- parse one cell: "C-4 00 A37" or "--- -- ---" ---------- */

static int parse_cell(const char **cursor, birb_row *row) {
    const char *s = *cursor;
    s = skip_to_next(s);

    if (*s == '\0' || *s == '\n' || *s == '#') return -1;

    /* note (3 chars) */
    if (s[0] == '\0' || s[1] == '\0' || s[2] == '\0') return -1;
    int note = parse_note(s);
    if (note < 0) {
        fprintf(stderr, "Error: invalid note '%.3s'\n", s);
        return -1;
    }
    row->note = (uint8_t)note;
    s += 3;
    s = skip_ws(s);

    /* instrument (2 chars: hex or --) */
    if (s[0] == '-' && s[1] == '-') {
        row->instrument = 0xFF;
        s += 2;
    } else {
        int inst = parse_hex2(s);
        if (inst < 0) {
            fprintf(stderr, "Error: invalid instrument '%.2s'\n", s);
            return -1;
        }
        row->instrument = (uint8_t)inst;
        s += 2;
    }
    s = skip_ws(s);

    /* volume (2 chars: hex or --) — optional column, detect by peeking ahead.
     * Old format: inst effect (3 chars). New format: inst vol(2) effect(3).
     * If next 3 chars look like an effect (--- or digit+hex+hex), skip volume. */
    {
        const char *peek = s;
        int is_effect_next = (peek[0] == '-' && peek[1] == '-' && peek[2] == '-');
        if (!is_effect_next && hexval(peek[0]) >= 0 && hexval(peek[1]) >= 0 && hexval(peek[2]) >= 0)
            is_effect_next = 1;
        if (is_effect_next) {
            row->volume = BIRB_VOL_NONE; /* no volume column in old format */
        } else if (s[0] == '-' && s[1] == '-') {
            row->volume = BIRB_VOL_NONE;
            s += 2;
            s = skip_ws(s);
        } else {
            int vol = parse_hex2(s);
            if (vol >= 0) { row->volume = (uint8_t)vol; s += 2; }
            else row->volume = BIRB_VOL_NONE;
            s = skip_ws(s);
        }
    }

    /* effect (3 chars: Epp or ---) */
    if (s[0] == '-' && s[1] == '-' && s[2] == '-') {
        row->effect = FX_NONE;
        row->param = 0;
        s += 3;
    } else {
        int fx_type = hexval(s[0]);
        int param = parse_hex2(s + 1);
        if (fx_type < 0 || param < 0) {
            fprintf(stderr, "Error: invalid effect '%.3s'\n", s);
            return -1;
        }
        row->effect = (uint8_t)fx_type;
        row->param = (uint8_t)param;
        s += 3;
    }

    *cursor = s;
    return 0;
}

/* ---------- parse instrument line ---------- */
/* @inst 0 pulse50 A:1 D:6 S:160 R:8 PE:-12,6 ARP:4,7 */

static int parse_instrument(const char *line, birb_song *song) {
    int idx;
    char wavename[32];
    if (sscanf(line, "%d %31s", &idx, wavename) != 2) return -1;
    if (idx < 0 || idx >= BIRB_MAX_INSTRUMENTS) return -1;
    if (idx >= song->num_instruments) song->num_instruments = (uint8_t)(idx + 1);

    birb_instrument *inst = &song->instruments[idx];
    memset(inst, 0, sizeof(*inst));
    inst->volume = 255; /* default full volume */

    /* FM/KS/DRUM/FORMANT use dedicated keywords in place of a waveform name. */
    int is_fm = (strcmp(wavename, "fm") == 0);
    int is_ks = (strcmp(wavename, "ks") == 0);
    int is_drum = (strcmp(wavename, "drum") == 0);
    int is_formant = (strcmp(wavename, "formant") == 0);

    if (is_fm) {
        inst->synth_type = SYNTH_FM;
        inst->waveform = WAVE_SINE; /* FM carrier output is conceptually sine */
        /* defaults: 2-op, ratio 1:1, full levels, no feedback */
        inst->fm.num_ops = 2;
        inst->fm.algorithm = 0;
        inst->fm.feedback = 0;
        inst->fm.mod_index = 64;
        for (int o = 0; o < 4; o++) {
            inst->fm.ops[o].ratio_i = 1;
            inst->fm.ops[o].ratio_f = 0;
            inst->fm.ops[o].level = 255;
            inst->fm.ops[o].adsr.attack = 0;
            inst->fm.ops[o].adsr.decay = 8;
            inst->fm.ops[o].adsr.sustain = 200;
            inst->fm.ops[o].adsr.release = 8;
        }
    } else if (is_ks) {
        inst->synth_type = SYNTH_KS;
        inst->waveform = WAVE_SAWTOOTH; /* placeholder — not used for KS dispatch */
        inst->ks_damping = 40;          /* sensible default: long-ish sustain */
    } else if (is_drum) {
        inst->synth_type = SYNTH_DRUM;
        inst->waveform = WAVE_SINE;     /* placeholder */
        /* Defaults — overridden by kick/snare/... keyword below. */
        inst->drum_type = 0;
        inst->drum_tune = 0;
        inst->drum_decay = 180;
        inst->drum_tone = 128;
        inst->drum_snap = 128;
        /* Default amp envelope for drums: instant attack, quick decay to
         * zero. Explicit adsr=... in text overrides. Without this drums
         * default to ADSR=0,0,0,0 which makes them silent. */
        inst->envelope.attack  = 0;
        inst->envelope.decay   = 20;
        inst->envelope.sustain = 0;
        inst->envelope.release = 8;
    } else if (is_formant) {
        inst->synth_type = SYNTH_FORMANT;
        inst->waveform = WAVE_SAWTOOTH; /* placeholder */
        inst->formant_source_wave = WAVE_SAWTOOTH;
        inst->formant_duty = BIRB_DUTY_50;
        inst->formant_vowel_a = 0;  /* A */
        inst->formant_vowel_b = 3;  /* O */
        inst->formant_sweep_speed = 32;
        inst->formant_resonance = 128;
    } else {
        /* find waveform */
        int found = 0;
        for (const wave_name *w = wave_names; w->name; w++) {
            if (strcmp(wavename, w->name) == 0) {
                inst->waveform = w->wave;
                inst->duty = birb_duty_decode(w->duty_code);
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Error: unknown waveform '%s'\n", wavename);
            return -1;
        }
        inst->synth_type = (inst->waveform == WAVE_SAMPLE) ? SYNTH_SAMPLE : SYNTH_BASIC;
    }

    /* parse key:value pairs */
    const char *s = strstr(line, wavename);
    if (s) s += strlen(wavename);
    else return -1;

    while (s && *s) {
        s = skip_ws(s);
        if (*s == '\0' || *s == '\n' || *s == '#') break;

        if (is_fm && s[0] == 'r' && s[1] == 'a' && s[2] == 't' && s[3] == 'i' && s[4] == 'o') {
            /* ratioN=X.Y — N is op index (1..4) */
            int op = s[5] - '1';
            if (op >= 0 && op < 4 && s[6] == '=') {
                float r = (float)atof(s + 7);
                if (r < 0) r = 0;
                inst->fm.ops[op].ratio_i = (uint8_t)((int)r & 0xFF);
                inst->fm.ops[op].ratio_f = (uint8_t)(((r - (int)r) * 16.0f) + 0.5f);
            }
        } else if (is_fm && s[0] == 'l' && s[1] == 'e' && s[2] == 'v' && s[3] == 'e' && s[4] == 'l') {
            int op = s[5] - '1';
            if (op >= 0 && op < 4 && s[6] == '=')
                inst->fm.ops[op].level = (uint8_t)atoi(s + 7);
        } else if (is_fm && s[0] == 'a' && s[1] == 'd' && s[2] == 's' && s[3] == 'r') {
            /* adsrN=A,D,S,R */
            int op = s[4] - '1';
            if (op >= 0 && op < 4 && s[5] == '=') {
                int a=0,d=0,ss=0,r=0;
                sscanf(s + 6, "%d,%d,%d,%d", &a, &d, &ss, &r);
                inst->fm.ops[op].adsr.attack  = (uint8_t)a;
                inst->fm.ops[op].adsr.decay   = (uint8_t)d;
                inst->fm.ops[op].adsr.sustain = (uint8_t)ss;
                inst->fm.ops[op].adsr.release = (uint8_t)r;
            }
        } else if (is_ks && strncmp(s, "damping=", 8) == 0) {
            inst->ks_damping = (uint8_t)atoi(s + 8);
        } else if (is_drum && strncmp(s, "tune=", 5) == 0) {
            inst->drum_tune = (int8_t)atoi(s + 5);
        } else if (is_drum && strncmp(s, "decay=", 6) == 0) {
            inst->drum_decay = (uint8_t)atoi(s + 6);
        } else if (is_drum && strncmp(s, "tone=", 5) == 0) {
            inst->drum_tone = (uint8_t)atoi(s + 5);
        } else if (is_drum && strncmp(s, "snap=", 5) == 0) {
            inst->drum_snap = (uint8_t)atoi(s + 5);
        } else if (is_drum && strncmp(s, "volume=", 7) == 0) {
            inst->volume = (uint8_t)atoi(s + 7);
        } else if (is_drum) {
            /* drum type keyword (kick/snare/hat/clap/tom/crash) — matches
             * the token without needing an '=' sign. */
            static const struct { const char *k; uint8_t v; } dmap[] = {
                {"kick", 0}, {"snare", 1}, {"hat", 2}, {"clap", 3},
                {"tom", 4}, {"crash", 5}, {NULL, 0}
            };
            for (int i = 0; dmap[i].k; i++) {
                size_t kl = strlen(dmap[i].k);
                if (strncmp(s, dmap[i].k, kl) == 0 &&
                    (s[kl] == ' ' || s[kl] == '\t' || s[kl] == '\0' || s[kl] == '\n')) {
                    inst->drum_type = dmap[i].v;
                    break;
                }
            }
        } else if (is_formant && strncmp(s, "source=", 7) == 0) {
            const char *v = s + 7;
            if      (strncmp(v, "pulse", 5) == 0) inst->formant_source_wave = WAVE_PULSE;
            else if (strncmp(v, "saw",   3) == 0) inst->formant_source_wave = WAVE_SAWTOOTH;
            else if (strncmp(v, "noise", 5) == 0) inst->formant_source_wave = WAVE_NOISE;
        } else if (is_formant && strncmp(s, "duty=", 5) == 0) {
            int d = atoi(s + 5);
            /* Accept either a byte 0..255 (mapped to coarse code) or an explicit 0..3 code. */
            if (d < 0) d = 0;
            if (d > 255) d = 255;
            inst->formant_duty = (d < 4) ? (uint8_t)d
                               : (d < 64) ? BIRB_DUTY_12
                               : (d < 96) ? BIRB_DUTY_25
                               : (d < 160) ? BIRB_DUTY_50
                               : BIRB_DUTY_75;
        } else if (is_formant && strncmp(s, "vowela=", 7) == 0) {
            char c = s[7]; if (c >= 'a' && c <= 'z') c -= 32;
            switch (c) { case 'A': inst->formant_vowel_a = 0; break;
                         case 'E': inst->formant_vowel_a = 1; break;
                         case 'I': inst->formant_vowel_a = 2; break;
                         case 'O': inst->formant_vowel_a = 3; break;
                         case 'U': inst->formant_vowel_a = 4; break;
                         default:  inst->formant_vowel_a = (uint8_t)atoi(s + 7); }
        } else if (is_formant && strncmp(s, "vowelb=", 7) == 0) {
            char c = s[7]; if (c >= 'a' && c <= 'z') c -= 32;
            switch (c) { case 'A': inst->formant_vowel_b = 0; break;
                         case 'E': inst->formant_vowel_b = 1; break;
                         case 'I': inst->formant_vowel_b = 2; break;
                         case 'O': inst->formant_vowel_b = 3; break;
                         case 'U': inst->formant_vowel_b = 4; break;
                         default:  inst->formant_vowel_b = (uint8_t)atoi(s + 7); }
        } else if (is_formant && strncmp(s, "sweep=", 6) == 0) {
            inst->formant_sweep_speed = (uint8_t)atoi(s + 6);
        } else if (is_formant && strncmp(s, "resonance=", 10) == 0) {
            inst->formant_resonance = (uint8_t)atoi(s + 10);
        } else if (is_formant && strncmp(s, "adsr=", 5) == 0) {
            int a=0,d=0,ss=0,r=0;
            sscanf(s + 5, "%d,%d,%d,%d", &a, &d, &ss, &r);
            inst->envelope.attack  = (uint8_t)a;
            inst->envelope.decay   = (uint8_t)d;
            inst->envelope.sustain = (uint8_t)ss;
            inst->envelope.release = (uint8_t)r;
        } else if (is_formant && strncmp(s, "volume=", 7) == 0) {
            inst->volume = (uint8_t)atoi(s + 7);
        } else if (is_fm && strncmp(s, "feedback=", 9) == 0) {
            inst->fm.feedback = (uint8_t)atoi(s + 9);
        } else if (is_fm && strncmp(s, "modidx=", 7) == 0) {
            inst->fm.mod_index = (uint8_t)atoi(s + 7);
        } else if (is_fm && strncmp(s, "algo=", 5) == 0) {
            inst->fm.algorithm = (uint8_t)atoi(s + 5);
        } else if (is_fm && strncmp(s, "nops=", 5) == 0) {
            int n = atoi(s + 5);
            if (n < 2) n = 2; if (n > 4) n = 4;
            inst->fm.num_ops = (uint8_t)n;
        } else if (s[0] == 'A' && s[1] == ':') {
            inst->envelope.attack = (uint8_t)atoi(s + 2);
        } else if (s[0] == 'D' && s[1] == ':') {
            inst->envelope.decay = (uint8_t)atoi(s + 2);
        } else if (s[0] == 'S' && s[1] == ':') {
            inst->envelope.sustain = (uint8_t)atoi(s + 2);
        } else if (s[0] == 'R' && s[1] == ':') {
            inst->envelope.release = (uint8_t)atoi(s + 2);
        } else if (s[0] == 'P' && s[1] == 'E' && s[2] == ':') {
            /* PE:-12,6 */
            int pe_val = 0, pe_len = 0;
            sscanf(s + 3, "%d,%d", &pe_val, &pe_len);
            inst->pitch_env = (int8_t)pe_val;
            inst->pitch_env_len = (uint8_t)pe_len;
        } else if (s[0] == 'A' && s[1] == 'R' && s[2] == 'P' && s[3] == ':') {
            /* ARP:4,7 */
            int a1 = 0, a2 = 0;
            sscanf(s + 4, "%d,%d", &a1, &a2);
            inst->arp_note1 = (uint8_t)a1;
            inst->arp_note2 = (uint8_t)a2;
        } else if (s[0] == 'V' && s[1] == ':') {
            inst->volume = (uint8_t)atoi(s + 2);
#ifndef BIRB_NO_REVERB
        } else if (strncmp(s, "rev=", 4) == 0) {
            inst->reverb_send = (uint8_t)atoi(s + 4);
#endif
        }

        /* advance to next token */
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++;
    }

    return 0;
}

/* ---------- main parser ---------- */

static int parse_birb_file(const char *filename, birb_song *song) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", filename);
        return -1;
    }

    memset(song, 0, sizeof(*song));
    song->bpm = 125;
    song->ticks_per_row = 6;

    char line[MAX_LINE];
    int in_order = 0;
    int in_pattern = -1;
    int pattern_row = 0;
    int pattern_max_rows = BIRB_MAX_ROWS;
    int line_num = 0;

    while (fgets(line, sizeof(line), f)) {
        line_num++;
        /* strip newline */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        const char *s = skip_ws(line);

        /* empty lines and comments */
        if (*s == '\0' || *s == '#') {
            /* blank line ends order block */
            if (in_order && *s == '\0') in_order = 0;
            continue;
        }

        /* directives */
        if (*s == '@') {
            in_order = 0;
            in_pattern = -1;
            s++;

            if (strncmp(s, "bpm ", 4) == 0) {
                song->bpm = (uint8_t)atoi(s + 4);
            } else if (strncmp(s, "ticks ", 6) == 0) {
                song->ticks_per_row = (uint8_t)atoi(s + 6);
#ifndef BIRB_NO_REVERB
            } else if (strncmp(s, "rev ", 4) == 0) {
                /* @rev <size> <damp> <wet>  — global reverb bus, each 0-255 */
                int rs = 0, rd = 0, rw = 0;
                sscanf(s + 4, "%d %d %d", &rs, &rd, &rw);
                song->rev_size = (uint8_t)rs;
                song->rev_damp = (uint8_t)rd;
                song->rev_wet  = (uint8_t)rw;
#endif
            } else if (strncmp(s, "inst ", 5) == 0) {
                if (parse_instrument(s + 5, song) < 0) {
                    fprintf(stderr, "Error on line %d: bad instrument\n", line_num);
                    fclose(f);
                    return -1;
                }
            } else if (strncmp(s, "order", 5) == 0) {
                in_order = 1;
                song->order_length = 0;
            } else if (strncmp(s, "pattern ", 8) == 0) {
                int idx, nrows;
                if (sscanf(s + 8, "%d %d", &idx, &nrows) < 1) {
                    fprintf(stderr, "Error on line %d: bad pattern header\n", line_num);
                    fclose(f);
                    return -1;
                }
                if (sscanf(s + 8, "%d %d", &idx, &nrows) < 2) {
                    nrows = BIRB_MAX_ROWS;
                }
                if (idx < 0 || idx >= BIRB_MAX_PATTERNS) {
                    fprintf(stderr, "Error on line %d: pattern index %d out of range\n", line_num, idx);
                    fclose(f);
                    return -1;
                }
                in_pattern = idx;
                pattern_row = 0;
                pattern_max_rows = nrows;
                song->pattern_lengths[idx] = (uint8_t)nrows;
                if (idx >= song->num_patterns) song->num_patterns = (uint8_t)(idx + 1);
            } else {
                fprintf(stderr, "Warning on line %d: unknown directive '@%s'\n", line_num, s);
            }
            continue;
        }

        /* order data */
        if (in_order) {
            if (song->order_length < BIRB_MAX_ORDER) {
                int p0, p1, p2, p3;
                int n = sscanf(s, "%d %d %d %d", &p0, &p1, &p2, &p3);
                if (n >= 1) {
                    int oi = song->order_length;
                    song->order[oi][0] = (uint8_t)p0;
                    song->order[oi][1] = (n >= 2) ? (uint8_t)p1 : (uint8_t)p0;
                    song->order[oi][2] = (n >= 3) ? (uint8_t)p2 : (uint8_t)p0;
                    song->order[oi][3] = (n >= 4) ? (uint8_t)p3 : (uint8_t)p0;
                    song->order_length++;
                }
            }
            continue;
        }

        /* pattern row data */
        if (in_pattern >= 0) {
            if (pattern_row >= pattern_max_rows) {
                fprintf(stderr, "Warning on line %d: extra row in pattern %d, ignoring\n", line_num, in_pattern);
                continue;
            }

            const char *cursor = s;
            for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
                birb_row row = { BIRB_NOTE_EMPTY, 0xFF, 0, FX_NONE, 0 };
                if (parse_cell(&cursor, &row) == 0) {
                    song->patterns[in_pattern][pattern_row][c] = row;
                }
            }
            pattern_row++;
            continue;
        }
    }

    fclose(f);
    printf("Parsed: bpm=%d ticks=%d instruments=%d patterns=%d order=%d\n",
           song->bpm, song->ticks_per_row, song->num_instruments,
           song->num_patterns, song->order_length);
    return 0;
}

/* ---------- binary writer ---------- */


/* Set by --smol ("smol birb"). Mirrors the editor toggle: drop engine code the
 * song provably never reaches (one pattern length, one instrument per channel)
 * and trade sound for size (no per-instrument volume, no master limiter).
 * Implies --no-master. Transforms the song cannot take stay off. */
static int birb_smol = 0;

/* Set by --locked-c (experimental): emit the full-feature locked-down C
 * player. Independent of --smol/--smol-c, which are untouched. */
static int birb_locked = 0;

/* Set by --no-master. Drops the soft saturator, limiter and ceiling from the
 * emitted JS, replacing them with a hard clamp. Worth ~200 raw bytes but it
 * ALTERS THE OUTPUT: there is no setting at which the chain is a no-op, since
 * SS() and the ceiling divide always apply — even at unity gain with the
 * threshold maxed, small signals come out ~1.29x hotter. Opt-in only. */
static int birb_no_master = 0;

/* ---------- baked formant coefficients ----------
 * Computed here, in double precision, exactly as the editor's JS does
 * (2*PI*f/44100, Math.sin/Math.cos), and written into the song. The runtime
 * engines then need no trig at all: the C engine used to approximate omega
 * with a documented 1.7% error while the JS engines used exact Math.sin, so
 * identical instruments produced different filters. Baking removes both the
 * libm dependency and the divergence. */
static const uint16_t bc_formant_freqs[5][3] = {
    { 730, 1090, 2440 }, { 530, 1840, 2480 }, { 270, 2290, 3010 },
    { 570,  840, 2410 }, { 300,  870, 2240 },
};
static const double bc_formant_gains[3] = { 1.0, 0.7, 0.4 };

static void bc_formant_coeffs(int vowel, double q, int32_t out[3][3]) {
    if (vowel < 0 || vowel > 4) vowel = 0;
    for (int i = 0; i < 3; i++) {
        double om = 2.0 * M_PI * bc_formant_freqs[vowel][i] / 44100.0;
        double sn = sin(om), cs = cos(om);
        double al = sn / (2.0 * q), iv = 1.0 / (1.0 + al);
        out[i][0] = (int32_t)(al * iv * bc_formant_gains[i] * 65536.0);
        out[i][1] = (int32_t)(-2.0 * cs * iv * 65536.0);
        out[i][2] = (int32_t)((1.0 - al) * iv * 65536.0);
    }
}
static double bc_formant_q(uint8_t res) { return 2.0 + (res / 255.0) * 30.0; }

static int write_binary(const char *filename, birb_song *song) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot write '%s'\n", filename);
        return -1;
    }

    /* header — channel count lives in the high 3 bits of the ticks_per_row
     * byte. For Phase 1 we always write BIRB_NUM_CHANNELS (which the current
     * build is compiled for and the editor writes 4-channel .bsb into). */
    uint8_t tpr_byte = (uint8_t)((song->ticks_per_row & BIRB_TPR_MASK) |
                                 (BIRB_CHANNELS_ENCODE(BIRB_NUM_CHANNELS) << BIRB_CHANNELS_SHIFT));
    uint8_t hdr[8] = {
        BIRB_MAGIC_0, BIRB_MAGIC_1, BIRB_MAGIC_2, BIRB_MAGIC_3,
        song->bpm, tpr_byte, song->num_instruments, song->num_patterns
    };
    fwrite(hdr, 1, 8, f);

    /* order */
    fwrite(&song->order_length, 1, 1, f);
    for (int i = 0; i < song->order_length; i++) {
        fwrite(song->order[i], 1, BIRB_NUM_CHANNELS, f);
    }

    /* instruments */
    for (int i = 0; i < song->num_instruments; i++) {
        birb_instrument *inst = &song->instruments[i];
        uint8_t buf[BIRB_INST_SIZE] = {
            (uint8_t)inst->waveform,
            birb_duty_encode(inst->duty),
            inst->envelope.attack,
            inst->envelope.decay,
            inst->envelope.sustain,
            inst->envelope.release,
            (uint8_t)inst->pitch_env,
            inst->pitch_env_len,
            inst->arp_note1,
            inst->arp_note2,
            inst->volume,
            inst->sample_idx
        };
        fwrite(buf, 1, BIRB_INST_SIZE, f);
    }

    /* Pattern lengths */
    int total_pattern_bytes = 0;
    for (int p = 0; p < song->num_patterns; p++) {
        uint8_t nrows = song->pattern_lengths[p];
        if (nrows == 0) nrows = BIRB_MAX_ROWS;
        fwrite(&nrows, 1, 1, f);
        total_pattern_bytes++;
    }

    /* Determine which planes are all-default (can be skipped) */
    static const uint8_t plane_defaults[5] = { 0, 0xFF, 0, 0, 0 };
    uint8_t plane_flags = 0;
    for (int pl = 0; pl < 5; pl++) {
        int empty = 1;
        for (int p = 0; p < song->num_patterns && empty; p++) {
            int nrows = song->pattern_lengths[p]; if (!nrows) nrows = BIRB_MAX_ROWS;
            for (int r = 0; r < nrows && empty; r++) {
                for (int c = 0; c < BIRB_NUM_CHANNELS && empty; c++) {
                    uint8_t v = 0;
                    switch (pl) {
                        case 0: v = song->patterns[p][r][c].note; break;
                        case 1: v = song->patterns[p][r][c].instrument; break;
                        case 2: v = song->patterns[p][r][c].volume; break;
                        case 3: v = song->patterns[p][r][c].effect; break;
                        case 4: v = song->patterns[p][r][c].param; break;
                    }
                    if (v != plane_defaults[pl]) empty = 0;
                }
            }
        }
        if (empty) plane_flags |= (1 << pl);
    }
    fwrite(&plane_flags, 1, 1, f);
    total_pattern_bytes++;

    /* Plane data (channel-major, pattern-major) */
    for (int pl = 0; pl < 5; pl++) {
        if (plane_flags & (1 << pl)) continue;
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
            for (int p = 0; p < song->num_patterns; p++) {
                int nrows = song->pattern_lengths[p]; if (!nrows) nrows = BIRB_MAX_ROWS;
                for (int r = 0; r < nrows; r++) {
                    uint8_t v = 0;
                    switch (pl) {
                        case 0: v = song->patterns[p][r][c].note; break;
                        case 1: v = song->patterns[p][r][c].instrument; break;
                        case 2: v = song->patterns[p][r][c].volume; break;
                        case 3: v = song->patterns[p][r][c].effect; break;
                        case 4: v = song->patterns[p][r][c].param; break;
                    }
                    fwrite(&v, 1, 1, f);
                    total_pattern_bytes++;
                }
            }
        }
    }

    /* SMPL section — re-encode sample pool as IMA-ADPCM */
    if (song->num_samples > 0) {
        static const int16_t adpcm_step[89] = {
            7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,
            80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
            494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
            2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,
            9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,
            29794,32767
        };
        static const int8_t adpcm_indexadj[16] = {
            -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8
        };
        fwrite("SMPL", 1, 4, f);
        uint8_t n = song->num_samples;
        fwrite(&n, 1, 1, f);
        for (int s = 0; s < song->num_samples; s++) {
            birb_sample_meta *m = &song->samples[s];
            uint32_t length = m->length;
            uint16_t ls = (m->loop_start == 0xFFFFFFFFu) ? 0xFFFF : (uint16_t)m->loop_start;
            uint16_t le = (uint16_t)m->loop_end;
            int16_t *src = &song->sample_pool[m->offset];
            /* encode */
            int32_t predictor = 0;
            int step_idx = 0;
            uint8_t hdr[10] = {
                (uint8_t)(length & 0xFF), (uint8_t)(length >> 8),
                (uint8_t)(ls & 0xFF), (uint8_t)(ls >> 8),
                (uint8_t)(le & 0xFF), (uint8_t)(le >> 8),
                m->base_note, 0,
                0, 0 /* initial predictor (filled after encoding) */
            };
            long hdr_pos = ftell(f);
            fwrite(hdr, 1, 10, f);

            uint8_t out_byte = 0;
            for (uint32_t i = 0; i < length; i++) {
                int32_t sample = src[i];
                int32_t diff = sample - predictor;
                int step = adpcm_step[step_idx];
                int nibble = 0;
                int sign = 0;
                if (diff < 0) { sign = 8; diff = -diff; }
                if (diff >= step)    { nibble |= 4; diff -= step; }
                if (diff >= step/2)  { nibble |= 2; diff -= step/2; }
                if (diff >= step/4)  { nibble |= 1; }
                nibble |= sign;
                /* reconstruct predictor */
                int d = step >> 3;
                if (nibble & 1) d += step >> 2;
                if (nibble & 2) d += step >> 1;
                if (nibble & 4) d += step;
                if (nibble & 8) d = -d;
                predictor += d;
                if (predictor > 32767) predictor = 32767;
                if (predictor < -32768) predictor = -32768;
                step_idx += adpcm_indexadj[nibble];
                if (step_idx < 0) step_idx = 0;
                if (step_idx > 88) step_idx = 88;
                if (i & 1) {
                    out_byte |= (uint8_t)(nibble << 4);
                    fwrite(&out_byte, 1, 1, f);
                } else {
                    out_byte = (uint8_t)(nibble & 0x0F);
                    if (i == length - 1) fwrite(&out_byte, 1, 1, f);
                }
            }
            /* no initial predictor was stored; decoder starts from 0 with step_idx=0 */
            (void)hdr_pos;
        }
    }

    /* FMIN section — FM instrument params (only when ≥1 FM inst present) */
    int fm_count = 0;
    for (int i = 0; i < song->num_instruments; i++)
        if (song->instruments[i].synth_type == SYNTH_FM) fm_count++;
    if (fm_count > 0) {
        fwrite("FMIN", 1, 4, f);
        uint8_t cnt = (uint8_t)fm_count;
        fwrite(&cnt, 1, 1, f);
        for (int i = 0; i < song->num_instruments; i++) {
            birb_instrument *inst = &song->instruments[i];
            if (inst->synth_type != SYNTH_FM) continue;
            int nops = inst->fm.num_ops ? inst->fm.num_ops : 2;
            if (nops > 4) nops = 4;
            uint8_t hdr[6] = {
                (uint8_t)i, (uint8_t)nops,
                inst->fm.algorithm, inst->fm.feedback,
                inst->fm.mod_index, 0
            };
            fwrite(hdr, 1, 6, f);
            for (int o = 0; o < nops; o++) {
                uint8_t rec[8] = {
                    inst->fm.ops[o].ratio_i, inst->fm.ops[o].ratio_f,
                    inst->fm.ops[o].level,
                    inst->fm.ops[o].adsr.attack, inst->fm.ops[o].adsr.decay,
                    inst->fm.ops[o].adsr.sustain, inst->fm.ops[o].adsr.release,
                    0
                };
                fwrite(rec, 1, 8, f);
            }
        }
    }

    /* KSIN section — Karplus-Strong instrument params (when ≥1 KS inst). */
    int ks_count = 0;
    for (int i = 0; i < song->num_instruments; i++)
        if (song->instruments[i].synth_type == SYNTH_KS) ks_count++;
    if (ks_count > 0) {
        fwrite("KSIN", 1, 4, f);
        uint8_t cnt = (uint8_t)ks_count;
        fwrite(&cnt, 1, 1, f);
        for (int i = 0; i < song->num_instruments; i++) {
            birb_instrument *inst = &song->instruments[i];
            if (inst->synth_type != SYNTH_KS) continue;
            uint8_t rec[2] = { (uint8_t)i, inst->ks_damping };
            fwrite(rec, 1, 2, f);
        }
    }

    /* DRIN section — drum instrument params (when ≥1 DRUM inst). */
    int drum_count = 0;
    for (int i = 0; i < song->num_instruments; i++)
        if (song->instruments[i].synth_type == SYNTH_DRUM) drum_count++;
    if (drum_count > 0) {
        fwrite("DRIN", 1, 4, f);
        uint8_t cnt = (uint8_t)drum_count;
        fwrite(&cnt, 1, 1, f);
        for (int i = 0; i < song->num_instruments; i++) {
            birb_instrument *inst = &song->instruments[i];
            if (inst->synth_type != SYNTH_DRUM) continue;
            uint8_t rec[6] = {
                (uint8_t)i,
                (uint8_t)(inst->drum_type & 0x07),
                (uint8_t)inst->drum_tune,
                inst->drum_decay, inst->drum_tone, inst->drum_snap
            };
            fwrite(rec, 1, 6, f);
        }
    }

    /* FRIN section — formant instrument params (when ≥1 FORMANT inst). */
    int formant_count = 0;
    for (int i = 0; i < song->num_instruments; i++)
        if (song->instruments[i].synth_type == SYNTH_FORMANT) formant_count++;
    if (formant_count > 0) {
        fwrite("FRIN", 1, 4, f);
        uint8_t cnt = (uint8_t)formant_count;
        fwrite(&cnt, 1, 1, f);
        for (int i = 0; i < song->num_instruments; i++) {
            birb_instrument *inst = &song->instruments[i];
            if (inst->synth_type != SYNTH_FORMANT) continue;
            uint8_t rec[7] = {
                (uint8_t)i,
                inst->formant_source_wave,
                inst->formant_duty,
                inst->formant_vowel_a,
                inst->formant_vowel_b,
                inst->formant_sweep_speed,
                inst->formant_resonance
            };
            fwrite(rec, 1, 7, f);
            /* 18 baked coefficients, int32 LE */
            {
                double q = bc_formant_q(inst->formant_resonance);
                int32_t cf[2][3][3];
                bc_formant_coeffs(inst->formant_vowel_a, q, cf[0]);
                bc_formant_coeffs(inst->formant_vowel_b, q, cf[1]);
                for (int v = 0; v < 2; v++)
                    for (int fi = 0; fi < 3; fi++)
                        for (int c = 0; c < 3; c++) {
                            int32_t x = cf[v][fi][c];
                            uint8_t b[4] = { (uint8_t)x, (uint8_t)(x >> 8),
                                             (uint8_t)(x >> 16), (uint8_t)(x >> 24) };
                            fwrite(b, 1, 4, f);
                        }
            }
        }
    }

    /* REVB section — global reverb bus params + per-instrument sends. Written
     * only when reverb is audible (wet > 0 or some instrument sends). Must sit
     * after FRIN and before NAME to match the positional loader order. */
#ifndef BIRB_NO_REVERB
    {
        int send_count = 0;
        for (int i = 0; i < song->num_instruments; i++)
            if (song->instruments[i].reverb_send) send_count++;
        if (song->rev_wet > 0 || send_count > 0) {
            fwrite("REVB", 1, 4, f);
            uint8_t g[3] = { song->rev_size, song->rev_damp, song->rev_wet };
            fwrite(g, 1, 3, f);
            uint8_t cnt = (uint8_t)send_count;
            fwrite(&cnt, 1, 1, f);
            for (int i = 0; i < song->num_instruments; i++) {
                if (!song->instruments[i].reverb_send) continue;
                uint8_t rec[2] = { (uint8_t)i, song->instruments[i].reverb_send };
                fwrite(rec, 1, 2, f);
            }
        }
    }
#endif

    /* MSTR section — master bus + per-instrument dynamics. Written only when
     * the song departs from the defaults, so simple songs cost nothing. Must
     * sit after REVB and before NAME to match the positional loader order. */
#ifndef BIRB_NO_MASTER
    {
        int dyn_count = 0;
        for (int i = 0; i < song->num_instruments; i++)
            if (song->instruments[i].drive || song->instruments[i].duck_send ||
                song->instruments[i].duck_amt) dyn_count++;
        int non_default = (song->master_gain   && song->master_gain   != 128) ||
                          (song->limit_thresh  && song->limit_thresh  != 242) ||
                          (song->limit_release && song->limit_release != 50)  ||
                          (song->duck_release  && song->duck_release  != 120);
        if (non_default || dyn_count > 0) {
            fwrite("MSTR", 1, 4, f);
            uint8_t g[4] = {
                song->master_gain   ? song->master_gain   : 128,
                song->limit_thresh  ? song->limit_thresh  : 242,
                song->limit_release ? song->limit_release : 50,
                song->duck_release  ? song->duck_release  : 120,
            };
            fwrite(g, 1, 4, f);
            uint8_t cnt = (uint8_t)dyn_count;
            fwrite(&cnt, 1, 1, f);
            for (int i = 0; i < song->num_instruments; i++) {
                birb_instrument *in = &song->instruments[i];
                if (!in->drive && !in->duck_send && !in->duck_amt) continue;
                uint8_t rec[4] = { (uint8_t)i, in->drive, in->duck_send, in->duck_amt };
                fwrite(rec, 1, 4, f);
            }
        }
    }
#endif

    /* NAME section — instrument names */
    int has_names = 0;
    for (int i = 0; i < song->num_instruments; i++)
        if (song->instruments[i].name[0]) { has_names = 1; break; }
    if (has_names) {
        fwrite("NAME", 1, 4, f);
        for (int i = 0; i < song->num_instruments; i++) {
            uint8_t slen = (uint8_t)strlen(song->instruments[i].name);
            fwrite(&slen, 1, 1, f);
            if (slen) fwrite(song->instruments[i].name, 1, slen, f);
        }
    }

    long size = ftell(f);
    fclose(f);
    printf("Wrote %s (%ld bytes: 8 hdr + %d order + %d inst + %d patterns)\n",
           filename, size,
           1 + song->order_length * BIRB_NUM_CHANNELS,
           song->num_instruments * BIRB_INST_SIZE,
           total_pattern_bytes);
    return 0;
}

/* ---------- C header writer ---------- */

static int write_c_header(const char *filename, const char *bin_filename) {
    /* read the binary file we just wrote, emit as C array */
    FILE *bf = fopen(bin_filename, "rb");
    if (!bf) {
        fprintf(stderr, "Error: cannot read '%s' for C header export\n", bin_filename);
        return -1;
    }
    fseek(bf, 0, SEEK_END);
    long size = ftell(bf);
    fseek(bf, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    fread(data, 1, (size_t)size, bf);
    fclose(bf);

    FILE *f = fopen(filename, "w");
    if (!f) {
        free(data);
        fprintf(stderr, "Error: cannot write '%s'\n", filename);
        return -1;
    }

    fprintf(f, "/* Generated by birbc — do not edit */\n");
    fprintf(f, "#ifndef BIRB_SONG_DATA_H\n");
    fprintf(f, "#define BIRB_SONG_DATA_H\n\n");
    fprintf(f, "#include <stdint.h>\n\n");
    fprintf(f, "static const uint8_t birb_song_data[] = {\n");

    for (long i = 0; i < size; i++) {
        if (i % 16 == 0) fprintf(f, "    ");
        fprintf(f, "0x%02x", data[i]);
        if (i < size - 1) fprintf(f, ",");
        if (i % 16 == 15 || i == size - 1) fprintf(f, "\n");
    }

    fprintf(f, "};\n\n");
    fprintf(f, "#define BIRB_SONG_DATA_SIZE %ld\n\n", size);
    fprintf(f, "#endif /* BIRB_SONG_DATA_H */\n");

    fclose(f);
    free(data);
    printf("Wrote %s (%ld bytes as C array)\n", filename, size);
    return 0;
}

/* ---------- JS writer for 4K demos ---------- */
/* Emits a single JS file with:
 *   - Pre-parsed song data as JS literals (no runtime parser)
 *   - Inline render-only synth engine
 *   - birb(audioCtx) → {play(), stop(), row, pat}
 */

/* helper: emit a JS array of uint8 values, compact */
static void emit_u8_array(FILE *f, const uint8_t *data, int len) {
    fprintf(f, "[");
    for (int i = 0; i < len; i++) {
        if (i) fprintf(f, ",");
        fprintf(f, "%d", data[i]);
    }
    fprintf(f, "]");
}

/* smol birb ships an event list produced by walking the song here, instead of
 * pattern planes plus a runtime sequencer. Mirrors the editor's walkSong exactly:
 * row 0 is applied before ct is ever reset (so it runs tpr-1 ticks and later rows
 * run tpr), Bxx/Dxx are followed and never shipped, Fxx stays an event because it
 * retunes spt, and a volume cell that restates the channel's current value emits
 * nothing. Any divergence here shows up as an audible mismatch with the editor. */
/* t is the absolute tick, tpr the ticks-per-row in force at that row - the
 * locked writer needs it to place the sub-row effects, which in a sequencer
 * player compare against a tick-within-row counter that no longer exists. */
typedef struct { int t, c, n, rv, fx, pm, ins, tpr; } bc_event;
static bc_event *bc_ev = NULL;
static int bc_nev = 0, bc_any_fx = 0;
static long bc_walk_T = 0;

/* mutable walk state, so the row emit can be shared by both call sites the way
 * the editor's nested emit() closure is */
typedef struct { int tpr; long spt; int jo, jr; int rv_now[BIRB_NUM_CHANNELS]; int cap; } bc_wst;

static void bc_emit_row(const birb_song *song, int nch, int eo, int er, long tk, bc_wst *st) {
    for (int c = 0; c < nch; c++) {
        int q = song->order[eo][c];
        if (q == 255) continue;
        int pl = song->pattern_lengths[q]; if (!pl) pl = 16;
        if (er >= pl) continue;
        const birb_row *cell = &song->patterns[q][er][c];
        int n = cell->note, rv = cell->volume, fx = cell->effect, pm = cell->param;
        if (fx == FX_POS_JUMP) { st->jo = pm; st->jr = 0; }
        else if (fx == FX_PAT_BREAK) { if (st->jo < 0) st->jo = eo + 1; st->jr = pm; }
        if (fx == FX_SET_SPEED && pm) {
            if (pm < 0x20) st->tpr = pm; else st->spt = 44100L * 5 / (pm * 2);
        }
        int keep_fx = (fx && fx != FX_POS_JUMP && fx != FX_PAT_BREAK) ? fx : 0;
        if (n >= 2 && fx != FX_TONE_PORTA) st->rv_now[c] = 255;
        int rv_out = 0;
        if (rv && rv != st->rv_now[c]) { rv_out = rv; st->rv_now[c] = rv; }
        if (n || rv_out || keep_fx) {
            if (bc_nev >= st->cap) return;
            bc_ev[bc_nev].t = (int)tk; bc_ev[bc_nev].c = c;
            bc_ev[bc_nev].n = n; bc_ev[bc_nev].rv = rv_out;
            bc_ev[bc_nev].fx = keep_fx; bc_ev[bc_nev].pm = keep_fx ? pm : 0;
            bc_ev[bc_nev].ins = (cell->instrument == 0xFF) ? -1 : cell->instrument;
            bc_ev[bc_nev].tpr = st->tpr;
            if (keep_fx) bc_any_fx = 1;
            bc_nev++;
        }
    }
}

static void bc_walk(const birb_song *song, int nch) {
    bc_wst st;
    st.tpr = song->ticks_per_row ? song->ticks_per_row : 6;
    st.spt = 44100L * 5 / ((song->bpm ? song->bpm : 125) * 2);
    st.jo = -1; st.jr = 0;
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) st.rv_now[c] = 255;

    const int ol = song->order_length;
    int ct = 0, cr = 0, op = 0, first = 1, done = 0, guard = 0;
    long tk = 0, samples = 0;

    st.cap = (ol ? ol : 1) * BIRB_MAX_ROWS * (nch ? nch : 1) + 16;
    bc_ev = (bc_event *)malloc(sizeof(bc_event) * st.cap);
    bc_nev = 0; bc_any_fx = 0;
    if (!bc_ev) { bc_walk_T = 0; return; }

    while (!done && guard++ < 200000) {
        if (first) {
            first = 0;
            int n0 = bc_nev;
            bc_emit_row(song, nch, op, cr, tk, &st);
            /* row 0 is applied before ct is ever reset, so it runs one tick
             * shorter than the rest; tpr is read after the row in case an Fxx
             * on it retuned the span. */
            for (int k = n0; k < bc_nev; k++) bc_ev[k].tpr = st.tpr - 1;
        }
        ct++;
        if (ct >= st.tpr) {
            ct = 0;
            if (st.jo >= 0) { op = st.jo < ol ? st.jo : 0; cr = st.jr; st.jo = -1; st.jr = 0; }
            else {
                cr++;
                int pl = song->pattern_lengths[song->order[op][0]];
                if (!pl) pl = 16;
                if (cr >= pl) { cr = 0; if (++op >= ol) { done = 1; op = 0; } }
            }
            if (!done) {
                int n0 = bc_nev;
                bc_emit_row(song, nch, op, cr, tk, &st);
                for (int k = n0; k < bc_nev; k++) bc_ev[k].tpr = st.tpr;
            }
        }
        tk++; samples += st.spt;
    }
    bc_walk_T = samples;
}

/* ------------------------------------------------------------------
 * Locked-player event expansion.
 *
 * A sequencer resolves EDx, ECx and Rxy against a tick-within-row counter.
 * The locked player has no rows and no such counter, so they are resolved
 * here into the event list instead, which costs events and no player code.
 * Two note codes carry what a note number cannot: 254 restarts phase and
 * envelope without retriggering the voice (what Rxy does), 255 cuts.
 *
 * The sticky instrument column is resolved at the same time, so a note-on
 * names its parameter row outright and the player carries no fallback.
 * ------------------------------------------------------------------ */
#define BC_N_RETRIG 254
#define BC_N_CUT    255

static int bc_exp_cmp(const void *pa, const void *pb) {
    int ia = *(const int *)pa, ib = *(const int *)pb;
    if (bc_ev[ia].t != bc_ev[ib].t) return bc_ev[ia].t < bc_ev[ib].t ? -1 : 1;
    return ia < ib ? -1 : (ia > ib);   /* ties keep the order the walk emitted */
}

static int bc_expand_locked(void) {
    int cur[BIRB_NUM_CHANNELS];
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) cur[c] = 0;
    for (int k = 0; k < bc_nev; k++) {
        int c = bc_ev[k].c;
        if (c < 0 || c >= BIRB_NUM_CHANNELS) continue;
        if (bc_ev[k].n >= 2 && bc_ev[k].ins >= 0) cur[c] = bc_ev[k].ins;
        bc_ev[k].ins = cur[c];
    }

    int extra = 0;
    for (int k = 0; k < bc_nev; k++) {
        int span = bc_ev[k].tpr < 1 ? 1 : bc_ev[k].tpr;
        if (bc_ev[k].fx == FX_EXTENDED) extra++;
        else if (bc_ev[k].fx == FX_RETRIGGER && bc_ev[k].pm > 0)
            extra += span / bc_ev[k].pm + 1;
    }
    if (extra) {
        bc_event *g = (bc_event *)realloc(bc_ev, sizeof(bc_event) * (size_t)(bc_nev + extra));
        if (!g) return -1;
        bc_ev = g;
    }

    int n = bc_nev;
    for (int k = 0; k < bc_nev; k++) {
        bc_event *e = &bc_ev[k];
        int span = e->tpr < 1 ? 1 : e->tpr;
        if (e->fx == FX_EXTENDED && (e->pm >> 4) == 0xD) {
            int d = e->pm & 15;
            if (e->n >= 2) {
                /* the row no longer triggers; a delay that cannot land inside
                 * the row loses the note, which is what the runtime does when
                 * the next row clears the pending delay. */
                if (d > 0 && d < span) {
                    bc_event dl = *e;
                    dl.t = e->t + d; dl.rv = 0; dl.fx = 0; dl.pm = 0;
                    bc_ev[n++] = dl;
                }
                e->n = 0;
            }
            e->fx = 0; e->pm = 0;
        } else if (e->fx == FX_EXTENDED && (e->pm >> 4) == 0xC) {
            int v = e->pm & 15;
            if (v > 0 && v < span) {
                bc_event ct = *e;
                ct.t = e->t + v; ct.n = BC_N_CUT; ct.rv = 0; ct.fx = 0; ct.pm = 0;
                bc_ev[n++] = ct;
            }
            e->fx = 0; e->pm = 0;
        } else if (e->fx == FX_RETRIGGER) {
            int ri = e->pm;
            if (ri > 0)
                for (int t = ri; t < span; t += ri) {
                    bc_event rt = *e;
                    rt.t = e->t + t; rt.n = BC_N_RETRIG; rt.rv = 0; rt.fx = 0; rt.pm = 0;
                    bc_ev[n++] = rt;
                }
            e->fx = 0; e->pm = 0;
        }
    }
    bc_nev = n;

    /* an expanded event can land past the following row, so re-order */
    int *idx = (int *)malloc(sizeof(int) * (size_t)(bc_nev ? bc_nev : 1));
    bc_event *tmp = (bc_event *)malloc(sizeof(bc_event) * (size_t)(bc_nev ? bc_nev : 1));
    if (!idx || !tmp) { free(idx); free(tmp); return -1; }
    for (int k = 0; k < bc_nev; k++) idx[k] = k;
    qsort(idx, (size_t)bc_nev, sizeof(int), bc_exp_cmp);
    for (int k = 0; k < bc_nev; k++) tmp[k] = bc_ev[idx[k]];
    for (int k = 0; k < bc_nev; k++) bc_ev[k] = tmp[k];
    free(idx); free(tmp);

    bc_any_fx = 0;
    for (int k = 0; k < bc_nev; k++) if (bc_ev[k].fx) bc_any_fx = 1;
    return 0;
}

static int write_js(const char *filename, birb_song *song) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write '%s'\n", filename);
        return -1;
    }

    const int nch_js = BIRB_NUM_CHANNELS;
    const int LOCK = birb_smol;
    if (LOCK) bc_walk(song, nch_js);

    fprintf(f, "function birb(X){\n");
    fprintf(f, "var S=44100,N=4,F=65536,\n");
    fprintf(f, "bpm=%d,tpr=%d,ni=%d,np=%d,ol=%d,\n",
            song->bpm, song->ticks_per_row, song->num_instruments,
            song->num_patterns, song->order_length);

    /* order — superseded by the event list under smol */
    if (!LOCK) {
    fprintf(f, "O=[");
    for (int i = 0; i < song->order_length; i++) {
        if (i) fprintf(f, ",");
        fprintf(f, "[%d,%d,%d,%d]", song->order[i][0], song->order[i][1],
                song->order[i][2], song->order[i][3]);
    }
    fprintf(f, "],\n");
    }

    /* Detect which synth types we emit. */
    int uses_fm = 0, uses_ks = 0, uses_drum = 0, uses_formant = 0, uses_sine = 0;
    int uses_fm4 = 0, uses_fm2 = 0, fm_algo_mask = 0;
    char fm_fields[224], fm_zero[16];
    char fm_trig[512];
    int uses_pulse = 0, uses_tri = 0, uses_saw = 0, uses_noise = 0, n_basic_waves = 0;
    int drum_algos = 0;  /* bitmask: bit 0=KICK/TOM, 1=SNARE, 2=HAT/CRASH, 3=CLAP */
    for (int i = 0; i < song->num_instruments; i++) {
        if (song->instruments[i].synth_type == SYNTH_FM) {
            uses_fm = 1;
            /* FM4 carries all eight algorithms; emit only the operator count
             * and the algorithms the instruments reach. A 2-op patch never
             * calls FM4 at all. */
            int no = song->instruments[i].fm.num_ops ? song->instruments[i].fm.num_ops : 2;
            if (no >= 4) { uses_fm4 = 1; fm_algo_mask |= 1 << (song->instruments[i].fm.algorithm & 7); }
            else uses_fm2 = 1;
        }
        if (song->instruments[i].synth_type == SYNTH_KS) uses_ks = 1;
        if (song->instruments[i].synth_type == SYNTH_DRUM) {
            uses_drum = 1;
            int dt = song->instruments[i].drum_type & 7;
            int algo = (dt == 4) ? 0 : (dt == 5) ? 2 : dt;
            drum_algos |= (1 << (algo & 3));
        }
        if (song->instruments[i].synth_type == SYNTH_FORMANT) uses_formant = 1;
        if (song->instruments[i].synth_type == SYNTH_BASIC
            && song->instruments[i].waveform == WAVE_SINE) uses_sine = 1;
        if (song->instruments[i].synth_type == SYNTH_BASIC) {
            switch (song->instruments[i].waveform) {
                case WAVE_PULSE:    uses_pulse = 1; break;
                case WAVE_TRIANGLE: uses_tri   = 1; break;
                case WAVE_SAWTOOTH: uses_saw   = 1; break;
                case WAVE_NOISE:    uses_noise = 1; break;
                default: break;
            }
        }
    }

    n_basic_waves = uses_pulse + uses_tri + uses_saw + uses_noise + uses_sine;

    /* Reverb is emitted only when the song actually uses it (wet > 0 or some
     * instrument sends) — same emit-only-if-used pattern as FM/KS/drum. This
     * is how the exported player gets its "with / without reverb" variants. */
    /* per-voice dynamics are only emitted when some instrument uses them, so
     * a 4K song that does not touch drive or ducking pays nothing for them */
    int uses_drive = 0, uses_duck = 0;
    for (int i = 0; i < song->num_instruments; i++) {
        if (song->instruments[i].drive) uses_drive = 1;
        if (song->instruments[i].duck_send || song->instruments[i].duck_amt) uses_duck = 1;
    }

    /* Scan which effects the song actually uses. Roughly 40% of a
     * basic-oscillator export was effect code — the dispatch in R(), the
     * per-tick state machines in K(), and the channel fields — all emitted
     * unconditionally regardless of whether a single effect column was set. */
    int fx_used[FX_COUNT];
    for (int i = 0; i < FX_COUNT; i++) fx_used[i] = 0;
    int uses_any_fx = 0;
    for (int p = 0; p < song->num_patterns; p++)
        for (int r = 0; r < song->pattern_lengths[p]; r++)
            for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
                uint8_t e = song->patterns[p][r][c].effect;
                if (e && e < FX_COUNT) { fx_used[e] = 1; uses_any_fx = 1; }
            }
    /* Sub-cases of 7xy (extended): note cut vs note delay are separate code. */
    int uses_notecut = 0, uses_notedelay = 0;
    if (fx_used[FX_EXTENDED])
        for (int p = 0; p < song->num_patterns; p++)
            for (int r = 0; r < song->pattern_lengths[p]; r++)
                for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
                    if (song->patterns[p][r][c].effect == FX_EXTENDED) {
                        int sub = (song->patterns[p][r][c].param >> 4) & 0xF;
                        if (sub == 0xC) uses_notecut = 1;
                        if (sub == 0xD) uses_notedelay = 1;
                    }
    /* Pitch envelope is an instrument property, not an effect column. */
    int uses_pitchenv = 0;
    for (int i = 0; i < song->num_instruments; i++)
        if (song->instruments[i].pitch_env && song->instruments[i].pitch_env_len)
            uses_pitchenv = 1;
    /* Arpeggio likewise can come from the instrument, not only from 1xy. */
    int uses_arp = fx_used[FX_ARPEGGIO];
    for (int i = 0; i < song->num_instruments; i++)
        if (song->instruments[i].arp_note1 || song->instruments[i].arp_note2)
            uses_arp = 1;
    int uses_vib  = fx_used[FX_VIBRATO];
    int uses_trem = fx_used[FX_TREMOLO];
    int uses_lfo  = uses_vib || uses_trem;
    int uses_slide = fx_used[FX_PITCH_UP] || fx_used[FX_PITCH_DOWN];
    int uses_porta = fx_used[FX_TONE_PORTA];

    int uses_jump = fx_used[FX_POS_JUMP] || fx_used[FX_PAT_BREAK];

    /* smol birb. Every pattern the same length lets P() index off a constant
     * stride; one instrument per channel makes the instrument column dead. */
    int smol_rows = song->num_patterns ? song->pattern_lengths[0] : 64;
    if (!smol_rows) smol_rows = 64;
    int smol_same_len = 1;
    for (int p = 0; p < song->num_patterns; p++) {
        int n = song->pattern_lengths[p]; if (!n) n = 64;
        if (n != smol_rows) smol_same_len = 0;
    }
    int smol_ch_inst[BIRB_NUM_CHANNELS], smol_per_ch = 1;
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++) smol_ch_inst[c] = -1;
    for (int p = 0; p < song->num_patterns; p++)
        for (int r = 0; r < song->pattern_lengths[p]; r++)
            for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
                birb_row *cl = &song->patterns[p][r][c];
                if (cl->note < 2 || cl->instrument == 0xFF) continue;
                if (smol_ch_inst[c] < 0) smol_ch_inst[c] = cl->instrument;
                else if (smol_ch_inst[c] != cl->instrument) smol_per_ch = 0;
            }
    for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
        if (smol_ch_inst[c] < 0) smol_ch_inst[c] = 0;   /* channel never names one */
    char smol_span[64];
    if (birb_smol) snprintf(smol_span, sizeof smol_span, "T=%ld,", bc_walk_T);
    else snprintf(smol_span, sizeof smol_span, "T=ol*%d*tpr*spt,", smol_rows);
    int s_fixedlen = birb_smol && smol_same_len;
    int s_perch    = birb_smol && smol_per_ch;
    int s_novol    = birb_smol;
    /* Row advance. The jump branch only exists if the song can actually jump,
     * and an equal-length song compares against a literal instead of a lookup. */
    char smol_advance[192];
    {
        char len_expr[32];
        if (s_fixedlen) snprintf(len_expr, sizeof len_expr, "%d", smol_rows);
        else            snprintf(len_expr, sizeof len_expr, "pl[O[op][0]]");
        if (uses_jump)
            snprintf(smol_advance, sizeof smol_advance,
                "if(jo>=0){op=jo<ol?jo:0;cr=jr;jo=-1;jr=0}else{cr++\n"
                "if(cr>=%s){cr=0;if(++op>=ol)op=0}}R()}\n", len_expr);
        else
            snprintf(smol_advance, sizeof smol_advance,
                "cr++\nif(cr>=%s){cr=0;if(++op>=ol)op=0}R()}\n", len_expr);
    }
    if (birb_smol) {
        fprintf(stderr, "smol birb: dropped");
        if (s_fixedlen) fprintf(stderr, " fixed-pattern-length");
        if (s_perch)    fprintf(stderr, " one-instrument-per-channel");
        fprintf(stderr, " no-instrument-volume no-master-limiter\n");
        if (!smol_same_len)
            fprintf(stderr, "  kept: patterns have different lengths\n");
        if (!smol_per_ch)
            fprintf(stderr, "  kept: a channel plays more than one instrument\n");
        fprintf(stderr, "  SOUNDS DIFFERENT: no per-instrument volume, hard clip instead of the limiter\n");
    }

    int uses_reverb = 0;
#ifndef BIRB_NO_REVERB
    double rv_fb = 0, rv_dc = 0, rv_wet = 0;
    int rev_any_send = 0;
    for (int i = 0; i < song->num_instruments; i++)
        if (song->instruments[i].reverb_send) rev_any_send = 1;
    rv_fb  = 0.7 + 0.28 * (song->rev_size / 255.0);  /* baked: params are static in an export */
    rv_dc  = 0.4 * (song->rev_damp / 255.0);
    rv_wet = song->rev_wet / 255.0;
    /* both halves are needed for anything to be heard: a wet bus with no sends
     * receives nothing, and sends into a dry bus return nothing. */
    uses_reverb = (song->rev_wet > 0) && rev_any_send;
#endif

    /* Instruments. Base 11 fields (wave duty a d s r pe pel arp1 arp2 vol).
     * When the song uses FM, append `nops, algo, fb, mi, [[ri,rf,lv,a,d,s,r]×4]`
     * at indices 11-15. When the song uses KS, append the damping value at the
     * next index (11 if no FM, 16 if FM) so the JS generator can read
     * consistent offsets. Non-KS / non-FM instruments get zero stubs so the
     * array shape stays uniform.
     *
     * The per-op `[ri,rf,lv,a,d,s,r]` mirrors `birb_fm_op` (ratio_i, ratio_f,
     * level, ADSR) so the emitted JS can run the same per-op envelope state
     * machine the editor's renderSong/AudioWorklet uses. */
    fprintf(f, "I=[");
    for (int i = 0; i < song->num_instruments; i++) {
        birb_instrument *inst = &song->instruments[i];
        int wave;
        if (inst->synth_type == SYNTH_FM)      wave = 6;
        else if (inst->synth_type == SYNTH_KS) wave = 7;
        else if (inst->synth_type == SYNTH_DRUM) wave = 8;
        else if (inst->synth_type == SYNTH_FORMANT) wave = 9;
        else                                    wave = (int)inst->waveform;
        if (i) fprintf(f, ",");
        fprintf(f, "[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                wave, birb_duty_encode(inst->duty),
                inst->envelope.attack, inst->envelope.decay,
                inst->envelope.sustain, inst->envelope.release,
                (int)inst->pitch_env, inst->pitch_env_len,
                inst->arp_note1, inst->arp_note2,
                inst->volume);
#ifndef BIRB_NO_FM
        if (uses_fm) {
            int nops = inst->fm.num_ops ? inst->fm.num_ops : 2;
            fprintf(f, ",%d,%d,%d,%d,[",
                    nops, inst->fm.algorithm,
                    inst->fm.feedback,
                    inst->fm.mod_index ? inst->fm.mod_index : 64);
            for (int o = 0; o < 4; o++) {
                if (o) fprintf(f, ",");
                fprintf(f, "[%d,%d,%d,%d,%d,%d,%d]",
                        inst->fm.ops[o].ratio_i,
                        inst->fm.ops[o].ratio_f,
                        inst->fm.ops[o].level,
                        inst->fm.ops[o].adsr.attack,
                        inst->fm.ops[o].adsr.decay,
                        inst->fm.ops[o].adsr.sustain,
                        inst->fm.ops[o].adsr.release);
            }
            fprintf(f, "]");
        } else if (uses_ks || uses_drum || uses_formant) {
            /* placeholder: nops, algo, fb, mi, ops */
            fprintf(f, ",0,0,0,0,0");
        }
#else
        if (uses_ks || uses_drum || uses_formant) fprintf(f, ",0,0,0,0,0");
#endif
#ifndef BIRB_NO_KS
        if (uses_ks || uses_drum || uses_formant) {
            fprintf(f, ",%d", (inst->synth_type == SYNTH_KS) ? inst->ks_damping : 0);
        }
#endif
#ifndef BIRB_NO_DRUM
        if (uses_drum || uses_formant) {
            uint8_t tune_byte = (uint8_t)inst->drum_tune;
            fprintf(f, ",%d,%d,%d,%d,%d",
                    inst->drum_type & 7, tune_byte,
                    inst->drum_decay, inst->drum_tone, inst->drum_snap);
        }
#endif
#ifndef BIRB_NO_FORMANT
        if (uses_formant) {
            fprintf(f, ",%d,%d,%d,%d,%d,%d",
                    inst->formant_source_wave, inst->formant_duty & 3,
                    inst->formant_vowel_a & 7, inst->formant_vowel_b & 7,
                    inst->formant_sweep_speed, inst->formant_resonance);
        }
#endif
        fprintf(f, "]");
    }
    fprintf(f, "],\n");

#ifndef BIRB_NO_REVERB
    /* per-instrument reverb sends (0-255), indexed like I[] */
    if (uses_reverb) {
        fprintf(f, "RS=[");
        for (int i = 0; i < song->num_instruments; i++) {
            if (i) fprintf(f, ",");
            fprintf(f, "%d", song->instruments[i].reverb_send);
        }
        fprintf(f, "],\n");
    }
#endif

    /* per-instrument drive / duck send / duck amount, indexed like I[] */
    if (uses_drive) {
        fprintf(f, "DV=[");
        for (int i = 0; i < song->num_instruments; i++)
            fprintf(f, "%s%d", i ? "," : "", song->instruments[i].drive);
        fprintf(f, "],\n");
    }
    if (uses_duck) {
        fprintf(f, "DS=[");
        for (int i = 0; i < song->num_instruments; i++)
            fprintf(f, "%s%d", i ? "," : "", song->instruments[i].duck_send);
        fprintf(f, "],\nDA=[");
        for (int i = 0; i < song->num_instruments; i++)
            fprintf(f, "%s%d", i ? "," : "", song->instruments[i].duck_amt);
        fprintf(f, "],\n");
    }

    if (s_perch) {
        fprintf(f, "IC=[");
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            fprintf(f, "%s%d", c ? "," : "", smol_ch_inst[c]);
        fprintf(f, "],\n");
    }

    /* pattern lengths (a single stride needs no table; none at all under smol) */
    if (!s_fixedlen && !LOCK) {
        fprintf(f, "pl=[");
        for (int p = 0; p < song->num_patterns; p++) {
            if (p) fprintf(f, ",");
            fprintf(f, "%d", song->pattern_lengths[p] ? song->pattern_lengths[p] : 64);
        }
        fprintf(f, "],\n");
    }

    /* 5 flat arrays with offset table for variable-length patterns */
    const char *pnames[] = {"pn", "pi", "pv", "pf", "pp"};
    for (int plane = 0; plane < (LOCK ? 0 : 5); plane++) {
        /* check if this plane is entirely empty/default */
        int all_empty = 1;
        uint8_t empty_val = (plane == 1) ? 0xFF : 0; /* inst plane default is 0xFF */
        for (int p = 0; p < song->num_patterns && all_empty; p++) {
            int nrows = song->pattern_lengths[p]; if (!nrows) nrows = 64;
            for (int c = 0; c < BIRB_NUM_CHANNELS && all_empty; c++)
                for (int r = 0; r < nrows && all_empty; r++) {
                    uint8_t val = 0;
                    switch (plane) {
                        case 0: val = song->patterns[p][r][c].note; break;
                        case 1: val = song->patterns[p][r][c].instrument; break;
                        case 2: val = song->patterns[p][r][c].volume; break;
                        case 3: val = song->patterns[p][r][c].effect; break;
                        case 4: val = song->patterns[p][r][c].param; break;
                    }
                    if (val != empty_val) all_empty = 0;
                }
        }
        /* With one instrument the instrument column carries no information:
         * every note resolves to C.i, which starts at 0. Drop the plane.
         * smol birb drops it whenever each channel sticks to one instrument. */
        if (plane == 1 && (song->num_instruments <= 1 || s_perch)) all_empty = 1;
        if (all_empty) {
            /* emit empty array — engine will handle undefined as 0/255 */
            fprintf(f, "%s=[],\n", pnames[plane]);
            continue;
        }
        /* Flatten first so trailing defaults can be dropped: P() reads
         * a[...]||0 so anything past the end already reads as 0. Only safe for
         * the planes whose default IS 0 — the instrument plane defaults to
         * 0xFF, so it cannot be truncated this way. */
        int total = 0;
        for (int p = 0; p < song->num_patterns; p++) {
            int nrows = song->pattern_lengths[p]; if (!nrows) nrows = 64;
            total += nrows * BIRB_NUM_CHANNELS;
        }
        uint8_t *flat = (uint8_t *)malloc(total ? total : 1);
        int w = 0;
        for (int p = 0; p < song->num_patterns; p++) {
            int nrows = song->pattern_lengths[p]; if (!nrows) nrows = 64;
            for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
                for (int r = 0; r < nrows; r++) {
                    switch (plane) {
                        case 0: flat[w] = song->patterns[p][r][c].note; break;
                        case 1: flat[w] = song->patterns[p][r][c].instrument; break;
                        case 2: flat[w] = song->patterns[p][r][c].volume; break;
                        case 3: flat[w] = song->patterns[p][r][c].effect; break;
                        case 4: flat[w] = song->patterns[p][r][c].param; break;
                    }
                    w++;
                }
        }
        int emit_n = total;
        if (plane != 1) while (emit_n > 0 && flat[emit_n - 1] == 0) emit_n--;
        fprintf(f, "%s=[", pnames[plane]);
        for (int k = 0; k < emit_n; k++) fprintf(f, "%s%d", k ? "," : "", flat[k]);
        fprintf(f, "],\n");
        free(flat);
    }

    /* offset table + accessor. Equal-length patterns index off a constant
     * stride, so neither table has to exist. Continues the declaration chain
     * above, hence no `var`. */
    if (LOCK) {
        /* delta-encoded tick first, so the numbers stay one or two digits.
         * inst rides along only when a channel is not bound to one instrument,
         * fx/param only when some non-structural effect survives the walk. */
        const int with_inst = !s_perch, with_fx = bc_any_fx;
        int prev = 0;
        fprintf(f, "EV=[");
        for (int k = 0; k < bc_nev; k++) {
            const bc_event *e = &bc_ev[k];
            fprintf(f, "%s%d,%d,%d,%d", k ? "," : "", e->t - prev, e->c, e->n, e->rv);
            prev = e->t;
            if (with_inst) fprintf(f, ",%d", e->ins < 0 ? 255 : e->ins);
            if (with_fx)   fprintf(f, ",%d,%d", e->fx, e->pm);
        }
        fprintf(f, "],ei=0,tk=0,et=0,i,c,r\n");
    }
    else if (s_fixedlen)
        fprintf(f,
            "i,c,r\n"
            "function P(a,p,c,r){return a.length?a[(p*N+c)*%d+r]||0:0}\n", smol_rows);
    else
        fprintf(f,
            "po=[0],i,c,r\n"
            "for(i=1;i<np;i++)po[i]=po[i-1]+pl[i-1]*N\n"
            "function P(a,p,c,r){return a.length?a[po[p]+c*pl[p]+r]||0:0}\n"
        );

    /* synth engine */
    fprintf(f,
        "var bf=[6221,6591,6983,7398,7838,8304,8797,9321,9875,10462,11084,11743],\n"
        "dv=[8192,16384,32768,49152],\n"
        "nf=n=>(n=n<0?0:n>95?95:n,((bf[n%%12]<<(n/12))+128)>>8),\n"
        "spt=S*5/((bpm||125)*2)|0,%s\n"
        "out=new Float32Array(T),ch=[]%s,tc=0\n",
        LOCK ? smol_span : s_fixedlen ? smol_span
             : "W=pl.reduce((a,b)=>a>b?a:b,1),T=ol*W*tpr*spt,",
        LOCK ? "" : ",ct=0,cr=0,op=0");
#ifndef BIRB_NO_REVERB
    /* reverb send bus (mirror of the editor's makeReverb; coefficients baked). */
    if (uses_reverb)
        fprintf(f,
            "var RB=[1116,1188,1277,1356%s].map(n=>new Float32Array(n)),RP=[0,0,0,0%s],RCL=[0,0,0,0]\n"
            "function RV(x){var o=0,k,b,p,y;for(k=0;k<4;k++){b=RB[k];p=RP[k];y=b[p];"
            "RCL[k]=y*%.6f+RCL[k]*%.6f;b[p]=x+RCL[k]*%.6f;RP[k]=p+1<b.length?p+1:0;o+=y}"
            "o*=%.6f;%sreturn o*%.6f}\n",
            birb_smol ? "" : ",556,441", birb_smol ? "" : ",0,0",
            1.0 - rv_dc, rv_dc, rv_fb, (1.0 - rv_fb) * 5.5,
            birb_smol ? "" : "for(k=4;k<6;k++){b=RB[k];p=RP[k];y=b[p];var ou=-o+y;"
                                "b[p]=o+y*0.5;RP[k]=p+1<b.length?p+1:0;o=ou}",
            rv_wet);
#endif
    snprintf(fm_trig, sizeof fm_trig,
        "\nif(j[0]===6){var fm=j[15];%s%sC.fmFb=j[13];C.fmMi=j[14];C.fmPrev=0;for(var k=0;k<%d;k++){"
        "var o_=fm[k];C.fmp[k]=0;C.fmR[k]=(o_[0]*16+(o_[1]&15))/16;C.fmf[k]=Math.round(C.b*C.fmR[k]);"
        "C.fmL[k]=Math.round(F*o_[2]/255);if((o_[3]|0)==0){C.fmEnv[k]=F;C.fmStg[k]=2}else{C.fmEnv[k]=0;C.fmStg[k]=1}}}",
        (uses_fm4 && uses_fm2) ? "C.fmNo=j[11];" : "", uses_fm4 ? "C.fmAlgo=j[12]|0;" : "",
        uses_fm4 ? 4 : 2);
    snprintf(fm_zero, sizeof fm_zero, "%s", uses_fm4 ? "[0,0,0,0]" : "[0,0]");
    snprintf(fm_fields, sizeof fm_fields,
        ",fmp:%s,fmf:%s,fmL:%s,fmR:%s,fmEnv:%s,fmStg:%s,fmMi:64,fmFb:0%s%s,fmPrev:0",
        fm_zero, fm_zero, fm_zero, uses_fm4 ? "[1,1,0,0]" : "[1,1]", fm_zero, fm_zero,
        (uses_fm4 && uses_fm2) ? ",fmNo:2" : "", uses_fm4 ? ",fmAlgo:0" : "");
    fprintf(f,
        "for(c=0;c<N;c++)ch[c]={p:0,f:0,b:0,w:0,n:0,u:F/2,e:0,t:0,a:0,d:0,s:0,r:0,i:0,%srv:255%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s}\n"
        "%s",
        s_novol ? "" : "v:255,",
        uses_pitchenv ? ",q:0,g:0" : "",
        uses_arp ? ",x:0,y:0,k:0" : "",
        uses_slide ? ",l:0" : "",
        uses_porta ? ",pt:0,ps:0" : "",
        fx_used[FX_RETRIGGER] ? ",ri:0" : "",
        uses_notecut ? ",nc:0" : "",
        uses_notedelay ? ",nd:0,dn:0,di:0" : "",
        uses_vib ? ",vp:0,vs:0,vd:0" : "",
        uses_trem ? ",tp:0,ts:0,td:0,tm:0" : "",
        uses_noise ? ",h:0x7FFF,j:16,m:0" : "",
        uses_fm ? fm_fields : "",
        uses_ks ? ",kb:new Int16Array(1024),kl:0,kp:0,kd:0,kg:0" : "",
        uses_drum ? ",drAl:0,drAlOrig:0,drP2:0,drNz:0,drPe:0,drPet:0,drRate:0,drSnap:0,drClk:0,drZ1:0,drZ2:0,drLf:0x7FFF,drTtl:0,drMix:0,drStage:0,drStageT:0,drBurstLen:0,drBodyT:0" : "",
        uses_formant ? ",ftSw:0,ftLf:0x7FFF,ftVa:0,ftVb:0,ftSp:0,ftDr:1,ftSS:0,ftR:128,ftRc:0,ftZ1:[0,0,0],ftZ2:[0,0,0],ftB0:[0,0,0],ftA1:[0,0,0],ftA2:[0,0,0]" : "",
        uses_reverb ? ",rs:0" : "",
        uses_jump ? "var jo=-1,jr=0\n" : "");
    if (uses_drive) fprintf(f, "for(c=0;c<N;c++){ch[c].dp=1;ch[c].dn=1}\n");
    if (uses_duck)  fprintf(f, "for(c=0;c<N;c++){ch[c].ds=0;ch[c].da=0}\n");
    char master_consts[160];
    if (birb_no_master)
        snprintf(master_consts, sizeof master_consts, "var MG=%.6f\n",
                 (song->master_gain ? song->master_gain : 128) / 64.0);
    else
        snprintf(master_consts, sizeof master_consts,
                 "var LE=0,MG=%.6f,MT=%.6f,MR=%.6f,MC=SS(%.6f)\n",
                 (song->master_gain ? song->master_gain : 128) / 64.0,
                 (song->limit_thresh ? song->limit_thresh : 242) / 255.0,
                 1.0 - 1.0 / (44100.0 * ((song->limit_release ? song->limit_release : 50) * 0.001)),
                 (song->limit_thresh ? song->limit_thresh : 242) / 255.0);

    /* Master bus. SS is the rational tanh the C engine uses (birb_soft_sat);
     * TG is the per-synth-type loudness calibration indexed by wave id, as the
     * same fixed16 constants over 65536 so C and JS agree. Both are always
     * emitted: without them the exported player would not match the editor. */
    fprintf(f,
        "%s"
        "%s"
        "%s"
        "%s",
        (!birb_no_master || uses_drive)
            ? "function SS(x){if(x<-3)x=-3;else if(x>3)x=3;var x2=x*x;return x*(27+x2)/(27+9*x2)}\n" : "",
        uses_lfo ? "function LT(p){p&=65535;var t=p<32768?p:65536-p;return((t<<2)-65536)>>7}\n" : "",
        (!uses_fm && !uses_ks && !uses_drum && !uses_formant)
            ? ""   /* gain inlined at the mix site instead */
            : "var TG=[29819,29819,29819,29819,29819,23127,15360,38838,61580,149078].map(v=>v/65536)\n",
        master_consts);
    /* drum-only tables — TG and the master constants above are NOT gated, every
     * song needs them */
    if (uses_drum)
        fprintf(f,
            "var KSW=[65518,65516,65512,65509,65505,65500,65495,65489,65482,65474,65465,65454,65442,65428,65412,65393,65372,65348,65320,65287,65251,65208,65160,65104,65040,64966,64882,64785,64674,64546,64400,64233,64067]\n"
            "function KC(t){var i=t>>3,f=t&7,c=KSW[i];return c+(((KSW[i+1]-c)*f)>>3)}\n"
            "var SHP=[1386,1545,1722,1920,2139,2383,2655,2956,3291,3663,4076,4533,5039,5600,6219,6903,7657,8487,9400,10401,11498,12697,14004,15424,16964,18627,20416,22332,24376,26543,28828,31221,33392]\n"
            "function SC(t){var i=t>>3,f=t&7,c=SHP[i];return c+(((SHP[i+1]-c)*f)>>3)}\n");
    if (uses_ks)
        fprintf(f,
            "var KQ=[657,776,916,1082,1277,1508,1781,2103,2484,2933,3463,4089,4829,5702,6733,7951,9388,11086,13091,15458,18253,21554,25452,30054,35489,41907,49485,58434,69001,81479,96213,113612,131398]\n"
            "function KG(d,l){var i=d>>3,f=d&7,q=KQ[i];q+=((KQ[i+1]-q)*f)>>3;var a=(q*l)>>8;return a>=65535?0:65535-a}\n");
    if (uses_duck)
        fprintf(f, "var DE=0,DR=%.6f\n",
            1.0 - 1.0 / (44100.0 * ((song->duck_release ? song->duck_release : 120) * 0.001)));
    if (uses_fm || uses_drum || uses_sine) {
        fprintf(f,
            "function SA_(ph){var p=((ph%%F)+F)%%F,ng=p>=F/2,t=ng?p-F/2:p;if(t>F/4)t=F/2-t;var x=t/F*4;if(x>1)x=1;var x2=x*x;var y=x*(1.5707288-x2*(0.6432292-x2*0.0727778));if(y>1)y=1;return ng?-y:y}\n");
    }
    if (uses_formant) {
        /* Runtime RBJ bandpass coefficient computation. Vowel frequencies
         * (Hz) × 3 formants per vowel; per-formant gains baked into b0.
         * Coeffs recomputed when vowel or Q changes; sweep just interps. */
        fprintf(f,
            "function FI(C){var A=FCO[C.i][0],B=FCO[C.i][1],t=C.ftSp,o=255-t;for(var i=0;i<3;i++){C.ftB0[i]=((A[i*3]*o+B[i*3]*t)/255)|0;C.ftA1[i]=((A[i*3+1]*o+B[i*3+1]*t)/255)|0;C.ftA2[i]=((A[i*3+2]*o+B[i*3+2]*t)/255)|0}}\n");
        /* baked coefficients per instrument — no trig, and identical to what
         * the C engine reads from the song, so the two agree exactly */
        fprintf(f, "var FCO=[");
        for (int i = 0; i < song->num_instruments; i++) {
            birb_instrument *fin = &song->instruments[i];
            if (i) fprintf(f, ",");
            if (fin->synth_type != SYNTH_FORMANT) { fprintf(f, "0"); continue; }
            double q = bc_formant_q(fin->formant_resonance);
            int32_t cf[2][3][3];
            bc_formant_coeffs(fin->formant_vowel_a, q, cf[0]);
            bc_formant_coeffs(fin->formant_vowel_b, q, cf[1]);
            fprintf(f, "[[");
            for (int fi = 0; fi < 3; fi++) for (int c = 0; c < 3; c++)
                fprintf(f, "%s%d", (fi || c) ? "," : "", cf[0][fi][c]);
            fprintf(f, "],[");
            for (int fi = 0; fi < 3; fi++) for (int c = 0; c < 3; c++)
                fprintf(f, "%s%d", (fi || c) ? "," : "", cf[1][fi][c]);
            fprintf(f, "]]");
        }
        fprintf(f, "]\n");
    }
    if (uses_fm4) {
        /* 4-op FM, mirrors fmRender4() in the editor. raw is the carrier op0's
         * pre-level sine; fmPrev = raw*F so feedback maths matches 2-op exactly.
         * Only the algorithms the song reaches are emitted; a single one needs
         * no switch, and with several the last arm is the default so an
         * out-of-range value cannot fall through with s undefined. */
        static const char *FM_ALG[8] = {
            "s3=SA_(C.fmp[3]+fb)*l3;s2=SA_(C.fmp[2]+s3)*l2;s1=SA_(C.fmp[1]+s2)*l1;raw=SA_(C.fmp[0]+s1*mi);s=raw*l0/F;",
            "s3=SA_(C.fmp[3]+fb)*l3;s2=SA_(C.fmp[2])*l2;s1=SA_(C.fmp[1]+s3+s2)*l1;raw=SA_(C.fmp[0]+s1*mi);s=raw*l0/F;",
            "s3=SA_(C.fmp[3]+fb)*l3;s2=SA_(C.fmp[2]+s3)*l2;s1=SA_(C.fmp[1])*l1;raw=SA_(C.fmp[0]+(s2+s1)*mi);s=raw*l0/F;",
            "s3=SA_(C.fmp[3]+fb)*l3;s1=SA_(C.fmp[1]+s3)*l1;s2=SA_(C.fmp[2])*l2;raw=SA_(C.fmp[0]+(s1+s2)*mi);s=raw*l0/F;",
            "s3=SA_(C.fmp[3]+fb)*l3;s2=SA_(C.fmp[2])*l2;s1=SA_(C.fmp[1])*l1;raw=SA_(C.fmp[0]+(s3+s2+s1)*mi);s=raw*l0/F;",
            "s3=SA_(C.fmp[3]+fb)*l3;s1=SA_(C.fmp[1])*l1;var r2=SA_(C.fmp[2]+s3*mi);raw=SA_(C.fmp[0]+s1*mi);s=(r2*l2+raw*l0)/F*0.5;",
            "s3=SA_(C.fmp[3]+fb)*l3;var r2=SA_(C.fmp[2]+s3*mi),r1=SA_(C.fmp[1]);raw=SA_(C.fmp[0]);s=(r2*l2+r1*l1+raw*l0)/F/3;",
            "var r3=SA_(C.fmp[3]+fb),r2=SA_(C.fmp[2]),r1=SA_(C.fmp[1]);raw=SA_(C.fmp[0]);s=(r3*l3+r2*l2+r1*l1+raw*l0)/F*0.25;",
        };
        int n_alg = 0, last = 0;
        for (int a = 0; a < 8; a++) if (fm_algo_mask & (1 << a)) { n_alg++; last = a; }
        fprintf(f,
            "function FM4(C){var l0=C.fmL[0]*C.fmEnv[0]/F,l1=C.fmL[1]*C.fmEnv[1]/F,l2=C.fmL[2]*C.fmEnv[2]/F,l3=C.fmL[3]*C.fmEnv[3]/F;\n"
            "var mi=C.fmMi/255,fb=C.fmFb?C.fmPrev*C.fmFb/256:0;var s,raw,s3,s2,s1;\n");
        if (n_alg == 1) fprintf(f, "%s\n", FM_ALG[last]);
        else {
            fprintf(f, "switch(C.fmAlgo&7){\n");
            for (int a = 0; a < 8; a++) {
                if (!(fm_algo_mask & (1 << a))) continue;
                if (a == last) fprintf(f, "default:{%sbreak}\n", FM_ALG[a]);
                else           fprintf(f, "case %d:{%sbreak}\n", a, FM_ALG[a]);
            }
            fprintf(f, "}\n");
        }
        fprintf(f,
            "C.fmPrev=raw*F;for(var i=0;i<4;i++)C.fmp[i]=(C.fmp[i]+C.fmf[i])%%F;return s}\n");
    }
    /* KS damping lives at j[11] (no FM/drum) or j[16] (FM or drum pads 4). */
    int ks_idx = (uses_fm || uses_drum || uses_formant) ? 16 : 11;
    int drum_base = (uses_fm || uses_ks || uses_drum || uses_formant) ? 17 : -1;
    int formant_base = drum_base >= 0 ? drum_base + 5 : -1;
    char formant_trigger[512] = "";
    if (uses_formant) {
        snprintf(formant_trigger, sizeof(formant_trigger),
            "\nif(j[0]===9){var sw=j[%d];if(sw!==0&&sw!==2&&sw!==3)sw=2;C.ftSw=sw;C.ftLf=(0x7FFF^(s*0x2BCD&0xFFFF))&0xFFFF;if(!C.ftLf)C.ftLf=0x7FFF;C.ftVa=j[%d]&7;if(C.ftVa>4)C.ftVa=0;C.ftVb=j[%d]&7;if(C.ftVb>4)C.ftVb=0;C.ftSS=j[%d];C.ftR=j[%d];C.ftSp=0;C.ftDr=1;C.ftRc=0;for(var fi=0;fi<3;fi++){C.ftZ1[fi]=0;C.ftZ2[fi]=0}C.u=[F/8,F/4,F/2,F*3/4][j[%d]&3];FI(C)}",
            formant_base, formant_base + 2, formant_base + 3, formant_base + 4, formant_base + 5, formant_base + 1);
    }
    fprintf(f,
        "function TR(C,n,ii){C.i=ii;var s=n-2,j=I[ii]\n"
        "C.n=s;C.b=nf(s);C.f=C.b;C.p=0;C.w=j[0];C.u=dv[j[1]&3]\n"
        "C.a=j[2];C.d=j[3];C.s=j[4];C.r=j[5];\n"
        "if(C.a==0){C.e=F;C.t=2}else{C.e=0;C.t=1}\n"
        "%sC.rv=255%s%s%s%s%s%s%s\n"
        "%s%s%s%s%s}\n",
        s_novol ? "" : "C.v=j[10]||255;",
        uses_pitchenv ? ";C.q=j[6];C.g=j[7]" : "",
        uses_arp ? ";C.x=j[8];C.y=j[9];C.k=0" : "",
        uses_slide ? ";C.l=0" : "",
        uses_porta ? ";C.ps=0" : "",
        uses_reverb ? ";C.rs=RS[ii]||0" : "",
        uses_drive ? ";C.dp=DV[ii]?1+DV[ii]*(8/255):1;C.dn=C.dp>1?1/SS(C.dp):1" : "",
        uses_duck  ? ";C.ds=DS[ii]||0;C.da=DA[ii]||0" : "",
        uses_noise ? "if(j[0]===3){C.h=0x7FFF;C.m=0;C.j=256>>(s/12)||1}" : "",
        uses_fm
            ? fm_trig
            : "",
        uses_ks
            ? "\nif(j[0]===7){var ln=C.b>0?F/C.b|0:0;if(ln<4)ln=4;if(ln>1024)ln=1024;C.kl=ln;C.kp=0;C.kd=j[16]||0;C.kg=KG(C.kd,ln);var lf=(0x7FFF^(s*0x1D79&0xFFFF))&0xFFFF;if(!lf)lf=0x7FFF;for(var ki=0;ki<ln;ki++){var kbit=(lf^(lf>>1))&1;lf=((lf>>1)|(kbit<<14))&0xFFFF;C.kb[ki]=(lf&1)?32767:-32767}}"
            : "",
        uses_drum
            ? "\nif(j[0]===8){var dt=j[17]&7,al=dt===4?0:dt===5?2:dt,tn=j[18];if(tn>127)tn-=256;var dec=j[19],tone=j[20],snp=j[21],dn=s+tn;if(dn<0)dn=0;if(dn>95)dn=95;var df=nf(dn),tt;C.drAl=al;C.drAlOrig=dt;C.drP2=0;C.drZ1=0;C.drZ2=0;C.drLf=(0x7FFF^(s*0x3D7F&0xFFFF))&0xFFFF;if(!C.drLf)C.drLf=0x7FFF;"
              "if(al===0){C.drPe=(df<<3)<<8;C.drPet=(df>>1)<<8;C.drRate=KC(tone);C.drSnap=snp;C.drClk=384;tt=dec*200+1024;if(dt===4)tt*=2}"
              "else if(al===1){C.f=df>0?df:nf(26);C.b=C.f;C.drMix=snp;C.drPet=SC(tone);C.drPe=F;C.drRate=65460;C.drZ1=0;tt=dec*120+1024;C.drP2=F;C.drNz=F-Math.min(4096,(301466/(tt||1))|0)}"
              "else if(al===2){var hp=snp*(F*15/16/255)|0;if(hp<F>>4)hp=F>>4;C.drPet=hp;C.drZ1=0;tt=dec*180+1024;if(dt===5)tt=90000+dec*400}"
              "else{C.drBurstLen=80+(snp>>1);C.drStage=0;C.drStageT=C.drBurstLen;tt=dec*160+2048}"
              "if(tt>0xFFFFFF)tt=0xFFFFFF;C.drTtl=tt}"
            : "",
        formant_trigger
    );
    (void)ks_idx; (void)drum_base; /* indices used via hardcoded layout above */
    (void)formant_base;
    char rhead[320];
    if (LOCK) {
        const int with_inst = !s_perch, with_fx = bc_any_fx;
        int k = 4;
        char inst[24], fxp[48];
        if (with_inst) { snprintf(inst, sizeof inst, "EV[ei+%d]", k++); }
        else           { snprintf(inst, sizeof inst, "IC[c]"); }
        if (with_fx) { snprintf(fxp, sizeof fxp, "fx=EV[ei+%d],pm=EV[ei+%d]", k, k+1); k += 2; }
        else         { snprintf(fxp, sizeof fxp, "fx=0,pm=0"); }
        snprintf(rhead, sizeof rhead,
            "function R(){while(ei<EV.length&&et+EV[ei]<=tk){et+=EV[ei];c=EV[ei+1];"
            "var C=ch[c],n=EV[ei+2],rv=EV[ei+3],%s,ii=%s;ei+=%d\n", fxp, inst, k);
    } else {
        snprintf(rhead, sizeof rhead,
            "function R(){for(c=0;c<N;c++){var q=O[op][c],C=ch[c];if(q>=np)continue\n"
            "var n=P(pn,q,c,cr),ii=%s,rv=P(pv,q,c,cr),fx=P(pf,q,c,cr),pm=P(pp,q,c,cr)\n",
            s_perch ? "IC[c]" : "pi.length?P(pi,q,c,cr):255");
    }
    fprintf(f,
        "%s"
        /* pi.length?...:255 — NOT `P(pi..)||255`: P returns 0 for inst0, and
         * `0||255` would turn an explicit inst0 into "keep current instrument". */
        "%s%s%s%s%s"
        "%sif(n==1){C.t=4;%s}else if(n>=2){%s"
        "%s}\n"
        "if(rv)C.rv=rv\n"
        "%s%s%s%s%s%s%s%s%s%s%s}}\n",
        rhead,
        fx_used[FX_RETRIGGER]  ? "C.ri=0;" : "",
        uses_notecut           ? "C.nc=0;" : "",
        uses_notedelay         ? "C.nd=0;" : "",
        (uses_porta || uses_notedelay)
            ? "\nvar itp=fx==5,ind=fx==7&&(pm>>4)==0xD\n" : "\n",
        uses_notedelay
            ? (s_perch ? "if(ind&&n>=2){C.dn=n;C.di=ii;C.nd=pm&15}\n"
                       : "if(ind&&n>=2){C.dn=n;C.di=ii==255?C.i:ii;C.nd=pm&15}\n") : "",
        uses_notedelay ? "else " : "",
        uses_fm ? "if(C.fmStg)for(var k=0;k<4;k++)if(C.fmStg[k])C.fmStg[k]=4" : "",
        uses_porta ? "if(itp){C.pt=nf(n-2)}\nelse" : "",
        s_perch ? "TR(C,n,ii)" : "{if(ii==255)ii=C.i;if(ii<ni)TR(C,n,ii)}",
        fx_used[FX_ARPEGGIO]   ? "if(fx==1){C.x=pm>>4;C.y=pm&15;C.k=0}\n" : "",
        (fx_used[FX_PITCH_UP]||fx_used[FX_PITCH_DOWN])
            ? "if(fx==2)C.l=pm<<2;else if(fx==3)C.l=-(pm<<2)\n" : "",
        fx_used[FX_VIBRATO]    ? "if(fx==4){C.vs=F/64*(pm>>4);C.vd=(pm&15)<<4}\n" : "",
        fx_used[FX_TONE_PORTA] ? "if(fx==5)C.ps=pm<<2\n" : "",
        fx_used[FX_RETRIGGER]  ? "if(fx==6)C.ri=pm\n" : "",
        uses_notecut           ? "if(fx==7&&(pm>>4)==0xC)C.nc=pm&15\n" : "",
        fx_used[FX_TREMOLO]    ? "if(fx==8){C.ts=F/64*(pm>>4);C.td=(pm&15)<<4}\n" : "",
        fx_used[FX_SAMPLE_OFFSET] ? "if(fx==9&&C.w===5)C.sp=(pm<<8)<<16\n" : "",
        (fx_used[FX_POS_JUMP] && !LOCK) ? "if(fx==0xB){jo=pm;jr=0}\n" : "",
        ((fx_used[FX_PAT_BREAK] && !LOCK) ? "if(fx==0xD){if(jo<0)jo=op+1;jr=pm}\n" : ""),
        (fx_used[FX_SET_SPEED]
            ? "if(fx==0xF&&pm){if(pm<0x20)tpr=pm;else{bpm=pm;spt=S*5/(pm*2)|0}}\n" : "")
    );
    fprintf(f,
        "%s"
        "function K(){%s"
        "%s"
        "for(c=0;c<N;c++){var C=ch[c]\n%s",
        LOCK ? "" : "R()\n",
        LOCK ? "R();tk++\n" : "ct++;if(ct>=tpr){ct=0\n",
        LOCK ? "" : smol_advance,
        uses_notedelay ? "if(C.nd&&ct==C.nd){if(C.di<ni)TR(C,C.dn,C.di);C.nd=0}\n" : "");
    fprintf(f, "%s%s%s%s%s%s%s%s",
        uses_notecut  ? "if(C.nc&&ct==C.nc){C.e=0;C.t=0}\n" : "",
        fx_used[FX_RETRIGGER]
            ? "if(C.ri&&ct>0&&ct%C.ri==0){C.p=0;C.t=1;C.e=0;if(C.w>=3){C.h=0x7FFF;C.m=0}}\n" : "",
        uses_pitchenv ? "if(C.g){C.b+=C.q<<2;if(C.b<1)C.b=1;C.g--}\n" : "",
        uses_slide    ? "if(C.l){C.b+=C.l;if(C.b<1)C.b=1}\n" : "",
        uses_porta
            ? "if(C.pt&&C.ps){if(C.b<C.pt){C.b+=C.ps;if(C.b>C.pt)C.b=C.pt}else if(C.b>C.pt){C.b-=C.ps;if(C.b<C.pt)C.b=C.pt}}\n" : "",
        uses_arp
            ? "if(C.x|C.y){var n=C.n,t=C.k%3;C.f=nf(t==1?n+C.x:t==2?n+C.y:n);C.k++}else C.f=C.b\n"
            : "C.f=C.b\n",
        uses_vib  ? "if(C.vd){C.f+=LT(C.vp)*C.vd/F;C.vp+=C.vs}\n" : "",
        uses_trem ? "if(C.td){C.tm=(LT(C.tp)*C.td)>>1;C.tp+=C.ts}else C.tm=0\n" : "");
    fprintf(f, "%s",
        "var e=C.t;if(e==1){C.e+=F/(C.a+1);if(C.e>=F){C.e=F;C.t=2}}\n"
        "else if(e==2){var g=F*C.s/255;C.e-=(F-g)/(C.d+1);if(C.e<=g){C.e=g;C.t=3}}\n"
        "else if(e==4){C.e-=C.e/(C.r+1);if(C.e<64){C.e=0;C.t=0}}\n");
    /* Per-tick live param refresh + per-op envelope state machine. Mirrors
     * the editor's doTick: re-read instrument arrays each tick so slider
     * tweaks (in tools that set ins fields between ticks) take effect on
     * already-ringing notes. For pure file playback the song is static, but
     * keeping the path identical guarantees byte-for-byte match with the
     * editor's renderSong. */
    if (uses_fm) {
        /* Per-tick FM live refresh + per-op envelope advance. NB: the live
         * loop variable here is `oo` (not `op`) — `op` is the outer
         * order-position state and var-hoisting would clobber it. */
        fprintf(f,
            "if(C.w===6&&C.i<ni){var jj=I[C.i],fm=jj[15];C.fmFb=jj[13];C.fmMi=jj[14];%s\n"
            "for(var k=0;k<%d;k++){var oo=fm[k];C.fmR[k]=(oo[0]*16+(oo[1]&15))/16;C.fmf[k]=Math.round(C.f*C.fmR[k]);C.fmL[k]=Math.round(F*(oo[2]||0)/255);\n"
            "var st=C.fmStg[k],en=C.fmEnv[k],oa=oo[3]|0,od=oo[4]|0,os=oo[5]|0,oR=oo[6]|0;\n"
            "if(st===1){en+=F/(oa+1);if(en>=F){en=F;st=2}}else if(st===2){var g2=(F*os/255)|0;en-=(F-g2)/(od+1);if(en<=g2){en=g2;st=3}}else if(st===4){en-=en/(oR+1);if(en<64){en=0;st=0}}\n"
            "C.fmStg[k]=st;C.fmEnv[k]=en}}\n",
            uses_fm4 ? "C.fmAlgo=jj[12]|0;" : "", uses_fm4 ? 4 : 2);
    }
    if (uses_ks) {
        fprintf(f, "if(C.w===7&&C.i<ni){var jk=I[C.i];C.kd=jk[16]||0}\n");
    }
    /* No per-tick drum refresh: birb_synth.c reads drum_snap/tone/decay once in
     * the trigger and the generator then owns that state. Re-reading the (static)
     * instrument table every tick only overwrote trigger-derived values. */
    if (uses_formant) {
        int va_idx = formant_base + 2, vb_idx = formant_base + 3,
            sw_idx = formant_base + 4, res_idx = formant_base + 5,
            src_idx = formant_base, duty_idx = formant_base + 1;
        fprintf(f,
            "if(C.w===9&&C.i<ni){var jf=I[C.i];C.ftVa=jf[%d]>4?0:jf[%d];C.ftVb=jf[%d]>4?0:jf[%d];C.ftSS=jf[%d];C.ftR=jf[%d];var sw2=jf[%d];if(sw2!==0&&sw2!==2&&sw2!==3)sw2=2;C.ftSw=sw2;C.u=[F/8,F/4,F/2,F*3/4][jf[%d]&3]}\n",
            va_idx, va_idx, vb_idx, vb_idx, sw_idx, res_idx, src_idx, duty_idx);
    }
    fprintf(f, "}}\n");
    fprintf(f,
        "for(i=0;i<T;i++){if(tc<=0){K();tc=spt}tc--\n"
        "var v=0%s%s;for(c=0;c<N;c++){var C=ch[c];if(!C.t&&!C.e%s)continue\n"
        "var h=C.p,s;\n",
        uses_reverb ? ",rI=0" : "",
        uses_duck ? ",DI=0,DN=DE" : "",
        uses_drum ? "&&!C.drTtl" : "");
    if (uses_fm) {
        /* nops=4 → FM4 (8 algos); nops=2 → simpler op1→op0 with feedback. */
        fprintf(f,
            "if(C.w===6){%s}else \n", (uses_fm4 && uses_fm2)
                ? "if(C.fmNo>=4){s=FM4(C)}else{var l1f=C.fmL[1]*C.fmEnv[1]/F,l0f=C.fmL[0]*C.fmEnv[0]/F;var mo=SA_(C.fmp[1])*(C.fmMi*l1f/255);if(C.fmFb)mo+=C.fmPrev*C.fmFb/256;var cr_=SA_(C.fmp[0]+mo);C.fmPrev=cr_*F;C.fmp[0]=(C.fmp[0]+C.fmf[0])%F;C.fmp[1]=(C.fmp[1]+C.fmf[1])%F;s=cr_*l0f/F}"
                : uses_fm4 ? "s=FM4(C)"
                : "var l1f=C.fmL[1]*C.fmEnv[1]/F,l0f=C.fmL[0]*C.fmEnv[0]/F;var mo=SA_(C.fmp[1])*(C.fmMi*l1f/255);if(C.fmFb)mo+=C.fmPrev*C.fmFb/256;var cr_=SA_(C.fmp[0]+mo);C.fmPrev=cr_*F;C.fmp[0]=(C.fmp[0]+C.fmf[0])%F;C.fmp[1]=(C.fmp[1]+C.fmf[1])%F;s=cr_*l0f/F");
    }
    if (uses_ks) {
        fprintf(f,
            "if(C.w===7){if(C.kl<2)s=0;else{var kpp=C.kp,knx=kpp+1;if(knx>=C.kl)knx=0;var kcur=C.kb[kpp];C.kb[kpp]=(((kcur+C.kb[knx])>>1)*C.kg)>>16;C.kp=knx;s=kcur/32768}}else \n");
    }
    if (uses_drum) {
        /* New drum DSP — mirrors editor renderSong:
         *   algo 0 (kick/tom): triangle body + pitch sweep + linear-decay click
         *   algo 1 (snare):    loud noise + brief square body, no biquad
         *   algo 2 (hat/crash):HP-filtered noise (pure noise + one-pole HP)
         *   algo 3 (clap):     three triangle-enveloped noise bursts + tail */
        fprintf(f,
            "if(C.w===8){if(C.drTtl<=0){s=0;C.e=0;C.t=0}else{C.drTtl--;var o_=0;var lfn=function(){var bb=(C.drLf^(C.drLf>>1))&1;C.drLf=((C.drLf>>1)|(bb<<14))&0xFFFF;if(!C.drLf)C.drLf=0x7FFF;return C.drLf};");
        if (drum_algos & 1)
            fprintf(f,
                "if(C.drAl===0){var gp=C.drPe-C.drPet;if(gp>0)C.drPe=C.drPet+Math.floor(gp*C.drRate/65536);C.p=(C.p+(C.drPe>>8))%%F;var tri=C.p<F/2?(C.p*4-F):(F*3-C.p*4);o_=((tri*28000/F)|0);if(C.drClk>0){var nn=lfn();var pk=C.drSnap*128,ap=(pk*C.drClk/384)|0;o_+=(nn&1)?ap:-ap;C.drClk--}}");
        if (drum_algos & 2)
            fprintf(f,
                "%sif(C.drAl===1){var nn=lfn();var sr_=(nn&1)?26000:-26000;var y_=sr_-C.drZ1;C.drZ1+=(y_*C.drPet)>>16;var noi=Math.floor(y_*C.drP2/65536);C.drP2=Math.floor(C.drP2*C.drNz/65536);C.p=(C.p+C.f)%%F;var tr_=C.p<F/2?(C.p*4-F):(F*3-C.p*4);var bd=(tr_*22000)>>16;bd=Math.floor(bd*C.drPe/65536);C.drPe=Math.floor(C.drPe*C.drRate/65536);var mxB=C.drMix,mxN=255-mxB;o_=((noi*mxN+bd*mxB)/255)|0}",
                (drum_algos & 1) ? "else " : "");
        if (drum_algos & 4)
            fprintf(f,
                "%sif(C.drAl===2){var nn=lfn();var src2=(nn&1)?24000:-24000;var yh=src2-C.drZ1;C.drZ1+=(yh*C.drPet)>>16;o_=yh;if(C.drAlOrig===5){var lf=SA_((C.drTtl<<3)&0xFFFF);o_=(o_*(1+lf*0.5))|0}}",
                (drum_algos & 3) ? "else " : "");
        if (drum_algos & 8)
            fprintf(f,
                "%sif(C.drAl===3){var amp=0;if(C.drStage<3){var bL=C.drBurstLen||96;var into=bL-C.drStageT;var hf=bL/2;var env=(into<hf)?(into*256/hf):((bL-into)*256/hf);if(env<0)env=0;if(env>256)env=256;var nn=lfn();var sc=(nn&1)?26000:-26000;amp=(sc*env)>>8}else{var nn=lfn();amp=(nn&1)?9000:-9000}if(C.drStageT>0)C.drStageT--;else if(C.drStage<3){C.drStage++;C.drStageT=(C.drStage===3)?0xFFFF:(C.drBurstLen||96)}o_=amp}",
                (drum_algos & 7) ? "else " : "");
        fprintf(f, "s=o_/32768;if(s>1)s=1;else if(s<-1)s=-1}}else \n");
    }
    if (uses_formant) {
        /* Biquad intermediates can overflow JS int32 under resonance, so use
         * `/65536|0` (float div, truncate) rather than `>>16` for those paths. */
        fprintf(f,
            "if(C.w===9){var src;if(C.ftSw===3){var fv=C.ftLf,fb=(fv^(fv>>1))&1;fv=((fv>>1)|(fb<<14))&0xFFFF;if(!fv)fv=0x7FFF;C.ftLf=fv;src=(fv&1)?16383:-16383}else if(C.ftSw===0){src=h<C.u?16383:-16383}else{src=(h*2-F)*32767/F|0}var sm=0;for(var fi=0;fi<3;fi++){var b0=C.ftB0[fi],a1=C.ftA1[fi],a2=C.ftA2[fi];var yy=(b0*src/65536|0)+C.ftZ1[fi];C.ftZ1[fi]=(-(a1*yy/65536|0))+C.ftZ2[fi];C.ftZ2[fi]=(-b0*src/65536|0)-(a2*yy/65536|0);sm+=yy}if(sm>32767)sm=32767;else if(sm<-32767)sm=-32767;s=sm/32768;if(C.ftSS){C.ftRc=(C.ftRc+1)&0xFF;if((C.ftRc&0x1F)===0){var st=C.ftSS>>3;if(!st)st=1;var sp=C.ftSp+C.ftDr*st;if(sp>=255){sp=255;C.ftDr=-1}else if(sp<=0){sp=0;C.ftDr=1}C.ftSp=sp;FI(C)}}}else \n");
    }
    fprintf(f,
        "%s", "");
    /* Oscillator dispatch: only the waveforms the song actually contains.
     * A single waveform collapses to a straight assignment with no test, and
     * `else` is only emitted when a previous branch was actually written —
     * a nested-ternary version of this produced dangling `else` tokens.
     * SA_ used to be referenced unconditionally although it is only defined
     * when FM/drum/sine are present. */
    if (n_basic_waves == 0) {
        fprintf(f, "s=0\n");
    } else if (n_basic_waves == 1) {
        if (uses_pulse)      fprintf(f, "s=h<C.u?.5:-.5\n");
        else if (uses_tri)   fprintf(f, "s=h<F/2?(h*4-F)/F:(F*3-h*4)/F\n");
        else if (uses_saw)   fprintf(f, "s=(h*2-F)/F\n");
        else if (uses_sine)  fprintf(f, "s=SA_(h)\n");
        else                 fprintf(f, "C.m++;if(C.m>=C.j){C.m=0;var z=(C.h^(C.h>>1))&1;C.h=(C.h>>1)|(z<<14)}s=(C.h&1)?.5:-.5\n");
    } else {
        int first = 1;
        if (uses_pulse) { fprintf(f, "%sif(!C.w)s=h<C.u?.5:-.5\n", first?"":"else "); first=0; }
        if (uses_tri)   { fprintf(f, "%sif(C.w==1)s=h<F/2?(h*4-F)/F:(F*3-h*4)/F\n", first?"":"else "); first=0; }
        if (uses_saw)   { fprintf(f, "%sif(C.w==2)s=(h*2-F)/F\n", first?"":"else "); first=0; }
        if (uses_sine)  { fprintf(f, "%sif(C.w==4)s=SA_(h)\n", first?"":"else "); first=0; }
        if (uses_noise) { fprintf(f, "%s{C.m++;if(C.m>=C.j){C.m=0;var z=(C.h^(C.h>>1))&1;C.h=(C.h>>1)|(z<<14)}s=(C.h&1)?.5:-.5}\n", first?"":"else "); first=0; }
    }
    fprintf(f, "%s\n",
        uses_trem ? "var en=C.e+(C.tm?C.e*C.tm/F:0);if(en<0)en=0;if(en>F)en=F"
                  : "var en=C.e");
    {
        const char *padv = uses_drum ? "if(C.w!==8)" : "";
        /* per-voice drive is applied to s before the envelope; calibration and
         * ducking scale cv; the master chain replaces the bare tanh */
        const char *drv = uses_drive ? "if(C.dp>1)s=SS(s*C.dp)*C.dn;" : "";
        const char *dsend = uses_duck
            ? "if(C.ds)DI+=(cv<0?-cv:cv)*C.ds/255;if(C.da&&DN>0){var dg=1-DN*C.da/255;cv*=dg>0?dg:0}" : "";
        const char *dtick = uses_duck
            ? "var dd=DI>1?1:DI;DE=dd>DE?dd:dd+(DE-dd)*DR;" : "";
        /* one family -> one constant, so the table and its index go away */
        const char *tgain = (!uses_fm && !uses_ks && !uses_drum && !uses_formant)
            ? "(29819/65536)" : "TG[C.w]";
        const char *master = birb_no_master
            ? "v*=MG;out[i]=v>1?1:v<-1?-1:v"
            : "v*=MG;var la=v<0?-v:v;LE=la>LE?la:la+(LE-la)*MR;if(LE>MT)v*=MT/LE;out[i]=SS(v)/MC";
#ifndef BIRB_NO_REVERB
        if (uses_reverb)
            fprintf(f,
                "%svar cv=s*en*C.v*C.rv/F/255/255*%s;%sv+=cv;if(C.rs)rI+=cv*C.rs/255;%sC.p=(C.p+C.f)%%F}v+=RV(rI);%s%s}\n"
                "return{o:out,spt:spt,T:T}}\n", drv, tgain, dsend, padv, dtick, master);
        else
#endif
            fprintf(f,
                "%svar cv=s*en*C.v*C.rv/F/255/255*%s;%sv+=cv;%sC.p=(C.p+C.f)%%F}%s%s}\n"
                "return{o:out,spt:spt,T:T}}\n", drv, tgain, dsend, padv, dtick, master);
    }

    if (bc_ev) { free(bc_ev); bc_ev = NULL; bc_nev = 0; }
    fclose(f);

    /* measure output size */
    FILE *mf = fopen(filename, "rb");
    fseek(mf, 0, SEEK_END);
    long size = ftell(mf);
    fclose(mf);
    printf("Wrote %s (%ld bytes)\n", filename, size);
    return 0;
}

/* ---------- main ---------- */

/* ---------- load .bin file into song ---------- */

static int load_bin_file(const char *filename, birb_song *song) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", filename);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    fread(data, 1, (size_t)size, f);
    fclose(f);

    int result = birb_load(song, data, (int)size);
    free(data);
    if (result < 0) {
        fprintf(stderr, "Error: invalid .bin file '%s'\n", filename);
        return -1;
    }
    printf("Loaded: bpm=%d ticks=%d instruments=%d patterns=%d order=%d\n",
           song->bpm, song->ticks_per_row, song->num_instruments,
           song->num_patterns, song->order_length);
    return 0;
}


/* ------------------------------------------------------------------
 * smol C backend. Same walk and the same per-feature gating as the JS
 * one, emitting a self-contained player instead: the song is baked in,
 * so there is no loader, no pattern data and no sequencer. Replaces the
 * hand-written birb_4k.c, which was a sixth copy of the engine that
 * nothing verified against the tracker.
 * Arithmetic mirrors the JS smol export, floats included, so the two
 * can be compared sample for sample.
 * ------------------------------------------------------------------ */
/* row-time setup for the effects the C player supports */
static const char *bc_fx_setup(int arp, int slide) {
    static char buf[256];
    buf[0] = 0;
    if (arp)   strcat(buf, " if(fx==1){ a1[c]=pm>>4; a2[c]=pm&15; at[c]=0; }");
    if (slide) strcat(buf, " if(fx==2) sl[c]=pm<<2; else if(fx==3) sl[c]=-(pm<<2);");
    return buf;
}

/* voices that advance their own phase or position must not be stepped again */
static const char *bc_phase_guard(int fm, int drum, int smp) {
    static char b[64];
    b[0] = 0;
    if (!fm && !drum && !smp) return b;
    strcat(b, "if(");
    int n = 0;
    if (fm)   { strcat(b, n++ ? "&&CW[c]!=6" : "CW[c]!=6"); }
    if (drum) { strcat(b, n++ ? "&&CW[c]!=8" : "CW[c]!=8"); }
    if (smp)  { strcat(b, n++ ? "&&CW[c]!=5" : "CW[c]!=5"); }
    strcat(b, ") ");
    return b;
}

/* Mutable player state is registered as it is emitted, so the dev block can
 * mirror it without the hot path knowing anything about it. */
static char bc_st_decls[8192];
static char bc_st_names[4096];

static void bc_st_reset_reg(void) { bc_st_decls[0] = 0; bc_st_names[0] = 0; }

/* decl: the declaration without `static`, e.g. "i32 ph[N],bs[N];"
 * names: comma-separated field names to snapshot, e.g. "ph,bs" */
static void bc_st(FILE *f, const char *decl, const char *names) {
    fprintf(f, "static %s\n", decl);
    strncat(bc_st_decls, "  ", sizeof bc_st_decls - strlen(bc_st_decls) - 1);
    strncat(bc_st_decls, decl, sizeof bc_st_decls - strlen(bc_st_decls) - 1);
    strncat(bc_st_decls, "\n", sizeof bc_st_decls - strlen(bc_st_decls) - 1);
    if (bc_st_names[0]) strncat(bc_st_names, ",", sizeof bc_st_names - strlen(bc_st_names) - 1);
    strncat(bc_st_names, names, sizeof bc_st_names - strlen(bc_st_names) - 1);
}

/* the dev block: a state mirror, snapshot/restore and rewind. Compiled out
 * unless BIRB_DEV is defined, so the distributable carries none of it. */
static void bc_emit_dev(FILE *f) {
    fprintf(f,
        "\n#ifdef BIRB_DEV\n"
        "/* Editor support: snapshot, restore and rewind. The player keeps its\n"
        " * state in globals, so a host that wants several logical players swaps\n"
        " * snapshots around each render call. Seeking is reset() then rendering\n"
        " * forward and discarding. None of this exists without BIRB_DEV. */\n"
        "typedef struct {\n%s} BirbState;\n"
        "static void dcpy(void *d, const void *s, i32 n){\n"
        " char *a=(char*)d; const char *b=(const char*)s; while(n--) *a++=*b++; }\n"
        "static void dzero(void *d, i32 n){ char *a=(char*)d; while(n--) *a++=0; }\n"
        "i32 birb_state_size(void){ return (i32)sizeof(BirbState); }\n",
        bc_st_decls);

    /* save / load / reset, generated from the registered names */
    const char *dirs[3] = { "birb_save", "birb_load", "birb_reset" };
    for (int d = 0; d < 3; d++) {
        if (d == 0) fprintf(f, "void birb_save(BirbState *s){\n");
        else if (d == 1) fprintf(f, "void birb_load(const BirbState *s){\n");
        else fprintf(f, "void birb_reset(void){\n");
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", bc_st_names);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            if (d == 0)      fprintf(f, " dcpy(&s->%s, &%s, (i32)sizeof %s);\n", tok, tok, tok);
            else if (d == 1) fprintf(f, " dcpy(&%s, &s->%s, (i32)sizeof %s);\n", tok, tok, tok);
            else             fprintf(f, " dzero(&%s, (i32)sizeof %s);\n", tok, tok);
        }
        fprintf(f, "}\n");
        (void)dirs;
    }
    fprintf(f, "#endif /* BIRB_DEV */\n");
}

static int write_smol_c(const char *filename, birb_song *song) {
    FILE *f = fopen(filename, "w");
    if (!f) { fprintf(stderr, "Error: cannot write '%s'\n", filename); return -1; }

    const int nch = BIRB_NUM_CHANNELS;
    bc_st_reset_reg();
    bc_walk(song, nch);

    /* which instrument each channel plays */
    int ch_inst[BIRB_NUM_CHANNELS];
    for (int c = 0; c < nch; c++) ch_inst[c] = -1;
    for (int p = 0; p < song->num_patterns; p++)
        for (int r = 0; r < song->pattern_lengths[p]; r++)
            for (int c = 0; c < nch; c++) {
                birb_row *cl = &song->patterns[p][r][c];
                if (cl->note < 2 || cl->instrument == 0xFF) continue;
                if (ch_inst[c] < 0) ch_inst[c] = cl->instrument;
            }
    for (int c = 0; c < nch; c++) if (ch_inst[c] < 0) ch_inst[c] = 0;

    int w_used[4] = {0,0,0,0}, uses_pe = 0, unsupported = 0;
    int c_fm = 0, c_fm4 = 0, c_fm2 = 0, c_fm_mask = 0, c_rev = 0;
    int c_drum = 0, c_drum_mask = 0, c_drum_lfo = 0, c_ks = 0, c_fmt = 0, c_smp = 0;
    for (int c = 0; c < nch; c++) {
        birb_instrument *in = &song->instruments[ch_inst[c]];
        if (in->reverb_send) c_rev = 1;
        if (in->pitch_env && in->pitch_env_len) uses_pe = 1;
        if (in->synth_type == SYNTH_BASIC) { w_used[in->waveform & 3] = 1; continue; }
        if (in->synth_type == SYNTH_KS) { c_ks = 1; continue; }
        if (in->synth_type == SYNTH_FORMANT) { c_fmt = 1; continue; }
        if (in->synth_type == SYNTH_SAMPLE) { c_smp = 1; continue; }
        if (in->synth_type == SYNTH_DRUM) {
            int dt = in->drum_type & 7;
            int al = dt == 4 ? 0 : dt == 5 ? 2 : dt;
            c_drum = 1; c_drum_mask |= 1 << (al & 3);
            if (dt == 5) c_drum_lfo = 1;
            continue;
        }
        if (in->synth_type == SYNTH_FM) {
            int no = in->fm.num_ops ? in->fm.num_ops : 2;
            c_fm = 1;
            if (no >= 4) { c_fm4 = 1; c_fm_mask |= 1 << (in->fm.algorithm & 7); }
            else c_fm2 = 1;
            continue;
        }
        unsupported = 1;
    }
#ifdef BIRB_NO_REVERB
    c_rev = 0;
#endif
    if (!song->rev_wet) c_rev = 0;
    if (unsupported)
        fprintf(stderr, "  note: an unrecognised voice type is silent in the C backend\n");

    int tg_table = 0;
    for (int c = 0; c < nch; c++)
        if (song->instruments[ch_inst[c]].synth_type != SYNTH_BASIC) tg_table = 1;

    double mg = (song->master_gain ? song->master_gain : 128) / 64.0;

    fprintf(f,
        "/* Generated by birbc - do not edit, regenerate.\n"
        " * Self-contained smol player: song baked in, no loader, no pattern\n"
        " * data, no sequencer. State is global so render() can stream. */\n"
        "typedef signed char i8; typedef unsigned char u8;\n"
        "typedef short i16; typedef unsigned short u16;\n"
        "typedef int i32; typedef unsigned int u32;\n"
        "#define F 65536\n"
        "#define N %d\n"
        "#define SPT %ld\n"
        "#define TOTAL %ldL\n"
        "#define MG %.6ff\n"
        "%s",
        nch,
        (long)(44100L * 5 / ((song->bpm ? song->bpm : 125) * 2)),
        (long)bc_walk_T, mg,
        tg_table
            ? "static const float TG[10]={0.4550018311f,0.4550018311f,0.4550018311f,0.4550018311f,"
              "0.4550018311f,0.3528900146f,0.2343750000f,0.5926208496f,0.9396362305f,2.2748413086f};\n"
            : "#define TG 0.4550018311f\n");

    fprintf(f, "static const u8 CW[N]={");
    for (int c = 0; c < nch; c++) {
        birb_instrument *in = &song->instruments[ch_inst[c]];
        int wv;
        switch (in->synth_type) {
            case SYNTH_SAMPLE:  wv = 5; break;
            case SYNTH_FM:      wv = 6; break;
            case SYNTH_KS:      wv = 7; break;
            case SYNTH_DRUM:    wv = 8; break;
            case SYNTH_FORMANT: wv = 9; break;
            default:            wv = in->waveform & 3; break;
        }
        fprintf(f, "%s%d", c ? "," : "", wv);
    }
    fprintf(f, "};\n");
    if (w_used[0]) {
        fprintf(f, "static const i32 CU[N]={");
        for (int c = 0; c < nch; c++)
            fprintf(f, "%s%d", c ? "," : "", song->instruments[ch_inst[c]].duty);
        fprintf(f, "};\n");
    }
    { const char *an[4] = { "CA", "CD", "CS", "CR" };
      for (int k = 0; k < 4; k++) {
        fprintf(f, "static const u8 %s[N]={", an[k]);
        for (int c = 0; c < nch; c++) {
            birb_adsr *e = &song->instruments[ch_inst[c]].envelope;
            int v = k == 0 ? e->attack : k == 1 ? e->decay : k == 2 ? e->sustain : e->release;
            fprintf(f, "%s%d", c ? "," : "", v);
        }
        fprintf(f, "};\n");
      } }
    if (uses_pe) {
        fprintf(f, "static const i8 CPE[N]={");
        for (int c = 0; c < nch; c++)
            fprintf(f, "%s%d", c ? "," : "", song->instruments[ch_inst[c]].pitch_env);
        fprintf(f, "};\nstatic const u8 CPL[N]={");
        for (int c = 0; c < nch; c++)
            fprintf(f, "%s%d", c ? "," : "", song->instruments[ch_inst[c]].pitch_env_len);
        fprintf(f, "};\n");
    }

    if (c_fm) {
        int NO = c_fm4 ? 4 : 2;
        const char *fn[4] = { "FR", "FL", "FA", "FD" };
        for (int k = 0; k < 6; k++) {
            const char *nm = k < 4 ? fn[k] : (k == 4 ? "FS" : "FRl");
            fprintf(f, "static const i32 %s[N][%d]={", nm, NO);
            for (int c = 0; c < nch; c++) {
                birb_instrument *in = &song->instruments[ch_inst[c]];
                fprintf(f, "%s{", c ? "," : "");
                for (int o = 0; o < NO; o++) {
                    birb_fm_op *op = &in->fm.ops[o];
                    int v = 0;
                    if (in->synth_type == SYNTH_FM) switch (k) {
                        case 0: v = (op->ratio_i << 4) | (op->ratio_f & 0xF); break; /* x16 */
                        case 1: v = (int)((65536.0 * op->level) / 255.0 + 0.5); break;
                        case 2: v = op->adsr.attack; break;
                        case 3: v = op->adsr.decay; break;
                        case 4: v = op->adsr.sustain; break;
                        default: v = op->adsr.release; break;
                    }
                    fprintf(f, "%s%d", o ? "," : "", v);
                }
                fprintf(f, "}");
            }
            fprintf(f, "};\n");
        }
        fprintf(f, "static const i32 FFB[N]={");
        for (int c = 0; c < nch; c++) {
            birb_instrument *in = &song->instruments[ch_inst[c]];
            fprintf(f, "%s%d", c ? "," : "", in->synth_type == SYNTH_FM ? in->fm.feedback : 0);
        }
        fprintf(f, "};\nstatic const i32 FMI[N]={");
        for (int c = 0; c < nch; c++) {
            birb_instrument *in = &song->instruments[ch_inst[c]];
            fprintf(f, "%s%d", c ? "," : "", in->synth_type == SYNTH_FM ? in->fm.mod_index : 64);
        }
        fprintf(f, "};\n");
        if (c_fm4 && c_fm2) {
            fprintf(f, "static const u8 FNO[N]={");
            for (int c = 0; c < nch; c++) {
                birb_instrument *in = &song->instruments[ch_inst[c]];
                int no = (in->synth_type == SYNTH_FM && in->fm.num_ops) ? in->fm.num_ops : 2;
                fprintf(f, "%s%d", c ? "," : "", no);
            }
            fprintf(f, "};\n");
        }
        if (c_fm4) {
            int n_alg = 0;
            for (int a = 0; a < 8; a++) if (c_fm_mask & (1 << a)) n_alg++;
            if (n_alg > 1) {
                fprintf(f, "static const u8 FAL[N]={");
                for (int c = 0; c < nch; c++) {
                    birb_instrument *in = &song->instruments[ch_inst[c]];
                    fprintf(f, "%s%d", c ? "," : "",
                            in->synth_type == SYNTH_FM ? (in->fm.algorithm & 7) : 0);
                }
                fprintf(f, "};\n");
            }
        }
    }
    int c_arp = 0, c_slide = 0;
    for (int k = 0; k < bc_nev; k++) {
        if (bc_ev[k].fx == FX_ARPEGGIO) c_arp = 1;
        if (bc_ev[k].fx == FX_PITCH_UP || bc_ev[k].fx == FX_PITCH_DOWN) c_slide = 1;
    }
    for (int c = 0; c < nch; c++) {
        birb_instrument *in = &song->instruments[ch_inst[c]];
        if (in->arp_note1 || in->arp_note2) c_arp = 1;
    }
    int per_ch = 1;
    { int seen[BIRB_NUM_CHANNELS];
      for (int c = 0; c < nch; c++) seen[c] = -1;
      for (int p = 0; p < song->num_patterns; p++)
        for (int r = 0; r < song->pattern_lengths[p]; r++)
          for (int c = 0; c < nch; c++) {
            birb_row *cl = &song->patterns[p][r][c];
            if (cl->note < 2 || cl->instrument == 0xFF) continue;
            if (seen[c] < 0) seen[c] = cl->instrument;
            else if (seen[c] != cl->instrument) per_ch = 0;
          } }
    const int ev_fx = (c_arp || c_slide);
    if (!per_ch) {
        fprintf(stderr, "Error: smol binds one instrument per channel, and this song "
                        "plays more than one on some channel.\n"
                        "       Split those parts onto separate channels.\n");
        fclose(f);
        if (bc_ev) { free(bc_ev); bc_ev = NULL; bc_nev = 0; }
        return -1;
    }
    const int ev_w = 4 + (ev_fx ? 2 : 0);
    fprintf(f, "static const u16 EV[]={");
    { int prev = 0;
      for (int k = 0; k < bc_nev; k++) {
        fprintf(f, "%s%d,%d,%d,%d", k ? "," : "",
                bc_ev[k].t - prev, bc_ev[k].c, bc_ev[k].n, bc_ev[k].rv);
        if (ev_fx) fprintf(f, ",%d,%d", bc_ev[k].fx, bc_ev[k].pm);
        prev = bc_ev[k].t;
      } }
    fprintf(f, "};\n#define NEV %d\n", bc_nev * ev_w);

    fprintf(f,
        "/* increments in 1/256 units, shifted down after the octave shift */\n"
        "static const i32 BF[12]={6221,6591,6983,7398,7838,8304,8797,9321,9875,10462,11084,11743};\n"
        "static i32 nf(i32 n){ if(n<0)n=0; if(n>95)n=95;\n"
        " return ((BF[n%%12]<<(n/12))+128)>>8; }\n"
        "");
    bc_st(f, "i32 ph[N],bs[N],st[N],rv[N];", "ph,bs,st,rv");
    bc_st(f, "float ev_[N];", "ev_");
    if (c_arp) bc_st(f, "i32 fq[N];", "fq");
    if (c_rev) {
        fprintf(f, "static const i32 RS[N]={");
        for (int c = 0; c < nch; c++)
            fprintf(f, "%s%d", c ? "," : "", song->instruments[ch_inst[c]].reverb_send);
        fprintf(f, "};\n");
        bc_st(f, "float rc0[1116],rc1[1188],rc2[1277],rc3[1356],rcl[4];", "rc0,rc1,rc2,rc3,rcl");
        bc_st(f, "i32 rcp[4];", "rcp");
        fprintf(f,
            "static float *const rcb[4]={rc0,rc1,rc2,rc3};\n"
            "static const i32 rcn[4]={1116,1188,1277,1356};\n"
            "");
        {   double size = song->rev_size / 255.0, damp = song->rev_damp / 255.0;
            double wet = song->rev_wet / 255.0;
            double fb = 0.7 + 0.28 * size, dc = 0.4 * damp;
            fprintf(f,
                "static float rev_(float x){ float o=0.f;\n"
                " for(i32 k=0;k<4;k++){ i32 L=rcn[k],pp=rcp[k]; float y=rcb[k][pp];\n"
                "  rcl[k]=y*%.6ff+rcl[k]*%.6ff; rcb[k][pp]=x+rcl[k]*%.6ff;\n"
                "  rcp[k]=(pp+1<L)?pp+1:0; o+=y; }\n"
                /* the C player is always smol, so the diffusers are never emitted */
                " o*=%.6ff;\n"
                " return o*%.6ff; }\n",
                1.0 - dc, dc, fb, (1.0 - fb) * 5.5, wet);
        }
    }
    if (uses_pe)   bc_st(f, "i32 pet[N];", "pet");
    if (c_arp) {
        bc_st(f, "i32 a1[N],a2[N],at[N],bn[N];", "a1,a2,at,bn");
        fprintf(f, "static const i32 IA1[N]={");
        for (int c = 0; c < nch; c++)
            fprintf(f, "%s%d", c ? "," : "", song->instruments[ch_inst[c]].arp_note1);
        fprintf(f, "};\nstatic const i32 IA2[N]={");
        for (int c = 0; c < nch; c++)
            fprintf(f, "%s%d", c ? "," : "", song->instruments[ch_inst[c]].arp_note2);
        fprintf(f, "};\n");
    }
    if (c_slide)   bc_st(f, "i32 sl[N];", "sl");
    if (c_fm) {
        int NO = c_fm4 ? 4 : 2;
        { char d1[96], d2[96];
          snprintf(d1, sizeof d1, "i32 fp[N][%d],ff[N][%d],fst[N][%d];", NO, NO, NO);
          snprintf(d2, sizeof d2, "float fe[N][%d],fpv[N];", NO);
          bc_st(f, d1, "fp,ff,fst"); bc_st(f, d2, "fe,fpv"); }
    }
    if (c_fm || c_drum_lfo)
        fprintf(f,
            "/* same rational sine approximation as the JS engine */\n"
            "static float sa_(i32 ph){ i32 p=((ph%%F)+F)%%F; i32 ng=p>=F/2; i32 t=ng?p-F/2:p;\n"
            " if(t>F/4) t=F/2-t; float x=(float)t/F*4.f; if(x>1.f)x=1.f; float x2=x*x;\n"
            " float y=x*(1.5707288f-x2*(0.6432292f-x2*0.0727778f)); if(y>1.f)y=1.f;\n"
            " return ng?-y:y; }\n");
    if (c_ks) {
        fprintf(f, "static const i32 KD[N]={");
        for (int c = 0; c < nch; c++) {
            birb_instrument *in = &song->instruments[ch_inst[c]];
            fprintf(f, "%s%d", c ? "," : "",
                    in->synth_type == SYNTH_KS ? in->ks_damping : 0);
        }
        fprintf(f,
            "};\n"
            "static const i32 KQ[33]={657,776,916,1082,1277,1508,1781,2103,2484,2933,3463,4089,4829,5702,6733,7951,9388,11086,13091,15458,18253,21554,25452,30054,35489,41907,49485,58434,69001,81479,96213,113612,131398};\n"
            "static i32 KG(i32 d,i32 l){ i32 i=d>>3,fr=d&7,q=KQ[i];\n"
            " q+=((KQ[i+1]-q)*fr)>>3; i32 a=(i32)(((long long)q*l)>>8);\n"
            " return a>=65535?0:65535-a; }\n"
            "");
        bc_st(f, "i16 kb[N][1024];", "kb");
        bc_st(f, "i32 kl[N],kp[N],kg[N];", "kl,kp,kg");
    }
    if (c_smp) {
        fprintf(f, "static const i16 SPOOL[]={");
        for (uint32_t k = 0; k < song->sample_pool_used; k++)
            fprintf(f, "%s%d", k ? "," : "", (int)song->sample_pool[k]);
        if (!song->sample_pool_used) fprintf(f, "0");
        fprintf(f, "};\n");
        const char *sn[5] = { "SOFF", "SLEN", "SLS", "SLE", "SBN" };
        for (int k = 0; k < 5; k++) {
            fprintf(f, "static const i32 %s[]={", sn[k]);
            for (int i = 0; i < song->num_samples; i++) {
                birb_sample_meta *m = &song->samples[i];
                long v = k == 0 ? (long)m->offset : k == 1 ? (long)m->length
                       : k == 2 ? (long)(int32_t)m->loop_start
                       : k == 3 ? (long)m->loop_end : (long)m->base_note;
                fprintf(f, "%s%ld", i ? "," : "", v);
            }
            if (!song->num_samples) fprintf(f, "0");
            fprintf(f, "};\n");
        }
        fprintf(f, "static const i32 CSI[N]={");
        for (int c = 0; c < nch; c++) {
            birb_instrument *in = &song->instruments[ch_inst[c]];
            fprintf(f, "%s%d", c ? "," : "",
                    in->synth_type == SYNTH_SAMPLE ? in->sample_idx : 0);
        }
        fprintf(f, "};\n");
        bc_st(f, "i32 sp_[N],ss_[N],si_[N],sa_on[N];", "sp_,ss_,si_,sa_on");
    }
    if (c_fmt) {
        fprintf(f, "static const i32 FCO[N][2][9]={");
        for (int c = 0; c < nch; c++) {
            birb_instrument *in = &song->instruments[ch_inst[c]];
            int32_t cf[2][3][3];
            double q = 2.0 + (in->formant_resonance / 255.0) * 30.0;
            if (in->synth_type == SYNTH_FORMANT) {
                bc_formant_coeffs(in->formant_vowel_a, q, cf[0]);
                bc_formant_coeffs(in->formant_vowel_b, q, cf[1]);
            } else {
                for (int v = 0; v < 2; v++) for (int i = 0; i < 3; i++)
                    for (int k = 0; k < 3; k++) cf[v][i][k] = 0;
            }
            fprintf(f, "%s{", c ? "," : "");
            for (int v = 0; v < 2; v++) {
                fprintf(f, "%s{", v ? "," : "");
                for (int i = 0; i < 3; i++)
                    for (int k = 0; k < 3; k++)
                        fprintf(f, "%s%d", (i || k) ? "," : "", (int)cf[v][i][k]);
                fprintf(f, "}");
            }
            fprintf(f, "}");
        }
        fprintf(f, "};\n");
        const char *fn2[4] = { "FTSW", "FTDU", "FTSS", "FTRS" };
        for (int k = 0; k < 4; k++) {
            fprintf(f, "static const i32 %s[N]={", fn2[k]);
            for (int c = 0; c < nch; c++) {
                birb_instrument *in = &song->instruments[ch_inst[c]];
                int v = 0;
                if (in->synth_type == SYNTH_FORMANT) switch (k) {
                    case 0: v = in->formant_source_wave; break;
                    case 1: v = in->formant_duty & 3; break;
                    case 2: v = in->formant_sweep_speed; break;
                    default: v = in->formant_resonance; break;
                }
                fprintf(f, "%s%d", c ? "," : "", v);
            }
            fprintf(f, "};\n");
        }
        bc_st(f, "i32 ftz1[N][3],ftz2[N][3],ftb0[N][3],fta1[N][3],fta2[N][3];",
                 "ftz1,ftz2,ftb0,fta1,fta2");
        bc_st(f, "i32 ftlf[N],ftsp[N],ftdr[N],ftrc[N],CUv[N];",
                 "ftlf,ftsp,ftdr,ftrc,CUv");
        fprintf(f,
            "static void FI(i32 c){ i32 t=ftsp[c], o=255-t;\n"
            " for(i32 i=0;i<3;i++){\n"
            "  ftb0[c][i]=(FCO[c][0][i*3+0]*o+FCO[c][1][i*3+0]*t)/255;\n"
            "  fta1[c][i]=(FCO[c][0][i*3+1]*o+FCO[c][1][i*3+1]*t)/255;\n"
            "  fta2[c][i]=(FCO[c][0][i*3+2]*o+FCO[c][1][i*3+2]*t)/255; } }\n");
    }
    if (c_drum) {
        /* per-channel drum parameters, and the two coefficient tables the
         * kick sweep and the snare high-pass interpolate from */
        const char *dn[5] = { "DT", "DTU", "DDC", "DTO", "DSN" };
        for (int k = 0; k < 5; k++) {
            fprintf(f, "static const i32 %s[N]={", dn[k]);
            for (int c = 0; c < nch; c++) {
                birb_instrument *in = &song->instruments[ch_inst[c]];
                int v = 0;
                if (in->synth_type == SYNTH_DRUM) switch (k) {
                    case 0: v = in->drum_type & 7; break;
                    case 1: v = (signed char)in->drum_tune; break;
                    case 2: v = in->drum_decay; break;
                    case 3: v = in->drum_tone; break;
                    default: v = in->drum_snap; break;
                }
                fprintf(f, "%s%d", c ? "," : "", v);
            }
            fprintf(f, "};\n");
        }
        bc_st(f, "i32 dal[N],dor[N],dp2[N],dnz[N],dpe[N],dpt[N],drt[N],dsn[N],"
                 "dclk[N],dz1[N],dlf[N],dttl[N],dmix[N],dstg[N],dstt[N],dbl[N];",
                 "dal,dor,dp2,dnz,dpe,dpt,drt,dsn,dclk,dz1,dlf,dttl,dmix,dstg,dstt,dbl");
        fprintf(f,
            "static i32 dlfn(i32 c){ i32 b=(dlf[c]^(dlf[c]>>1))&1;\n"
            "  dlf[c]=((dlf[c]>>1)|(b<<14))&0xFFFF; if(!dlf[c]) dlf[c]=0x7FFF; return dlf[c]; }\n");
        if (c_drum_mask & 1)
            fprintf(f,
                "static const i32 KSW[33]={65518,65516,65512,65509,65505,65500,65495,65489,65482,65474,65465,65454,65442,65428,65412,65393,65372,65348,65320,65287,65251,65208,65160,65104,65040,64966,64882,64785,64674,64546,64400,64233,64067};\n"
                "static i32 KC(i32 t){ i32 i=t>>3,fr=t&7,c=KSW[i]; return c+(((KSW[i+1]-c)*fr)>>3); }\n");
        if (c_drum_mask & 2)
            fprintf(f,
                "static const i32 SHP[33]={1386,1545,1722,1920,2139,2383,2655,2956,3291,3663,4076,4533,5039,5600,6219,6903,7657,8487,9400,10401,11498,12697,14004,15424,16964,18627,20416,22332,24376,26543,28828,31221,33392};\n"
                "static i32 SC(i32 t){ i32 i=t>>3,fr=t&7,c=SHP[i]; return c+(((SHP[i+1]-c)*fr)>>3); }\n");
    }
    if (w_used[3]) bc_st(f, "i32 lf[N],lc[N],lp[N];", "lf,lc,lp");
    bc_st(f, "i32 ei,tk,et,tc;", "ei,tk,et,tc");
    fprintf(f,
        "static i16 out[4096];\n"
        "i16 *outPtr(void){ return out; }\n"
        "u32 getOutputBuf(void){ return (u32)(unsigned long)out; }\n"
        "i32 getLength(void){ return (i32)TOTAL; }\n");

    fprintf(f,
        "static void trig(i32 c,i32 n){ i32 s=n-2;\n"
        " bs[c]=nf(s); ph[c]=0; rv[c]=255;%s\n"
        " if(CA[c]==0){ ev_[c]=(float)F; st[c]=2; } else { ev_[c]=0.f; st[c]=1; }\n",
        c_arp ? (c_slide ? " bn[c]=s; a1[c]=IA1[c]; a2[c]=IA2[c]; at[c]=0; sl[c]=0;"
                         : " bn[c]=s; a1[c]=IA1[c]; a2[c]=IA2[c]; at[c]=0;")
               : (c_slide ? " sl[c]=0;" : ""));
    if (uses_pe)   fprintf(f, " pet[c]=CPL[c];\n");
    if (w_used[3]) fprintf(f, " if(CW[c]==3){ lf[c]=0x7FFF; lc[c]=0; lp[c]=(256>>(s/12))?(256>>(s/12)):1; }\n");
    if (c_smp)
        fprintf(f,
            " sa_on[c]=0;\n"
            " if(CW[c]==5){ i32 idx=CSI[c]; si_[c]=idx; sp_[c]=0; sa_on[c]=1;\n"
            "  i32 bf2=nf(SBN[idx]);\n"
            "  ss_[c] = bf2>0 ? (i32)(((long long)bs[c]*65536 + bf2/2)/bf2) : 65536; }\n");
    if (c_fmt)
        fprintf(f,
            " if(CW[c]==9){ i32 sw=FTSW[c]; if(sw!=0&&sw!=2&&sw!=3) sw=2;\n"
            "  ftlf[c]=(0x7FFF^((s*0x2BCD)&0xFFFF))&0xFFFF; if(!ftlf[c]) ftlf[c]=0x7FFF;\n"
            "  ftsp[c]=0; ftdr[c]=1; ftrc[c]=0;\n"
            "  for(i32 i=0;i<3;i++){ ftz1[c][i]=0; ftz2[c][i]=0; }\n"
            "  { static const i32 dvt[4]={F/8,F/4,F/2,F*3/4}; CUv[c]=dvt[FTDU[c]&3]; }\n"
            "  FI(c); }\n");
    if (c_ks)
        fprintf(f,
            " if(CW[c]==7){ i32 ln = bs[c]>0 ? (F/bs[c]) : 0;\n"
            "  if(ln<4) ln=4; if(ln>1024) ln=1024; kl[c]=ln; kp[c]=0; kg[c]=KG(KD[c],ln);\n"
            "  i32 lfv=(0x7FFF^((s*0x1D79)&0xFFFF))&0xFFFF; if(!lfv) lfv=0x7FFF;\n"
            "  for(i32 ki=0;ki<ln;ki++){ i32 kbv=(lfv^(lfv>>1))&1;\n"
            "   lfv=((lfv>>1)|(kbv<<14))&0xFFFF; kb[c][ki]=(lfv&1)?32767:-32767; } }\n");
    if (c_drum) {
        fprintf(f,
            " if(CW[c]==8){ i32 dt=DT[c], al=dt==4?0:dt==5?2:dt, tn=DTU[c];\n"
            "  i32 dec=DDC[c], tone=DTO[c], snp=DSN[c], dn=s+tn;\n"
            "  if(dn<0)dn=0; if(dn>95)dn=95; i32 df=nf(dn), tt=0;\n"
            "  dal[c]=al; dor[c]=dt; dp2[c]=0; dz1[c]=0;\n"
            "  dlf[c]=(0x7FFF^((s*0x3D7F)&0xFFFF))&0xFFFF; if(!dlf[c]) dlf[c]=0x7FFF;\n");
        if (c_drum_mask & 1)
            fprintf(f,
                "  if(al==0){ dpe[c]=(df<<3)<<8; dpt[c]=(df>>1)<<8; drt[c]=KC(tone);\n"
                "   dsn[c]=snp; dclk[c]=384; tt=dec*200+1024; if(dt==4) tt*=2; }\n");
        if (c_drum_mask & 2)
            fprintf(f,
                "  %sif(al==1){ bs[c]=df>0?df:nf(26); dmix[c]=snp; dpt[c]=SC(tone);\n"
                "   dpe[c]=F; drt[c]=65460; dz1[c]=0; tt=dec*120+1024; dp2[c]=F;\n"
                "   i32 q=301466/(tt?tt:1); dnz[c]=F-(q<4096?q:4096); }\n",
                (c_drum_mask & 1) ? "else " : "");
        if (c_drum_mask & 4)
            fprintf(f,
                "  %sif(al==2){ i32 hp=(i32)(snp*(F*15.0f/16.0f/255.0f)); if(hp<(F>>4)) hp=F>>4;\n"
                "   dpt[c]=hp; dz1[c]=0; tt=dec*180+1024; if(dt==5) tt=90000+dec*400; }\n",
                (c_drum_mask & 3) ? "else " : "");
        if (c_drum_mask & 8)
            fprintf(f,
                "  %s{ dbl[c]=80+(snp>>1); dstg[c]=0; dstt[c]=dbl[c]; tt=dec*160+2048; }\n",
                (c_drum_mask & 7) ? "else " : "");
        fprintf(f, "  if(tt>0xFFFFFF) tt=0xFFFFFF; dttl[c]=tt; }\n");
    }
    if (c_fm) {
        int NO = c_fm4 ? 4 : 2;
        fprintf(f,
            " if(CW[c]==6){ fpv[c]=0.f;\n"
            "  for(i32 k=0;k<%d;k++){ fp[c][k]=0; ff[c][k]=(bs[c]*FR[c][k]+8)>>4;\n"
            "   if(FA[c][k]==0){ fe[c][k]=(float)F; fst[c][k]=2; } else { fe[c][k]=0.f; fst[c][k]=1; } } }\n", NO);
    }
    fprintf(f, "}\n");

    fprintf(f,
        "static void tickf(void){\n"
        " while(ei<NEV && et+(i32)EV[ei]<=tk){ et+=(i32)EV[ei];\n"
        "  i32 c=EV[ei+1],n=EV[ei+2],v=EV[ei+3];%s ei+=%d;\n"
        "  if(n==1) st[c]=4; else if(n>=2) trig(c,n);\n"
        "  if(v) rv[c]=v;%s }\n"
        " tk++;\n"
        " for(i32 c=0;c<N;c++){\n",
        ev_fx ? " i32 fx=EV[ei+4],pm=EV[ei+5];" : "",
        ev_w,
        ev_fx ? bc_fx_setup(c_arp, c_slide) : "");
    if (uses_pe)
        fprintf(f, "  if(pet[c]){ bs[c]+=(i32)CPE[c]<<2; if(bs[c]<1)bs[c]=1; pet[c]--; }\n");
    if (c_slide)
        fprintf(f, "  if(sl[c]){ bs[c]+=sl[c]; if(bs[c]<1)bs[c]=1; }\n");
    if (c_arp)
        fprintf(f, "  if(a1[c]|a2[c]){ i32 t3=at[c]%%3, nn=bn[c];\n"
                   "   if(t3==1) nn+=a1[c]; else if(t3==2) nn+=a2[c];\n"
                   "   fq[c]=nf(nn); at[c]++; } else fq[c]=bs[c];\n");
    fprintf(f,
        "  i32 e=st[c];\n"
        "  if(e==1){ ev_[c]+=(float)F/(CA[c]+1); if(ev_[c]>=F){ ev_[c]=(float)F; st[c]=2; } }\n"
        "  else if(e==2){ float g=(float)F*CS[c]/255; ev_[c]-=((float)F-g)/(CD[c]+1); if(ev_[c]<=g){ ev_[c]=g; st[c]=3; } }\n"
        "  else if(e==4){ ev_[c]-=ev_[c]/(CR[c]+1); if(ev_[c]<64){ ev_[c]=0.f; st[c]=0; } }\n");
    if (c_fm) {
        int NO = c_fm4 ? 4 : 2;
        fprintf(f,
            "  if(CW[c]==6) for(i32 k=0;k<%d;k++){ ff[c][k]=(%s*FR[c][k]+8)>>4;\n"
            "   i32 t2=fst[c][k]; float en=fe[c][k];\n"
            "   if(t2==1){ en+=(float)F/(FA[c][k]+1); if(en>=F){ en=(float)F; t2=2; } }\n"
            "   else if(t2==2){ float g=(float)F*FS[c][k]/255; en-=((float)F-g)/(FD[c][k]+1); if(en<=g){ en=g; t2=3; } }\n"
            "   else if(t2==4){ en-=en/(FRl[c][k]+1); if(en<64){ en=0.f; t2=0; } }\n"
            "   fst[c][k]=t2; fe[c][k]=en; }\n", NO, c_arp ? "fq[c]" : "bs[c]");
    }
    fprintf(f, " }\n}\n");

    /* render: only the waveforms this song reaches */
    fprintf(f,
        "void render(i32 n){\n"
        " for(i32 i=0;i<n;i++){\n"
        "  if(tc<=0){ tickf(); tc=SPT; }\n"
        "  tc--;\n"
        "  float v=0.f%s;\n"
        "  for(i32 c=0;c<N;c++){\n"
        "   if(!st[c] && ev_[c]==0.f%s) continue;\n"
        "   i32 h=ph[c]; float s=0.f;\n", c_rev ? ",ri=0.f" : "",
        c_drum ? " && !dttl[c]" : "");
    { int first = 1;
      if (c_smp) {
        fprintf(f,
            "   %s(CW[c]==5){ if(sa_on[c]){ i32 idx=si_[c], pos=(i32)((u32)sp_[c]>>16);\n"
            "    if(pos>=SLEN[idx]){\n"
            "     if(SLS[idx]>=0 && SLE[idx]>SLS[idx]){ i32 ll=SLE[idx]-SLS[idx];\n"
            "      pos=SLS[idx]+((pos-SLS[idx])%%ll); sp_[c]=(pos<<16)|(sp_[c]&0xFFFF); }\n"
            "     else { sa_on[c]=0; s=0.f; } }\n"
            "    if(sa_on[c]){ s=(float)SPOOL[SOFF[idx]+pos]/32768.f;\n"
            "     sp_[c]=(i32)((u32)sp_[c]+(u32)ss_[c]); } } }\n", first?"if":"else if");
        first = 0;
      }
      if (c_fmt) {
        fprintf(f,
            "   %s(CW[c]==9){ i32 src;\n"
            "    if(FTSW[c]==3){ i32 fv=ftlf[c], fb2=(fv^(fv>>1))&1;\n"
            "     fv=((fv>>1)|(fb2<<14))&0xFFFF; if(!fv) fv=0x7FFF; ftlf[c]=fv;\n"
            "     src=(fv&1)?16383:-16383; }\n"
            "    else if(FTSW[c]==0){ src = h<CUv[c] ? 16383 : -16383; }\n"
            "    else { src=(i32)(((long long)(h*2-F)*32767)/F); }\n"
            "    i32 sm=0;\n"
            "    for(i32 fi=0;fi<3;fi++){\n"
            "     i32 b0=ftb0[c][fi],a1=fta1[c][fi],a2=fta2[c][fi];\n"
            "     i32 yy=(i32)(((long long)b0*src)/65536)+ftz1[c][fi];\n"
            "     ftz1[c][fi]=(-(i32)(((long long)a1*yy)/65536))+ftz2[c][fi];\n"
            "     ftz2[c][fi]=(-(i32)(((long long)b0*src)/65536))-(i32)(((long long)a2*yy)/65536);\n"
            "     sm+=yy; }\n"
            "    if(sm>32767) sm=32767; else if(sm<-32767) sm=-32767;\n"
            "    s=(float)sm/32768.f;\n"
            "    if(FTSS[c]){ ftrc[c]=(ftrc[c]+1)&0xFF;\n"
            "     if((ftrc[c]&0x1F)==0){ i32 stp=FTSS[c]>>3; if(!stp) stp=1;\n"
            "      i32 sp=ftsp[c]+ftdr[c]*stp;\n"
            "      if(sp>=255){ sp=255; ftdr[c]=-1; } else if(sp<=0){ sp=0; ftdr[c]=1; }\n"
            "      ftsp[c]=sp; FI(c); } } }\n", first?"if":"else if");
        first = 0;
      }
      if (c_ks) {
        fprintf(f,
            "   %s(CW[c]==7){ if(kl[c]<2) s=0.f; else {\n"
            "    i32 kpp=kp[c], knx=kpp+1; if(knx>=kl[c]) knx=0;\n"
            "    i32 kcur=kb[c][kpp];\n"
            "    kb[c][kpp]=(i16)((i32)(((kcur+kb[c][knx])>>1)*kg[c])>>16);\n"
            "    kp[c]=knx; s=(float)kcur/32768.f; } }\n", first?"if":"else if");
        first = 0;
      }
      if (c_drum) {
        fprintf(f,
            "   %s(CW[c]==8){ if(dttl[c]<=0){ s=0.f; ev_[c]=0.f; st[c]=0; } else { dttl[c]--;\n"
            "    i32 o_=0;\n", first?"if":"else if");
        if (c_drum_mask & 1)
            fprintf(f,
                "    if(dal[c]==0){ i32 gp=dpe[c]-dpt[c];\n"
                "     if(gp>0) dpe[c]=dpt[c]+(i32)(((long long)gp*drt[c])>>16);\n"
                "     ph[c]=(ph[c]+(dpe[c]>>8))%%F;\n"
                "     i32 tri = ph[c]<F/2 ? (ph[c]*4-F) : (F*3-ph[c]*4);\n"
                "     o_=(i32)(((long long)tri*28000)/F);\n"
                "     if(dclk[c]>0){ i32 nn=dlfn(c), pk=dsn[c]*128, ap=(i32)(((long long)pk*dclk[c])/384);\n"
                "      o_ += (nn&1)?ap:-ap; dclk[c]--; } }\n");
        if (c_drum_mask & 2)
            fprintf(f,
                "    %sif(dal[c]==1){ i32 nn=dlfn(c), sr_=(nn&1)?26000:-26000, y_=sr_-dz1[c];\n"
                "     dz1[c] += (i32)((i32)(u32)((long long)y_*dpt[c])>>16);\n"
                "     i32 noi=(i32)(((long long)y_*dp2[c])>>16);\n"
                "     dp2[c]=(i32)(((long long)dp2[c]*dnz[c])>>16);\n"
                "     ph[c]=(ph[c]+bs[c])%%F;\n"
                "     i32 tr_ = ph[c]<F/2 ? (ph[c]*4-F) : (F*3-ph[c]*4);\n"
                "     i32 bd=(i32)((i32)(u32)((long long)tr_*22000)>>16);\n"
                "     bd=(i32)(((long long)bd*dpe[c])>>16);\n"
                "     dpe[c]=(i32)(((long long)dpe[c]*drt[c])>>16);\n"
                "     i32 mxB=dmix[c], mxN=255-mxB;\n"
                "     o_=(i32)(((long long)noi*mxN+(long long)bd*mxB)/255); }\n",
                (c_drum_mask & 1) ? "else " : "");
        if (c_drum_mask & 4)
            fprintf(f,
                "    %sif(dal[c]==2){ i32 nn=dlfn(c), src2=(nn&1)?24000:-24000, yh=src2-dz1[c];\n"
                "     dz1[c] += (i32)((i32)(u32)((long long)yh*dpt[c])>>16); o_=yh;\n"
                "%s"
                "    }\n",
                (c_drum_mask & 3) ? "else " : "",
                c_drum_lfo
                  ? "     if(dor[c]==5){ float lo=sa_((dttl[c]<<3)&0xFFFF); o_=(i32)(o_*(1.f+lo*0.5f)); }\n"
                  : "");
        if (c_drum_mask & 8)
            fprintf(f,
                "    %s{ i32 amp=0;\n"
                "     if(dstg[c]<3){ i32 bL=dbl[c]?dbl[c]:96, into=bL-dstt[c], hf=bL/2;\n"
                "      i32 en2 = (into<hf) ? (into*256/hf) : ((bL-into)*256/hf);\n"
                "      if(en2<0) en2=0; if(en2>256) en2=256;\n"
                "      i32 nn=dlfn(c), sc=(nn&1)?26000:-26000; amp=(sc*en2)>>8; }\n"
                "     else { i32 nn=dlfn(c); amp=(nn&1)?9000:-9000; }\n"
                "     if(dstt[c]>0) dstt[c]--;\n"
                "     else if(dstg[c]<3){ dstg[c]++; dstt[c]=(dstg[c]==3)?0xFFFF:(dbl[c]?dbl[c]:96); }\n"
                "     o_=amp; }\n",
                (c_drum_mask & 7) ? "else " : "");
        fprintf(f,
            "    s=(float)o_/32768.f; if(s>1.f)s=1.f; else if(s<-1.f)s=-1.f; } }\n");
        first = 0;
      }
      if (c_fm) {
        /* same algorithm arms as the JS emitter, only the ones reached */
        static const char *CFM_ALG[8] = {
          "s3=sa_(fp[c][3]+fb)*l3; s2=sa_(fp[c][2]+(i32)s3)*l2; s1=sa_(fp[c][1]+(i32)s2)*l1; raw=sa_(fp[c][0]+(i32)(s1*mi)); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s2=sa_(fp[c][2])*l2; s1=sa_(fp[c][1]+(i32)s3+(i32)s2)*l1; raw=sa_(fp[c][0]+(i32)(s1*mi)); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s2=sa_(fp[c][2]+(i32)s3)*l2; s1=sa_(fp[c][1])*l1; raw=sa_(fp[c][0]+(i32)((s2+s1)*mi)); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s1=sa_(fp[c][1]+(i32)s3)*l1; s2=sa_(fp[c][2])*l2; raw=sa_(fp[c][0]+(i32)((s1+s2)*mi)); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s2=sa_(fp[c][2])*l2; s1=sa_(fp[c][1])*l1; raw=sa_(fp[c][0]+(i32)((s3+s2+s1)*mi)); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s1=sa_(fp[c][1])*l1; { float r2=sa_(fp[c][2]+(i32)(s3*mi)); raw=sa_(fp[c][0]+(i32)(s1*mi)); sv=(r2*l2+raw*l0)/F*0.5f; }",
          "s3=sa_(fp[c][3]+fb)*l3; { float r2=sa_(fp[c][2]+(i32)(s3*mi)),r1=sa_(fp[c][1]); raw=sa_(fp[c][0]); sv=(r2*l2+r1*l1+raw*l0)/F/3.f; }",
          "{ float r3=sa_(fp[c][3]+fb),r2=sa_(fp[c][2]),r1=sa_(fp[c][1]); raw=sa_(fp[c][0]); sv=(r3*l3+r2*l2+r1*l1+raw*l0)/F*0.25f; }",
        };
        int NO = c_fm4 ? 4 : 2, n_alg = 0, last = 0;
        for (int a = 0; a < 8; a++) if (c_fm_mask & (1 << a)) { n_alg++; last = a; }
        fprintf(f, "   %s(CW[c]==6){ float sv=0.f,raw=0.f;\n"
                   "    float l0=fe[c][0]*FL[c][0]/F, l1=fe[c][1]*FL[c][1]/F;\n",
                   first?"if":"else if");
        if (c_fm4) fprintf(f, "    float l2=fe[c][2]*FL[c][2]/F, l3=fe[c][3]*FL[c][3]/F;\n");
        fprintf(f, "    float mi=(float)FMI[c]/255.f, fb=FFB[c]?fpv[c]*FFB[c]/256.f:0.f;\n");
        if (c_fm4) fprintf(f, "    float s1=0.f,s2=0.f,s3=0.f;\n");
        if (c_fm4 && c_fm2) fprintf(f, "    if(FNO[c]>=4){\n");
        if (c_fm4) {
            if (n_alg == 1) fprintf(f, "    %s\n", CFM_ALG[last]);
            else {
                fprintf(f, "    switch(FAL[c]&7){\n");
                for (int a = 0; a < 8; a++) {
                    if (!(c_fm_mask & (1 << a))) continue;
                    if (a == last) fprintf(f, "     default: %s break;\n", CFM_ALG[a]);
                    else           fprintf(f, "     case %d: %s break;\n", a, CFM_ALG[a]);
                }
                fprintf(f, "    }\n");
            }
        }
        if (c_fm4 && c_fm2) fprintf(f, "    } else {\n");
        if (c_fm2)
            fprintf(f,
                "    float mo=sa_(fp[c][1])*((float)FMI[c]*l1/255.f);\n"
                "    if(FFB[c]) mo+=fpv[c]*FFB[c]/256.f;\n"
                "    raw=sa_(fp[c][0]+(i32)mo); sv=raw*l0/F;\n");
        if (c_fm4 && c_fm2) fprintf(f, "    }\n");
        fprintf(f,
            "    fpv[c]=raw*F;\n"
            "    for(i32 k=0;k<%d;k++) fp[c][k]=(fp[c][k]+ff[c][k])%%F;\n"
            "    s=sv; }\n", NO);
        first = 0;
      }
      if (w_used[0]) { fprintf(f, "   %s(CW[c]==0){ s = h<CU[c] ? .5f : -.5f; }\n", first?"if":"else if"); first=0; }
      if (w_used[1]) { fprintf(f, "   %s(CW[c]==1){ s = h<F/2 ? (float)(h*4-F)/F : (float)(F*3-h*4)/F; }\n", first?"if":"else if"); first=0; }
      if (w_used[2]) { fprintf(f, "   %s(CW[c]==2){ s = (float)(h*2-F)/F; }\n", first?"if":"else if"); first=0; }
      if (w_used[3]) {
        fprintf(f, "   %s{ lc[c]++; if(lc[c]>=lp[c]){ lc[c]=0; i32 b=(lf[c]^(lf[c]>>1))&1; lf[c]=(lf[c]>>1)|(b<<14); }\n"
                   "     s = (lf[c]&1) ? .5f : -.5f; }\n", first?"" : "else ");
        first=0;
      } }
    fprintf(f,
        "   float cv = s * ev_[c] / F * (float)rv[c] / 255.f * %s;\n"
        "   v += cv;%s\n"
        "   %sph[c] = (ph[c]+%s) %% F;\n"
        "  }\n"
        "%s"
        "  v *= MG;\n"
        "  if(v>1.f) v=1.f; else if(v<-1.f) v=-1.f;\n"
        "  out[i] = (i16)(v*32767.f);\n"
        " }\n}\n",
        tg_table ? "TG[CW[c]]" : "TG",
        c_rev ? " if(RS[c]) ri += cv * RS[c] / 255.f;" : "",
        bc_phase_guard(c_fm, c_drum, c_smp), c_arp ? "fq[c]" : "bs[c]",
        c_rev ? "  v += rev_(ri);\n" : "");

    bc_emit_dev(f);
    fclose(f);
    fprintf(stderr, "Wrote %s (%d events, %ld samples)\n", filename, bc_nev, (long)bc_walk_T);
    if (bc_ev) { free(bc_ev); bc_ev = NULL; bc_nev = 0; }
    return 0;
}

/* ==================================================================
 * Locked-down C player, full feature set (--locked-c).
 *
 * The shape smol-c established - song baked in, event list, no loader, no
 * pattern data, no sequencer, only the code the song reaches - carrying
 * everything the tracker can play rather than smol's reduced set. Separate
 * from write_smol_c on purpose: that one exists to be small and its
 * structure encodes the drops, so threading a full feature set through it
 * fights the design at every branch.
 *
 * Parity target is the full JS export, which is the tracker's own engine.
 * Arithmetic is float wherever the JS is float so the two can be compared
 * sample for sample; the fixed-point of birb_synth.c is a different domain
 * and is not what this mirrors.
 *
 * Voice slots are tracker channels, parameter tables are indexed by
 * instrument, and a note-on rebinds its slot's row.
 * ================================================================== */

/* emit one per-instrument table of ints */
static void lk_tab(FILE *f, const char *type, const char *name, int ni,
                   const int *vals) {
    fprintf(f, "static const %s %s[NI]={", type, name);
    for (int i = 0; i < ni; i++) fprintf(f, "%s%d", i ? "," : "", vals[i]);
    fprintf(f, "};\n");
}

static int write_locked_c(const char *filename, birb_song *song) {
    FILE *f = fopen(filename, "w");
    if (!f) { fprintf(stderr, "Error: cannot write '%s'\n", filename); return -1; }

    /* channels the song authored: the loader pads order columns past the
     * song's own count with 0xFF, so the highest column carrying a pattern is
     * the width. */
    int nch = 0;
    for (int i = 0; i < song->order_length && i < BIRB_MAX_ORDER; i++)
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            if (song->order[i][c] != 0xFF && c + 1 > nch) nch = c + 1;
    if (nch < 1) nch = 1;

    bc_st_reset_reg();
    bc_walk(song, nch);
    if (bc_expand_locked() < 0) {
        fprintf(stderr, "Error: out of memory expanding the event list\n");
        fclose(f);
        if (bc_ev) { free(bc_ev); bc_ev = NULL; bc_nev = 0; }
        return -1;
    }

    /* one parameter row per instrument the song actually triggers */
    int row[BIRB_MAX_INSTRUMENTS], slot[BIRB_MAX_INSTRUMENTS], NI = 0;
    for (int i = 0; i < BIRB_MAX_INSTRUMENTS; i++) slot[i] = -1;
    for (int k = 0; k < bc_nev; k++) {
        int ii = bc_ev[k].ins;
        if (bc_ev[k].n < 2 || bc_ev[k].n > 97) continue;
        if (ii < 0 || ii >= BIRB_MAX_INSTRUMENTS) continue;
        if (slot[ii] < 0) { slot[ii] = NI; row[NI++] = ii; }
    }
    if (!NI) { slot[0] = 0; row[NI++] = 0; }

    /* ---- what this song reaches ---- */
    int w_used[5] = {0,0,0,0,0};          /* pulse tri saw noise sine */
    int u_fm = 0, u_fm4 = 0, u_fm2 = 0, fm_mask = 0;
    int u_ks = 0, u_fmt = 0, u_smp = 0, u_drum = 0, drum_mask = 0, drum_lfo = 0;
    int u_pe = 0, u_rev = 0, u_drive = 0, u_duck = 0, unsupported = 0;
    for (int k = 0; k < NI; k++) {
        birb_instrument *in = &song->instruments[row[k]];
        if (in->reverb_send) u_rev = 1;
        if (in->drive) u_drive = 1;
        if (in->duck_send || in->duck_amt) u_duck = 1;
        if (in->pitch_env && in->pitch_env_len) u_pe = 1;
        switch (in->synth_type) {
            case SYNTH_BASIC:   w_used[in->waveform < 5 ? in->waveform : 0] = 1; break;
            case SYNTH_KS:      u_ks = 1; break;
            case SYNTH_FORMANT: u_fmt = 1; break;
            case SYNTH_SAMPLE:  u_smp = 1; break;
            case SYNTH_DRUM: {
                int dt = in->drum_type & 7, al = dt == 4 ? 0 : dt == 5 ? 2 : dt;
                u_drum = 1; drum_mask |= 1 << (al & 3);
                if (dt == 5) drum_lfo = 1;
                break;
            }
            case SYNTH_FM: {
                int no = in->fm.num_ops ? in->fm.num_ops : 2;
                u_fm = 1;
                if (no >= 4) { u_fm4 = 1; fm_mask |= 1 << (in->fm.algorithm & 7); }
                else u_fm2 = 1;
                break;
            }
            default: unsupported = 1; break;
        }
    }
#ifdef BIRB_NO_REVERB
    u_rev = 0;
#endif
    if (!song->rev_wet) u_rev = 0;
    if (unsupported)
        fprintf(stderr, "  note: an unrecognised voice type is silent in the C backend\n");

    /* effects that survived the walk and the expansion */
    int fxu[FX_COUNT];
    for (int i = 0; i < FX_COUNT; i++) fxu[i] = 0;
    for (int k = 0; k < bc_nev; k++)
        if (bc_ev[k].fx > 0 && bc_ev[k].fx < FX_COUNT) fxu[bc_ev[k].fx] = 1;
    int u_arp = fxu[FX_ARPEGGIO];
    for (int k = 0; k < NI; k++)
        if (song->instruments[row[k]].arp_note1 || song->instruments[row[k]].arp_note2)
            u_arp = 1;
    int u_slide = fxu[FX_PITCH_UP] || fxu[FX_PITCH_DOWN];
    int u_vib   = fxu[FX_VIBRATO];
    int u_trem  = fxu[FX_TREMOLO];
    int u_porta = fxu[FX_TONE_PORTA];
    int u_soff  = fxu[FX_SAMPLE_OFFSET] && u_smp;
    int u_speed = fxu[FX_SET_SPEED];
    int u_lfo   = u_vib || u_trem;
    /* the expansion turns these into note codes, so the player needs neither */
    int u_retrig = 0, u_cut = 0;
    for (int k = 0; k < bc_nev; k++) {
        if (bc_ev[k].n == BC_N_RETRIG) u_retrig = 1;
        if (bc_ev[k].n == BC_N_CUT)    u_cut = 1;
    }
    const int ev_fx = (u_arp || u_slide || u_vib || u_trem || u_porta
                       || u_soff || u_speed);
    const int master = !birb_no_master;

    /* ---- header and constants ---- */
    double mg = (song->master_gain ? song->master_gain : 128) / 64.0;
    long spt0 = 44100L * 5 / ((song->bpm ? song->bpm : 125) * 2);
    fprintf(f,
        "/* Generated by birbc --locked-c - do not edit, regenerate.\n"
        " * Locked-down player: song baked in, no loader, no pattern data, no\n"
        " * sequencer. Full feature set. State is global so render() streams. */\n"
        "typedef signed char i8; typedef unsigned char u8;\n"
        "typedef short i16; typedef unsigned short u16;\n"
        "typedef int i32; typedef unsigned int u32;\n"
        "#define F 65536\n"
        "#define N %d\n"
        "#define NI %d\n"
        "#define TOTAL %ldL\n"
        "#define MG %.6f\n",
        nch, NI, (long)bc_walk_T, mg);
    fprintf(f,
        "static const double TG[10]={29819./65536,29819./65536,29819./65536,"
        "29819./65536,29819./65536,23127./65536,15360./65536,38838./65536,"
        "61580./65536,149078./65536};\n");
    if (master) {
        double mt = (song->limit_thresh ? song->limit_thresh : 242) / 255.0;
        /* the ceiling is the saturator's own value at the threshold: limiting
         * to MT and then saturating would otherwise land the loudest peak
         * short of full scale. */
        double x = mt > 3.0 ? 3.0 : mt, x2 = x * x;
        double mc = x * (27.0 + x2) / (27.0 + 9.0 * x2);
        fprintf(f,
            "#define MT %.6f\n#define MR %.6f\n#define MC %.17g\n",
            mt,
            1.0 - 1.0 / (44100.0 * ((song->limit_release ? song->limit_release : 50) * 0.001)),
            mc);
    }
    fprintf(f, "static i32 spt=%ld;\n", spt0);

    /* ---- per-instrument tables ---- */
    { int v[BIRB_MAX_INSTRUMENTS];
      for (int k = 0; k < NI; k++) {
        birb_instrument *in = &song->instruments[row[k]];
        switch (in->synth_type) {
            case SYNTH_SAMPLE:  v[k] = 5; break;
            case SYNTH_FM:      v[k] = 6; break;
            case SYNTH_KS:      v[k] = 7; break;
            case SYNTH_DRUM:    v[k] = 8; break;
            case SYNTH_FORMANT: v[k] = 9; break;
            default:            v[k] = in->waveform < 5 ? in->waveform : 0; break;
        }
      }
      lk_tab(f, "u8", "CW", NI, v);
      if (w_used[0]) {
        for (int k = 0; k < NI; k++) v[k] = song->instruments[row[k]].duty;
        lk_tab(f, "i32", "CU", NI, v);
      }
      { const char *an[4] = { "CA", "CD", "CS", "CR" };
        for (int a = 0; a < 4; a++) {
          for (int k = 0; k < NI; k++) {
            birb_adsr *e = &song->instruments[row[k]].envelope;
            v[k] = a == 0 ? e->attack : a == 1 ? e->decay : a == 2 ? e->sustain : e->release;
          }
          lk_tab(f, "u8", an[a], NI, v);
        } }
      /* always emitted: the JS mix multiplies by it unconditionally, and
       * folding a constant 255 away would change the double rounding */
      for (int k = 0; k < NI; k++) {
        int iv = song->instruments[row[k]].volume;
        v[k] = iv ? iv : 255;
      }
      lk_tab(f, "i32", "CV", NI, v);
      if (u_pe) {
        for (int k = 0; k < NI; k++) v[k] = song->instruments[row[k]].pitch_env;
        lk_tab(f, "i8", "CPE", NI, v);
        for (int k = 0; k < NI; k++) v[k] = song->instruments[row[k]].pitch_env_len;
        lk_tab(f, "u8", "CPL", NI, v);
      }
      if (u_arp) {
        for (int k = 0; k < NI; k++) v[k] = song->instruments[row[k]].arp_note1;
        lk_tab(f, "i32", "IA1", NI, v);
        for (int k = 0; k < NI; k++) v[k] = song->instruments[row[k]].arp_note2;
        lk_tab(f, "i32", "IA2", NI, v);
      }
      if (u_rev) {
        for (int k = 0; k < NI; k++) v[k] = song->instruments[row[k]].reverb_send;
        lk_tab(f, "i32", "RS", NI, v);
      }
      if (u_duck) {
        for (int k = 0; k < NI; k++) v[k] = song->instruments[row[k]].duck_send;
        lk_tab(f, "i32", "DS", NI, v);
        for (int k = 0; k < NI; k++) v[k] = song->instruments[row[k]].duck_amt;
        lk_tab(f, "i32", "DA", NI, v);
      }
      if (u_smp) {
        for (int k = 0; k < NI; k++) {
            birb_instrument *in = &song->instruments[row[k]];
            v[k] = in->synth_type == SYNTH_SAMPLE ? in->sample_idx : 0;
        }
        lk_tab(f, "i32", "CSI", NI, v);
      }
      if (u_ks) {
        for (int k = 0; k < NI; k++) {
            birb_instrument *in = &song->instruments[row[k]];
            v[k] = in->synth_type == SYNTH_KS ? in->ks_damping : 0;
        }
        lk_tab(f, "i32", "KD", NI, v);
      }
    }
    /* drive is a double per instrument: pre-gain and the matching normaliser */
    if (u_drive) {
        fprintf(f, "static const double DVP[NI]={");
        for (int k = 0; k < NI; k++) {
            double dp = 1.0 + song->instruments[row[k]].drive * (8.0 / 255.0);
            fprintf(f, "%s%.17g", k ? "," : "", dp);
        }
        fprintf(f, "};\nstatic const double DVN[NI]={");
        for (int k = 0; k < NI; k++) {
            double dp = 1.0 + song->instruments[row[k]].drive * (8.0 / 255.0);
            double x = dp > 3.0 ? 3.0 : dp, x2 = x * x;
            double ss = x * (27.0 + x2) / (27.0 + 9.0 * x2);
            fprintf(f, "%s%.17g", k ? "," : "", dp > 1.0 ? 1.0 / ss : 1.0);
        }
        fprintf(f, "};\n");
    }

    /* ---- FM ---- */
    int NO = u_fm4 ? 4 : 2;
    if (u_fm) {
        const char *fn[6] = { "FR", "FL", "FA", "FD", "FS", "FRl" };
        for (int a = 0; a < 6; a++) {
            fprintf(f, "static const i32 %s[NI][%d]={", fn[a], NO);
            for (int k = 0; k < NI; k++) {
                birb_instrument *in = &song->instruments[row[k]];
                fprintf(f, "%s{", k ? "," : "");
                for (int o = 0; o < NO; o++) {
                    birb_fm_op *op = &in->fm.ops[o];
                    int val = 0;
                    if (in->synth_type == SYNTH_FM) switch (a) {
                        case 0: val = (op->ratio_i << 4) | (op->ratio_f & 0xF); break;
                        case 1: val = (int)((65536.0 * op->level) / 255.0 + 0.5); break;
                        case 2: val = op->adsr.attack; break;
                        case 3: val = op->adsr.decay; break;
                        case 4: val = op->adsr.sustain; break;
                        default: val = op->adsr.release; break;
                    }
                    fprintf(f, "%s%d", o ? "," : "", val);
                }
                fprintf(f, "}");
            }
            fprintf(f, "};\n");
        }
        int v[BIRB_MAX_INSTRUMENTS];
        for (int k = 0; k < NI; k++) {
            birb_instrument *in = &song->instruments[row[k]];
            v[k] = in->synth_type == SYNTH_FM ? in->fm.feedback : 0;
        }
        lk_tab(f, "i32", "FFB", NI, v);
        for (int k = 0; k < NI; k++) {
            birb_instrument *in = &song->instruments[row[k]];
            v[k] = in->synth_type == SYNTH_FM ? in->fm.mod_index : 64;
        }
        lk_tab(f, "i32", "FMI", NI, v);
        if (u_fm4 && u_fm2) {
            for (int k = 0; k < NI; k++) {
                birb_instrument *in = &song->instruments[row[k]];
                v[k] = (in->synth_type == SYNTH_FM && in->fm.num_ops) ? in->fm.num_ops : 2;
            }
            lk_tab(f, "u8", "FNO", NI, v);
        }
        int n_alg = 0;
        for (int a = 0; a < 8; a++) if (fm_mask & (1 << a)) n_alg++;
        if (u_fm4 && n_alg > 1) {
            for (int k = 0; k < NI; k++) {
                birb_instrument *in = &song->instruments[row[k]];
                v[k] = in->synth_type == SYNTH_FM ? (in->fm.algorithm & 7) : 0;
            }
            lk_tab(f, "u8", "FAL", NI, v);
        }
    }

    /* ---- drum ---- */
    if (u_drum) {
        const char *dn[5] = { "DT", "DTU", "DDC", "DTO", "DSN" };
        int v[BIRB_MAX_INSTRUMENTS];
        for (int a = 0; a < 5; a++) {
            for (int k = 0; k < NI; k++) {
                birb_instrument *in = &song->instruments[row[k]];
                int val = 0;
                if (in->synth_type == SYNTH_DRUM) switch (a) {
                    case 0: val = in->drum_type & 7; break;
                    case 1: val = (signed char)in->drum_tune; break;
                    case 2: val = in->drum_decay; break;
                    case 3: val = in->drum_tone; break;
                    default: val = in->drum_snap; break;
                }
                v[k] = val;
            }
            lk_tab(f, "i32", dn[a], NI, v);
        }
    }

    /* ---- formant ---- */
    if (u_fmt) {
        fprintf(f, "static const i32 FCO[NI][2][9]={");
        for (int k = 0; k < NI; k++) {
            birb_instrument *in = &song->instruments[row[k]];
            int32_t cf[2][3][3];
            double q = 2.0 + (in->formant_resonance / 255.0) * 30.0;
            if (in->synth_type == SYNTH_FORMANT) {
                bc_formant_coeffs(in->formant_vowel_a, q, cf[0]);
                bc_formant_coeffs(in->formant_vowel_b, q, cf[1]);
            } else {
                for (int a = 0; a < 2; a++) for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) cf[a][i][j] = 0;
            }
            fprintf(f, "%s{", k ? "," : "");
            for (int a = 0; a < 2; a++) {
                fprintf(f, "%s{", a ? "," : "");
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++)
                        fprintf(f, "%s%d", (i || j) ? "," : "", (int)cf[a][i][j]);
                fprintf(f, "}");
            }
            fprintf(f, "}");
        }
        fprintf(f, "};\n");
        const char *fn2[4] = { "FTSW", "FTDU", "FTSS", "FTRS" };
        int v[BIRB_MAX_INSTRUMENTS];
        for (int a = 0; a < 4; a++) {
            for (int k = 0; k < NI; k++) {
                birb_instrument *in = &song->instruments[row[k]];
                int val = 0;
                if (in->synth_type == SYNTH_FORMANT) switch (a) {
                    case 0: val = in->formant_source_wave; break;
                    case 1: val = in->formant_duty & 3; break;
                    case 2: val = in->formant_sweep_speed; break;
                    default: val = in->formant_resonance; break;
                }
                v[k] = val;
            }
            lk_tab(f, "i32", fn2[a], NI, v);
        }
    }

    /* ---- samples ---- */
    if (u_smp) {
        fprintf(f, "static const i16 SPOOL[]={");
        for (uint32_t k = 0; k < song->sample_pool_used; k++)
            fprintf(f, "%s%d", k ? "," : "", (int)song->sample_pool[k]);
        if (!song->sample_pool_used) fprintf(f, "0");
        fprintf(f, "};\n");
        const char *sn[5] = { "SOFF", "SLEN", "SLS", "SLE", "SBN" };
        for (int a = 0; a < 5; a++) {
            fprintf(f, "static const i32 %s[]={", sn[a]);
            for (int i = 0; i < song->num_samples; i++) {
                birb_sample_meta *m = &song->samples[i];
                long val = a == 0 ? (long)m->offset : a == 1 ? (long)m->length
                         : a == 2 ? (long)(int32_t)m->loop_start
                         : a == 3 ? (long)m->loop_end : (long)m->base_note;
                fprintf(f, "%s%ld", i ? "," : "", val);
            }
            if (!song->num_samples) fprintf(f, "0");
            fprintf(f, "};\n");
        }
    }

    /* ---- event list ---- */
    const int ev_w = 5 + (ev_fx ? 2 : 0);
    fprintf(f, "static const u16 EV[]={");
    { int prev = 0;
      for (int k = 0; k < bc_nev; k++) {
        int ii = bc_ev[k].ins;
        int sl = (ii >= 0 && ii < BIRB_MAX_INSTRUMENTS && slot[ii] >= 0) ? slot[ii] : 0;
        fprintf(f, "%s%d,%d,%d,%d,%d", k ? "," : "",
                bc_ev[k].t - prev, bc_ev[k].c, bc_ev[k].n, bc_ev[k].rv, sl);
        if (ev_fx) fprintf(f, ",%d,%d", bc_ev[k].fx, bc_ev[k].pm);
        prev = bc_ev[k].t;
      } }
    fprintf(f, "};\n#define NEV %d\n", bc_nev * ev_w);

    /* ---- shared helpers ---- */
    fprintf(f,
        "static const i32 BF[12]={6221,6591,6983,7398,7838,8304,8797,9321,9875,"
        "10462,11084,11743};\n"
        "static i32 nf(i32 n){ if(n<0)n=0; if(n>95)n=95;\n"
        " return ((BF[n%%12]<<(n/12))+128)>>8; }\n");
    if (u_fm || drum_lfo || w_used[4])
        fprintf(f,
            "static double sa_(i32 ph){ i32 p=((ph%%F)+F)%%F; i32 ng=p>=F/2; i32 t=ng?p-F/2:p;\n"
            " if(t>F/4) t=F/2-t; double x=(double)t/F*4.; if(x>1.)x=1.; double x2=x*x;\n"
            " double y=x*(1.5707288-x2*(0.6432292-x2*0.0727778)); if(y>1.)y=1.;\n"
            " return ng?-y:y; }\n");
    if (u_lfo)
        fprintf(f,
            "static i32 LT(i32 p){ p&=65535; i32 t=p<32768?p:65536-p;\n"
            " return ((t<<2)-65536)>>7; }\n");
    if (master || u_drive)
        fprintf(f,
            "static double SS(double x){ if(x<-3.)x=-3.; else if(x>3.)x=3.;\n"
            " double x2=x*x; return x*(27.+x2)/(27.+9.*x2); }\n");

    /* ---- state ---- */
    bc_st(f, "double ph[N],fq[N],ev_[N];", "ph,fq,ev_");
    bc_st(f, "i32 bs[N],st[N],rv[N];", "bs,st,rv");
    bc_st(f, "u8 CI[N];", "CI");
    if (u_pe)    bc_st(f, "i32 pet[N];", "pet");
    if (u_slide) bc_st(f, "i32 sl[N];", "sl");
    if (u_porta) bc_st(f, "i32 pt[N],ps[N];", "pt,ps");
    if (u_arp)   bc_st(f, "i32 a1[N],a2[N],at[N],bn[N];", "a1,a2,at,bn");
    if (u_vib)   bc_st(f, "i32 vp[N],vs[N],vd[N];", "vp,vs,vd");
    if (u_trem)  bc_st(f, "i32 tp[N],ts[N],td[N],tm[N];", "tp,ts,td,tm");
    if (w_used[3]) bc_st(f, "i32 lh[N],lj[N],lm[N];", "lh,lj,lm");
    if (u_fm) {
        char d1[128], d2[128];
        snprintf(d1, sizeof d1, "i32 fp[N][%d],ff[N][%d],fst[N][%d];", NO, NO, NO);
        snprintf(d2, sizeof d2, "double fe[N][%d],fpv[N];", NO);
        bc_st(f, d1, "fp,ff,fst"); bc_st(f, d2, "fe,fpv");
    }
    if (u_ks) {
        bc_st(f, "i16 kb[N][1024];", "kb");
        bc_st(f, "i32 kl[N],kp[N],kg[N];", "kl,kp,kg");
        fprintf(f,
            "static const i32 KQ[33]={657,776,916,1082,1277,1508,1781,2103,2484,2933,3463,4089,4829,5702,6733,7951,9388,11086,13091,15458,18253,21554,25452,30054,35489,41907,49485,58434,69001,81479,96213,113612,131398};\n"
            "static i32 KG(i32 d,i32 l){ i32 i=d>>3,fr=d&7,q=KQ[i];\n"
            " q+=((KQ[i+1]-q)*fr)>>3; i32 a=(i32)(((long long)q*l)>>8);\n"
            " return a>=65535?0:65535-a; }\n");
    }
    if (u_smp) bc_st(f, "i32 sp_[N],ss_[N],si_[N],sa_on[N];", "sp_,ss_,si_,sa_on");
    if (u_fmt) {
        bc_st(f, "i32 ftz1[N][3],ftz2[N][3],ftb0[N][3],fta1[N][3],fta2[N][3];",
                 "ftz1,ftz2,ftb0,fta1,fta2");
        bc_st(f, "i32 ftlf[N],ftsp[N],ftdr[N],ftrc[N],CUv[N];",
                 "ftlf,ftsp,ftdr,ftrc,CUv");
        fprintf(f,
            "static void FI(i32 c){ i32 I=CI[c], t=ftsp[c], o=255-t;\n"
            " for(i32 i=0;i<3;i++){\n"
            "  ftb0[c][i]=(FCO[I][0][i*3+0]*o+FCO[I][1][i*3+0]*t)/255;\n"
            "  fta1[c][i]=(FCO[I][0][i*3+1]*o+FCO[I][1][i*3+1]*t)/255;\n"
            "  fta2[c][i]=(FCO[I][0][i*3+2]*o+FCO[I][1][i*3+2]*t)/255; } }\n");
    }
    if (u_drum) {
        bc_st(f, "i32 dal[N],dor[N],dp2[N],dnz[N],dpe[N],dpt[N],drt[N],dsn[N],"
                 "dclk[N],dz1[N],dlf[N],dttl[N],dmix[N],dstg[N],dstt[N],dbl[N];",
                 "dal,dor,dp2,dnz,dpe,dpt,drt,dsn,dclk,dz1,dlf,dttl,dmix,dstg,dstt,dbl");
        fprintf(f,
            "static i32 dlfn(i32 c){ i32 b=(dlf[c]^(dlf[c]>>1))&1;\n"
            "  dlf[c]=((dlf[c]>>1)|(b<<14))&0xFFFF; if(!dlf[c]) dlf[c]=0x7FFF; return dlf[c]; }\n");
        if (drum_mask & 1)
            fprintf(f,
                "static const i32 KSW[33]={65518,65516,65512,65509,65505,65500,65495,65489,65482,65474,65465,65454,65442,65428,65412,65393,65372,65348,65320,65287,65251,65208,65160,65104,65040,64966,64882,64785,64674,64546,64400,64233,64067};\n"
                "static i32 KC(i32 t){ i32 i=t>>3,fr=t&7,c=KSW[i]; return c+(((KSW[i+1]-c)*fr)>>3); }\n");
        if (drum_mask & 2)
            fprintf(f,
                "static const i32 SHP[33]={1386,1545,1722,1920,2139,2383,2655,2956,3291,3663,4076,4533,5039,5600,6219,6903,7657,8487,9400,10401,11498,12697,14004,15424,16964,18627,20416,22332,24376,26543,28828,31221,33392};\n"
                "static i32 SC(i32 t){ i32 i=t>>3,fr=t&7,c=SHP[i]; return c+(((SHP[i+1]-c)*fr)>>3); }\n");
    }
    if (u_rev) {
        bc_st(f, "double rc0[1116],rc1[1188],rc2[1277],rc3[1356],rcl[4];",
                 "rc0,rc1,rc2,rc3,rcl");
        bc_st(f, "double ra0[556],ra1[441];", "ra0,ra1");
        bc_st(f, "i32 rcp[4],rap[2];", "rcp,rap");
        double size = song->rev_size / 255.0, damp = song->rev_damp / 255.0;
        double wet = song->rev_wet / 255.0;
        double fb = 0.7 + 0.28 * size, dc = 0.4 * damp;
        fprintf(f,
            "static double *const rcb[4]={rc0,rc1,rc2,rc3};\n"
            "static const i32 rcn[4]={1116,1188,1277,1356};\n"
            "static double *const rab[2]={ra0,ra1};\n"
            "static const i32 ran[2]={556,441};\n"
            "static double rev_(double x){ double o=0.;\n"
            " for(i32 k=0;k<4;k++){ i32 L=rcn[k],pp=rcp[k]; double y=rcb[k][pp];\n"
            "  rcl[k]=y*%.6f+rcl[k]*%.6f; rcb[k][pp]=x+rcl[k]*%.6f;\n"
            "  rcp[k]=(pp+1<L)?pp+1:0; o+=y; }\n"
            " o*=%.6f;\n"
            " for(i32 k=0;k<2;k++){ i32 L=ran[k],pp=rap[k]; double y=rab[k][pp];\n"
            "  double ou=-o+y; rab[k][pp]=o+y*0.5;\n"
            "  rap[k]=(pp+1<L)?pp+1:0; o=ou; }\n"
            " return o*%.6f; }\n",
            1.0 - dc, dc, fb, (1.0 - fb) * 5.5, wet);
    }
    if (master) bc_st(f, "double LE;", "LE");
    if (u_duck) {
        bc_st(f, "double DE;", "DE");
        fprintf(f, "#define DR %.6f\n",
                1.0 - 1.0 / (44100.0 * ((song->duck_release ? song->duck_release : 120) * 0.001)));
    }
    bc_st(f, "i32 ei,tk,et,tc;", "ei,tk,et,tc");
    fprintf(f,
        "static i16 out[4096];\n"
        "i16 *outPtr(void){ return out; }\n"
        "u32 getOutputBuf(void){ return (u32)(unsigned long)out; }\n"
        "i32 getLength(void){ return (i32)TOTAL; }\n");

    /* ---- trigger ---- */
    fprintf(f,
        "static void trig(i32 c,i32 n,i32 I){ CI[c]=(u8)I; i32 s=n-2;\n"
        " bs[c]=nf(s); ph[c]=0.; rv[c]=255;\n");
    if (u_arp)   fprintf(f, " bn[c]=s; a1[c]=IA1[I]; a2[c]=IA2[I]; at[c]=0;\n");
    if (u_slide) fprintf(f, " sl[c]=0;\n");
    if (u_porta) fprintf(f, " ps[c]=0;\n");
    fprintf(f,
        " if(CA[I]==0){ ev_[c]=(double)F; st[c]=2; } else { ev_[c]=0.; st[c]=1; }\n");
    if (u_pe)      fprintf(f, " pet[c]=CPL[I];\n");
    if (w_used[3]) fprintf(f, " if(CW[I]==3){ lh[c]=0x7FFF; lm[c]=0; lj[c]=(256>>(s/12))?(256>>(s/12)):1; }\n");
    if (u_smp)
        fprintf(f,
            " sa_on[c]=0;\n"
            " if(CW[I]==5){ i32 idx=CSI[I]; si_[c]=idx; sp_[c]=0; sa_on[c]=1;\n"
            "  i32 bf2=nf(SBN[idx]);\n"
            "  ss_[c] = bf2>0 ? (i32)(((long long)bs[c]*65536 + bf2/2)/bf2) : 65536; }\n");
    if (u_fmt)
        fprintf(f,
            " if(CW[I]==9){ ftlf[c]=(0x7FFF^((s*0x2BCD)&0xFFFF))&0xFFFF; if(!ftlf[c]) ftlf[c]=0x7FFF;\n"
            "  ftsp[c]=0; ftdr[c]=1; ftrc[c]=0;\n"
            "  for(i32 i=0;i<3;i++){ ftz1[c][i]=0; ftz2[c][i]=0; }\n"
            "  { static const i32 dvt[4]={F/8,F/4,F/2,F*3/4}; CUv[c]=dvt[FTDU[I]&3]; }\n"
            "  FI(c); }\n");
    if (u_ks)
        fprintf(f,
            " if(CW[I]==7){ i32 ln = bs[c]>0 ? (F/bs[c]) : 0;\n"
            "  if(ln<4) ln=4; if(ln>1024) ln=1024; kl[c]=ln; kp[c]=0; kg[c]=KG(KD[I],ln);\n"
            "  i32 lfv=(0x7FFF^((s*0x1D79)&0xFFFF))&0xFFFF; if(!lfv) lfv=0x7FFF;\n"
            "  for(i32 ki=0;ki<ln;ki++){ i32 kbv=(lfv^(lfv>>1))&1;\n"
            "   lfv=((lfv>>1)|(kbv<<14))&0xFFFF; kb[c][ki]=(lfv&1)?32767:-32767; } }\n");
    if (u_drum) {
        fprintf(f,
            " if(CW[I]==8){ i32 dt=DT[I], al=dt==4?0:dt==5?2:dt, tn=DTU[I];\n"
            "  i32 dec=DDC[I], tone=DTO[I], snp=DSN[I], dn=s+tn;\n"
            "  if(dn<0)dn=0; if(dn>95)dn=95; i32 df=nf(dn), tt=0;\n"
            "  dal[c]=al; dor[c]=dt; dp2[c]=0; dz1[c]=0;\n"
            "  dlf[c]=(0x7FFF^((s*0x3D7F)&0xFFFF))&0xFFFF; if(!dlf[c]) dlf[c]=0x7FFF;\n");
        if (drum_mask & 1)
            fprintf(f,
                "  if(al==0){ dpe[c]=(df<<3)<<8; dpt[c]=(df>>1)<<8; drt[c]=KC(tone);\n"
                "   dsn[c]=snp; dclk[c]=384; tt=dec*200+1024; if(dt==4) tt*=2; }\n");
        if (drum_mask & 2)
            fprintf(f,
                "  %sif(al==1){ bs[c]=df>0?df:nf(26); dmix[c]=snp; dpt[c]=SC(tone);\n"
                "   dpe[c]=F; drt[c]=65460; dz1[c]=0; tt=dec*120+1024; dp2[c]=F;\n"
                "   i32 q=301466/(tt?tt:1); dnz[c]=F-(q<4096?q:4096); }\n",
                (drum_mask & 1) ? "else " : "");
        if (drum_mask & 4)
            fprintf(f,
                "  %sif(al==2){ i32 hp=(i32)(snp*(F*15.0/16.0/255.0)); if(hp<(F>>4)) hp=F>>4;\n"
                "   dpt[c]=hp; dz1[c]=0; tt=dec*180+1024; if(dt==5) tt=90000+dec*400; }\n",
                (drum_mask & 3) ? "else " : "");
        if (drum_mask & 8)
            fprintf(f,
                "  %s{ dbl[c]=80+(snp>>1); dstg[c]=0; dstt[c]=dbl[c]; tt=dec*160+2048; }\n",
                (drum_mask & 7) ? "else " : "");
        fprintf(f, "  if(tt>0xFFFFFF) tt=0xFFFFFF; dttl[c]=tt; }\n");
    }
    if (u_fm)
        fprintf(f,
            " if(CW[I]==6){ fpv[c]=0.;\n"
            "  for(i32 k=0;k<%d;k++){ fp[c][k]=0; ff[c][k]=(i32)((double)bs[c]*FR[I][k]/16.+0.5);\n"
            "   if(FA[I][k]==0){ fe[c][k]=(double)F; fst[c][k]=2; } else { fe[c][k]=0.; fst[c][k]=1; } } }\n",
            NO);
    fprintf(f, "}\n");

    /* ---- per-tick ---- */
    { char fxdecl[64];
      if (ev_fx) snprintf(fxdecl, sizeof fxdecl, " i32 fx=EV[ei+5],pm=EV[ei+6];");
      else fxdecl[0] = 0;
      fprintf(f,
        "static void tickf(void){\n"
        " while(ei<NEV && et+(i32)EV[ei]<=tk){ et+=(i32)EV[ei];\n"
        "  i32 c=EV[ei+1],n=EV[ei+2],v=EV[ei+3],I=EV[ei+4];%s ei+=%d;\n",
        fxdecl, ev_w);
      /* a tone porta with a note retunes the target instead of retriggering */
      if (u_porta)
          fprintf(f, "  if(fx==5&&n>=2&&n<=97){ pt[c]=nf(n-2); }\n  else ");
      else
          fprintf(f, "  ");
      fprintf(f, "if(n==1) st[c]=4;\n");
      if (u_cut)
          fprintf(f, "  else if(n==%d){ ev_[c]=0.; st[c]=0; }\n", BC_N_CUT);
      if (u_retrig) {
          fprintf(f, "  else if(n==%d){ ph[c]=0.; ev_[c]=0.; st[c]=1;", BC_N_RETRIG);
          if (w_used[3]) fprintf(f, " if(CW[CI[c]]>=3){ lh[c]=0x7FFF; lm[c]=0; }");
          fprintf(f, " }\n");
      }
      fprintf(f, "  else if(n>=2&&n<=97) trig(c,n,I);\n  if(v) rv[c]=v;\n");
      if (u_arp)   fprintf(f, "  if(fx==1){ a1[c]=pm>>4; a2[c]=pm&15; at[c]=0; }\n");
      if (u_slide) fprintf(f, "  if(fx==2) sl[c]=pm<<2; else if(fx==3) sl[c]=-(pm<<2);\n");
      if (u_vib)   fprintf(f, "  if(fx==4){ vs[c]=F/64*(pm>>4); vd[c]=(pm&15)<<4; }\n");
      if (u_porta) fprintf(f, "  if(fx==5) ps[c]=pm<<2;\n");
      if (u_trem)  fprintf(f, "  if(fx==8){ ts[c]=F/64*(pm>>4); td[c]=(pm&15)<<4; }\n");
      if (u_soff)  fprintf(f, "  if(fx==9&&CW[CI[c]]==5) sp_[c]=(pm<<8)<<16;\n");
      if (u_speed) fprintf(f, "  if(fx==15&&pm>=0x20) spt=44100*5/(pm*2);\n");
      fprintf(f, " }\n tk++;\n for(i32 c=0;c<N;c++){ i32 I=CI[c];\n"); }
    if (u_pe)
        fprintf(f, "  if(pet[c]){ bs[c]+=(i32)CPE[I]<<2; if(bs[c]<1)bs[c]=1; pet[c]--; }\n");
    if (u_slide)
        fprintf(f, "  if(sl[c]){ bs[c]+=sl[c]; if(bs[c]<1)bs[c]=1; }\n");
    if (u_porta)
        fprintf(f,
            "  if(pt[c]&&ps[c]){ if(bs[c]<pt[c]){ bs[c]+=ps[c]; if(bs[c]>pt[c]) bs[c]=pt[c]; }\n"
            "   else if(bs[c]>pt[c]){ bs[c]-=ps[c]; if(bs[c]<pt[c]) bs[c]=pt[c]; } }\n");
    if (u_arp)
        fprintf(f,
            "  if(a1[c]|a2[c]){ i32 t3=at[c]%%3, nn=bn[c];\n"
            "   if(t3==1) nn+=a1[c]; else if(t3==2) nn+=a2[c];\n"
            "   fq[c]=(double)nf(nn); at[c]++; } else fq[c]=(double)bs[c];\n");
    else
        fprintf(f, "  fq[c]=(double)bs[c];\n");
    if (u_vib)
        fprintf(f,
            "  if(vd[c]){ fq[c]+=(double)(LT(vp[c])*vd[c])/(double)F; vp[c]+=vs[c]; }\n");
    if (u_trem)
        fprintf(f,
            "  if(td[c]){ tm[c]=(LT(tp[c])*td[c])>>1; tp[c]+=ts[c]; } else tm[c]=0;\n");
    fprintf(f,
        "  i32 e=st[c];\n"
        "  if(e==1){ ev_[c]+=(double)F/(CA[I]+1); if(ev_[c]>=F){ ev_[c]=(double)F; st[c]=2; } }\n"
        "  else if(e==2){ double g=(double)F*CS[I]/255; ev_[c]-=((double)F-g)/(CD[I]+1); if(ev_[c]<=g){ ev_[c]=g; st[c]=3; } }\n"
        "  else if(e==4){ ev_[c]-=ev_[c]/(CR[I]+1); if(ev_[c]<64){ ev_[c]=0.; st[c]=0; } }\n");
    if (u_fm)
        fprintf(f,
            "  if(CW[I]==6) for(i32 k=0;k<%d;k++){ ff[c][k]=(i32)(fq[c]*FR[I][k]/16.+0.5);\n"
            "   i32 t2=fst[c][k]; double en=fe[c][k];\n"
            "   if(t2==1){ en+=(double)F/(FA[I][k]+1); if(en>=F){ en=(double)F; t2=2; } }\n"
            "   else if(t2==2){ i32 g=(i32)((double)F*FS[I][k]/255); en-=((double)F-g)/(FD[I][k]+1); if(en<=g){ en=g; t2=3; } }\n"
            "   else if(t2==4){ en-=en/(FRl[I][k]+1); if(en<64){ en=0.; t2=0; } }\n"
            "   fst[c][k]=t2; fe[c][k]=en; }\n", NO);
    fprintf(f, " }\n}\n");

    /* ---- render ---- */
    fprintf(f,
        "void render(i32 n){\n"
        " for(i32 i=0;i<n;i++){\n"
        "  if(tc<=0){ tickf(); tc=spt; }\n"
        "  tc--;\n"
        "  double v=0.%s%s;\n"
        "  for(i32 c=0;c<N;c++){\n"
        "   if(!st[c] && ev_[c]==0.%s) continue;\n"
        "   i32 I=CI[c]; double h=ph[c], s=0.;\n",
        u_rev ? ",ri=0." : "",
        u_duck ? ",DI=0.,DN=DE" : "",
        u_drum ? " && !dttl[c]" : "");
    { int first = 1;
      if (u_smp) {
        fprintf(f,
            "   %s(CW[I]==5){ if(sa_on[c]){ i32 idx=si_[c], pos=(i32)((u32)sp_[c]>>16);\n"
            "    if(pos>=SLEN[idx]){\n"
            "     if(SLS[idx]>=0 && SLE[idx]>SLS[idx]){ i32 ll=SLE[idx]-SLS[idx];\n"
            "      pos=SLS[idx]+((pos-SLS[idx])%%ll); sp_[c]=(pos<<16)|(sp_[c]&0xFFFF); }\n"
            "     else { sa_on[c]=0; s=0.; } }\n"
            "    if(sa_on[c]){ s=(double)SPOOL[SOFF[idx]+pos]/32768.;\n"
            "     sp_[c]=(i32)((u32)sp_[c]+(u32)ss_[c]); } } }\n", first?"if":"else if");
        first = 0;
      }
      if (u_fmt) {
        fprintf(f,
            "   %s(CW[I]==9){ i32 src;\n"
            "    if(FTSW[I]==3){ i32 fv=ftlf[c], fb2=(fv^(fv>>1))&1;\n"
            "     fv=((fv>>1)|(fb2<<14))&0xFFFF; if(!fv) fv=0x7FFF; ftlf[c]=fv;\n"
            "     src=(fv&1)?16383:-16383; }\n"
            "    else if(FTSW[I]==0){ src = h<(double)CUv[c] ? 16383 : -16383; }\n"
            "    else { src=(i32)((h*2.-(double)F)*32767./(double)F); }\n"
            "    i32 sm=0;\n"
            "    for(i32 fi=0;fi<3;fi++){\n"
            "     i32 b0=ftb0[c][fi],a1=fta1[c][fi],a2=fta2[c][fi];\n"
            "     i32 yy=(i32)(u32)(long long)((double)b0*src/65536.)+ftz1[c][fi];\n"
            "     ftz1[c][fi]=(-(i32)(u32)(long long)((double)a1*yy/65536.))+ftz2[c][fi];\n"
            "     ftz2[c][fi]=(-(i32)(u32)(long long)((double)b0*src/65536.))-(i32)(u32)(long long)((double)a2*yy/65536.);\n"
            "     sm+=yy; }\n"
            "    if(sm>32767) sm=32767; else if(sm<-32767) sm=-32767;\n"
            "    s=(double)sm/32768.;\n"
            "    if(FTSS[I]){ ftrc[c]=(ftrc[c]+1)&0xFF;\n"
            "     if((ftrc[c]&0x1F)==0){ i32 stp=FTSS[I]>>3; if(!stp) stp=1;\n"
            "      i32 sp=ftsp[c]+ftdr[c]*stp;\n"
            "      if(sp>=255){ sp=255; ftdr[c]=-1; } else if(sp<=0){ sp=0; ftdr[c]=1; }\n"
            "      ftsp[c]=sp; FI(c); } } }\n", first?"if":"else if");
        first = 0;
      }
      if (u_ks) {
        fprintf(f,
            "   %s(CW[I]==7){ if(kl[c]<2) s=0.; else {\n"
            "    i32 kpp=kp[c], knx=kpp+1; if(knx>=kl[c]) knx=0;\n"
            "    i32 kcur=kb[c][kpp];\n"
            "    kb[c][kpp]=(i16)((i32)(((kcur+kb[c][knx])>>1)*kg[c])>>16);\n"
            "    kp[c]=knx; s=(double)kcur/32768.; } }\n", first?"if":"else if");
        first = 0;
      }
      if (u_drum) {
        fprintf(f,
            "   %s(CW[I]==8){ if(dttl[c]<=0){ s=0.; ev_[c]=0.; st[c]=0; } else { dttl[c]--;\n"
            "    i32 o_=0;\n", first?"if":"else if");
        if (drum_mask & 1)
            fprintf(f,
                "    if(dal[c]==0){ i32 gp=dpe[c]-dpt[c];\n"
                "     if(gp>0) dpe[c]=dpt[c]+(i32)(((long long)gp*drt[c])>>16);\n"
                "     ph[c]+=(double)(dpe[c]>>8); while(ph[c]>=(double)F) ph[c]-=(double)F;\n"
                "     double tri = ph[c]<(double)F/2 ? (ph[c]*4.-(double)F) : ((double)F*3.-ph[c]*4.);\n"
                "     o_=(i32)(tri*28000./(double)F);\n"
                "     if(dclk[c]>0){ i32 nn=dlfn(c), pk=dsn[c]*128, ap=(i32)(((long long)pk*dclk[c])/384);\n"
                "      o_ += (nn&1)?ap:-ap; dclk[c]--; } }\n");
        if (drum_mask & 2)
            fprintf(f,
                "    %sif(dal[c]==1){ i32 nn=dlfn(c), sr_=(nn&1)?26000:-26000, y_=sr_-dz1[c];\n"
                "     dz1[c] += (i32)((i32)(u32)((long long)y_*dpt[c])>>16);\n"
                "     i32 noi=(i32)(((long long)y_*dp2[c])>>16);\n"
                "     dp2[c]=(i32)(((long long)dp2[c]*dnz[c])>>16);\n"
                "     ph[c]+=fq[c]; while(ph[c]>=(double)F) ph[c]-=(double)F;\n"
                "     double tr_ = ph[c]<(double)F/2 ? (ph[c]*4.-(double)F) : ((double)F*3.-ph[c]*4.);\n"
                "     i32 bd=(i32)((i32)(u32)(long long)(tr_*22000.)>>16);\n"
                "     bd=(i32)(((long long)bd*dpe[c])>>16);\n"
                "     dpe[c]=(i32)(((long long)dpe[c]*drt[c])>>16);\n"
                "     i32 mxB=dmix[c], mxN=255-mxB;\n"
                "     o_=(i32)(((long long)noi*mxN+(long long)bd*mxB)/255); }\n",
                (drum_mask & 1) ? "else " : "");
        if (drum_mask & 4)
            fprintf(f,
                "    %sif(dal[c]==2){ i32 nn=dlfn(c), src2=(nn&1)?24000:-24000, yh=src2-dz1[c];\n"
                "     dz1[c] += (i32)((i32)(u32)((long long)yh*dpt[c])>>16); o_=yh;\n"
                "%s"
                "    }\n",
                (drum_mask & 3) ? "else " : "",
                drum_lfo
                  ? "     if(dor[c]==5){ double lo=sa_((dttl[c]<<3)&0xFFFF); o_=(i32)(o_*(1.+lo*0.5)); }\n"
                  : "");
        if (drum_mask & 8)
            fprintf(f,
                "    %s{ i32 amp=0;\n"
                "     if(dstg[c]<3){ i32 bL=dbl[c]?dbl[c]:96, into=bL-dstt[c]; double hf=bL/2.;\n"
                "      double en2 = (into<hf) ? (into*256/hf) : ((bL-into)*256/hf);\n"
                "      if(en2<0) en2=0; if(en2>256) en2=256;\n"
                "      i32 nn=dlfn(c), sc=(nn&1)?26000:-26000;\n"
                "      amp=(i32)((i32)(u32)(long long)(sc*en2)>>8); }\n"
                "     else { i32 nn=dlfn(c); amp=(nn&1)?9000:-9000; }\n"
                "     if(dstt[c]>0) dstt[c]--;\n"
                "     else if(dstg[c]<3){ dstg[c]++; dstt[c]=(dstg[c]==3)?0xFFFF:(dbl[c]?dbl[c]:96); }\n"
                "     o_=amp; }\n",
                (drum_mask & 7) ? "else " : "");
        fprintf(f,
            "    s=(double)o_/32768.; if(s>1.)s=1.; else if(s<-1.)s=-1.; } }\n");
        first = 0;
      }
      if (u_fm) {
        static const char *ALG[8] = {
          "s3=sa_(fp[c][3]+fb)*l3; s2=sa_(fp[c][2]+s3)*l2; s1=sa_(fp[c][1]+s2)*l1; raw=sa_(fp[c][0]+s1*mi); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s2=sa_(fp[c][2])*l2; s1=sa_(fp[c][1]+s3+s2)*l1; raw=sa_(fp[c][0]+s1*mi); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s2=sa_(fp[c][2]+s3)*l2; s1=sa_(fp[c][1])*l1; raw=sa_(fp[c][0]+(s2+s1)*mi); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s1=sa_(fp[c][1]+s3)*l1; s2=sa_(fp[c][2])*l2; raw=sa_(fp[c][0]+(s1+s2)*mi); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s2=sa_(fp[c][2])*l2; s1=sa_(fp[c][1])*l1; raw=sa_(fp[c][0]+(s3+s2+s1)*mi); sv=raw*l0/F;",
          "s3=sa_(fp[c][3]+fb)*l3; s1=sa_(fp[c][1])*l1; { double r2=sa_(fp[c][2]+s3*mi); raw=sa_(fp[c][0]+s1*mi); sv=(r2*l2+raw*l0)/F*0.5; }",
          "s3=sa_(fp[c][3]+fb)*l3; { double r2=sa_(fp[c][2]+s3*mi),r1=sa_(fp[c][1]); raw=sa_(fp[c][0]); sv=(r2*l2+r1*l1+raw*l0)/F/3.; }",
          "{ double r3=sa_(fp[c][3]+fb),r2=sa_(fp[c][2]),r1=sa_(fp[c][1]); raw=sa_(fp[c][0]); sv=(r3*l3+r2*l2+r1*l1+raw*l0)/F*0.25; }",
        };
        int n_alg = 0, last = 0;
        for (int a = 0; a < 8; a++) if (fm_mask & (1 << a)) { n_alg++; last = a; }
        fprintf(f, "   %s(CW[I]==6){ double sv=0.,raw=0.;\n"
                   "    double l0=fe[c][0]*FL[I][0]/F, l1=fe[c][1]*FL[I][1]/F;\n",
                   first?"if":"else if");
        if (u_fm4) fprintf(f, "    double l2=fe[c][2]*FL[I][2]/F, l3=fe[c][3]*FL[I][3]/F;\n");
        fprintf(f, "    double mi=(double)FMI[I]/255., fb=FFB[I]?fpv[c]*FFB[I]/256.:0.;\n");
        if (u_fm4) fprintf(f, "    double s1=0.,s2=0.,s3=0.;\n");
        if (u_fm4 && u_fm2) fprintf(f, "    if(FNO[I]>=4){\n");
        if (u_fm4) {
            if (n_alg <= 1) fprintf(f, "    %s\n", ALG[last]);
            else {
                fprintf(f, "    switch(FAL[I]&7){\n");
                for (int a = 0; a < 8; a++) {
                    if (!(fm_mask & (1 << a))) continue;
                    if (a == last) fprintf(f, "     default: %s break;\n", ALG[a]);
                    else           fprintf(f, "     case %d: %s break;\n", a, ALG[a]);
                }
                fprintf(f, "    }\n");
            }
        }
        if (u_fm4 && u_fm2) fprintf(f, "    } else {\n");
        if (u_fm2)
            fprintf(f,
                "    double mo=sa_(fp[c][1])*((double)FMI[I]*l1/255.);\n"
                "    if(FFB[I]) mo+=fpv[c]*FFB[I]/256.;\n"
                "    raw=sa_(fp[c][0]+mo); sv=raw*l0/F;\n");
        if (u_fm4 && u_fm2) fprintf(f, "    }\n");
        fprintf(f,
            "    fpv[c]=raw*F;\n"
            "    for(i32 k=0;k<%d;k++) fp[c][k]=(fp[c][k]+ff[c][k])%%F;\n"
            "    s=sv; }\n", NO);
        first = 0;
      }
      if (w_used[0]) { fprintf(f, "   %s(CW[I]==0){ s = h<(double)CU[I] ? .5 : -.5; }\n", first?"if":"else if"); first=0; }
      if (w_used[1]) { fprintf(f, "   %s(CW[I]==1){ s = h<(double)F/2 ? (h*4.-(double)F)/F : ((double)F*3.-h*4.)/F; }\n", first?"if":"else if"); first=0; }
      if (w_used[2]) { fprintf(f, "   %s(CW[I]==2){ s = (h*2.-(double)F)/F; }\n", first?"if":"else if"); first=0; }
      if (w_used[4]) { fprintf(f, "   %s(CW[I]==4){ s = sa_(h); }\n", first?"if":"else if"); first=0; }
      if (w_used[3]) {
        fprintf(f, "   %s{ lm[c]++; if(lm[c]>=lj[c]){ lm[c]=0; i32 b=(lh[c]^(lh[c]>>1))&1; lh[c]=(lh[c]>>1)|(b<<14); }\n"
                   "     s = (lh[c]&1) ? .5 : -.5; }\n", first?"" : "else ");
        first=0;
      }
      if (first) fprintf(f, "   s=0.;\n"); }

    if (u_drive)
        fprintf(f, "   if(DVP[I]>1.) s=SS(s*DVP[I])*DVN[I];\n");
    fprintf(f, "   double en=ev_[c];\n");
    if (u_trem)
        fprintf(f,
            "   if(tm[c]){ en+=en*tm[c]/(double)F; if(en<0.)en=0.; if(en>(double)F)en=(double)F; }\n");
    fprintf(f,
        "   double cv = s*en*CV[I]*rv[c]/F/255./255.*TG[CW[I]];\n");
    if (u_duck)
        fprintf(f,
            "   if(DS[I]) DI += (cv<0.?-cv:cv)*DS[I]/255.;\n"
            "   if(DA[I]&&DN>0.){ double dg=1.-DN*DA[I]/255.; cv*= dg>0.?dg:0.; }\n");
    fprintf(f, "   v += cv;\n");
    if (u_rev) fprintf(f, "   if(RS[I]) ri += cv*RS[I]/255.;\n");
    { char guard[96]; guard[0] = 0;
      int n = 0;
      if (u_fm)   { strcat(guard, n++ ? "&&CW[I]!=6" : "CW[I]!=6"); }
      if (u_drum) { strcat(guard, n++ ? "&&CW[I]!=8" : "CW[I]!=8"); }
      if (u_smp)  { strcat(guard, n++ ? "&&CW[I]!=5" : "CW[I]!=5"); }
      if (n) fprintf(f, "   if(%s){ ph[c]+=fq[c]; if(ph[c]>=(double)F) ph[c]-=(double)F; }\n", guard);
      else   fprintf(f, "   ph[c]+=fq[c]; if(ph[c]>=(double)F) ph[c]-=(double)F;\n"); }
    fprintf(f, "  }\n");
    if (u_rev)  fprintf(f, "  v += rev_(ri);\n");
    if (u_duck) fprintf(f, "  { double dd=DI>1.?1.:DI; DE = dd>DE ? dd : dd+(DE-dd)*DR; }\n");
    fprintf(f, "  v *= MG;\n");
    if (master)
        fprintf(f,
            "  { double la=v<0.?-v:v; LE = la>LE ? la : la+(LE-la)*MR;\n"
            "    if(LE>MT) v*=MT/LE; v=SS(v)/MC; }\n");
    else
        fprintf(f, "  if(v>1.) v=1.; else if(v<-1.) v=-1.;\n");
    fprintf(f,
        /* the JS engine writes into a Float32Array, so each sample is
     * rounded to single precision before it is scaled */
    "  out[i] = (i16)((double)(float)v*32767.);\n"
        " }\n}\n");

    bc_emit_dev(f);
    fclose(f);
    fprintf(stderr, "Wrote %s (%d channels, %d instrument rows, %d events, %ld samples)\n",
            filename, nch, NI, bc_nev, (long)bc_walk_T);
    if (bc_ev) { free(bc_ev); bc_ev = NULL; bc_nev = 0; }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s input.[birb|bin] [-o output_base] [--js]\n", prog);
    fprintf(stderr, "  Inputs:  .birb  text format\n");
    fprintf(stderr, "           .bsb   binary format (from editor)\n");
    fprintf(stderr, "  Outputs: output_base.bsb  (binary)\n");
    fprintf(stderr, "           output_base.h    (C header)\n");
    fprintf(stderr, "           --js             also emit .js for 4K demos\n");
    fprintf(stderr, "  Other:   --version | -v   print version and exit\n");
    fprintf(stderr, "           --no-master        omit the JS master bus (smaller, CHANGES SOUND)\n");
    fprintf(stderr, "           --smol             smol birb: minimal export (smallest, CHANGES SOUND)\n");
    fprintf(stderr, "           --smol-c           emit a standalone C player with the song baked in\n");
    fprintf(stderr, "           --locked-c         experimental: locked-down C player, full feature set\n");
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("birbc (birb) %s\n", BIRB_VERSION);
        return 0;
    }
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *input = argv[1];
    const char *output_base = NULL;
    int emit_js = 0, emit_smol_c = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_base = argv[++i];
        } else if (strcmp(argv[i], "--smol-c") == 0) {
            emit_smol_c = 1;
            birb_smol = 1;          /* the C player is the smol feature set */
        } else if (strcmp(argv[i], "--locked-c") == 0) {
            birb_locked = 1;
        } else if (strcmp(argv[i], "--js") == 0) {
            emit_js = 1;
        } else if (strcmp(argv[i], "--smol") == 0) {
            birb_smol = 1;
            birb_no_master = 1;      /* the limiter is part of what smol drops */
        } else if (strcmp(argv[i], "--no-master") == 0) {
            birb_no_master = 1;
        }
    }

    /* derive output base from input if not specified */
    char base_buf[256];
    if (!output_base) {
        strncpy(base_buf, input, sizeof(base_buf) - 1);
        base_buf[sizeof(base_buf) - 1] = '\0';
        char *dot = strrchr(base_buf, '.');
        if (dot) *dot = '\0';
        output_base = base_buf;
    }

    /* parse — detect format from extension */
    static birb_song song;
    int input_is_bin = 0;
    {
        const char *ext = strrchr(input, '.');
        if (ext && (strcmp(ext, ".bin") == 0 || strcmp(ext, ".bsb") == 0)) input_is_bin = 1;
    }

    if (input_is_bin) {
        if (load_bin_file(input, &song) < 0) return 1;
    } else {
        if (parse_birb_file(input, &song) < 0) return 1;
    }

    /* output filenames */
    char bin_name[280], h_name[280], js_name[280];
    snprintf(bin_name, sizeof(bin_name), "%s.bsb", output_base);
    snprintf(h_name, sizeof(h_name), "%s.h", output_base);
    snprintf(js_name, sizeof(js_name), "%s.js", output_base);

    /* write binary */
    if (write_binary(bin_name, &song) < 0) return 1;

    /* write C header */
    if (write_c_header(h_name, bin_name) < 0) return 1;

    /* write JS (4K demo target) */
    if (emit_js) {
        if (write_js(js_name, &song) < 0) return 1;
    }

    /* experimental full-feature locked-down C player */
    if (birb_locked) {
        char c_name[512];
        snprintf(c_name, sizeof c_name, "%s_locked.c", output_base);
        if (write_locked_c(c_name, &song) < 0) return 1;
    }

    /* write the standalone C player (native / wasm 4K target) */
    if (emit_smol_c) {
        char c_name[512];
        snprintf(c_name, sizeof c_name, "%s_smol.c", output_base);
        if (write_smol_c(c_name, &song) < 0) return 1;
    }

    return 0;
}
