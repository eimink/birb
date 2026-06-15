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
        }
    }

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

static int write_js(const char *filename, birb_song *song) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write '%s'\n", filename);
        return -1;
    }

    fprintf(f, "function birb(X){\n");
    fprintf(f, "var S=44100,N=4,F=65536,\n");
    fprintf(f, "bpm=%d,tpr=%d,ni=%d,np=%d,ol=%d,\n",
            song->bpm, song->ticks_per_row, song->num_instruments,
            song->num_patterns, song->order_length);

    /* order — single flat array, 4 per position */
    fprintf(f, "O=[");
    for (int i = 0; i < song->order_length; i++) {
        if (i) fprintf(f, ",");
        fprintf(f, "[%d,%d,%d,%d]", song->order[i][0], song->order[i][1],
                song->order[i][2], song->order[i][3]);
    }
    fprintf(f, "],\n");

    /* Detect which synth types we emit. */
    int uses_fm = 0, uses_ks = 0, uses_drum = 0, uses_formant = 0, uses_sine = 0;
    int drum_algos = 0;  /* bitmask: bit 0=KICK/TOM, 1=SNARE, 2=HAT/CRASH, 3=CLAP */
    for (int i = 0; i < song->num_instruments; i++) {
        if (song->instruments[i].synth_type == SYNTH_FM) uses_fm = 1;
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
    }

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

    /* pattern lengths */
    fprintf(f, "pl=[");
    for (int p = 0; p < song->num_patterns; p++) {
        if (p) fprintf(f, ",");
        fprintf(f, "%d", song->pattern_lengths[p] ? song->pattern_lengths[p] : 64);
    }
    fprintf(f, "],\n");

    /* 5 flat arrays with offset table for variable-length patterns */
    const char *pnames[] = {"pn", "pi", "pv", "pf", "pp"};
    for (int plane = 0; plane < 5; plane++) {
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
        if (all_empty) {
            /* emit empty array — engine will handle undefined as 0/255 */
            fprintf(f, "%s=[],\n", pnames[plane]);
            continue;
        }
        fprintf(f, "%s=[", pnames[plane]);
        int first = 1;
        for (int p = 0; p < song->num_patterns; p++) {
            int nrows = song->pattern_lengths[p]; if (!nrows) nrows = 64;
            for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
                for (int r = 0; r < nrows; r++) {
                    if (!first) fprintf(f, ",");
                    first = 0;
                    uint8_t val = 0;
                    switch (plane) {
                        case 0: val = song->patterns[p][r][c].note; break;
                        case 1: val = song->patterns[p][r][c].instrument; break;
                        case 2: val = song->patterns[p][r][c].volume; break;
                        case 3: val = song->patterns[p][r][c].effect; break;
                        case 4: val = song->patterns[p][r][c].param; break;
                    }
                    fprintf(f, "%d", val);
                }
        }
        fprintf(f, "],\n");
    }

    /* offset table + accessor for variable-length patterns */
    fprintf(f,
        "po=[0],i,c,r\n"
        "for(i=1;i<np;i++)po[i]=po[i-1]+pl[i-1]*N\n"
        "function P(a,p,c,r){return a.length?a[po[p]+c*pl[p]+r]||0:0}\n"
    );

    /* synth engine */
    fprintf(f,
        "var bf=[24,26,27,29,31,32,34,36,38,41,43,46],\n"
        "dv=[8192,16384,32768,49152],\n"
        "nf=n=>(n=n<0?0:n>95?95:n,bf[n%%12]<<(n/12)),\n"
        "spt=S*5/((bpm||125)*2)|0,W=pl.reduce((a,b)=>a>b?a:b,1),T=ol*W*tpr*spt,\n"
        "out=new Float32Array(T),ch=[],ct=0,cr=0,op=0,tc=0\n");
    fprintf(f,
        "for(c=0;c<N;c++)ch[c]={p:0,f:0,b:0,w:0,n:0,u:F/2,e:0,t:0,a:0,d:0,s:0,r:0,q:0,g:0,x:0,y:0,k:0,l:0,h:0x7FFF,j:16,m:0,i:0,v:255,rv:255,pt:0,ps:0,ri:0,nc:0,nd:0,dn:0,di:0,vp:0,vs:0,vd:0,tp:0,ts:0,td:0,tm:0%s%s%s%s}\n"
        "var jo=-1,jr=0\n",
        uses_fm ? ",fmp:[0,0,0,0],fmf:[0,0,0,0],fmL:[0,0,0,0],fmR:[1,1,0,0],fmEnv:[0,0,0,0],fmStg:[0,0,0,0],fmAlgo:0,fmMi:64,fmFb:0,fmNo:2,fmPrev:0" : "",
        uses_ks ? ",kb:new Int16Array(1024),kl:0,kp:0,kd:0" : "",
        uses_drum ? ",drAl:0,drAlOrig:0,drP2:0,drPe:0,drPet:0,drRate:0,drSnap:0,drClk:0,drZ1:0,drZ2:0,drLf:0x7FFF,drTtl:0,drMix:0,drStage:0,drStageT:0,drBurstLen:0,drBodyT:0" : "",
        uses_formant ? ",ftSw:0,ftLf:0x7FFF,ftVa:0,ftVb:0,ftSp:0,ftDr:1,ftSS:0,ftR:128,ftRc:0,ftZ1:[0,0,0],ftZ2:[0,0,0],ftB0:[0,0,0],ftA1:[0,0,0],ftA2:[0,0,0]" : "");
    if (uses_fm || uses_drum || uses_sine) {
        fprintf(f,
            "function SA_(ph){var p=((ph%%F)+F)%%F,ng=p>=F/2,t=ng?p-F/2:p;if(t>F/4)t=F/2-t;var x=t/F*4;if(x>1)x=1;var x2=x*x;var y=x*(1.5707288-x2*(0.6432292-x2*0.0727778));if(y>1)y=1;return ng?-y:y}\n");
    }
    if (uses_formant) {
        /* Runtime RBJ bandpass coefficient computation. Vowel frequencies
         * (Hz) × 3 formants per vowel; per-formant gains baked into b0.
         * Coeffs recomputed when vowel or Q changes; sweep just interps. */
        fprintf(f,
            "var FFREQS=[[730,1090,2440],[530,1840,2480],[270,2290,3010],[570,840,2410],[300,870,2240]],\n"
            "FGAINS=[1.0,0.7,0.4]\n"
            "function FCC(vw,q,dst){for(var i=0;i<3;i++){var om=2*Math.PI*FFREQS[vw][i]/44100,sn=Math.sin(om),cs=Math.cos(om),al=sn/(2*q),iv=1/(1+al);dst[i*3+0]=(al*iv*FGAINS[i]*F)|0;dst[i*3+1]=(-2*cs*iv*F)|0;dst[i*3+2]=((1-al)*iv*F)|0}}\n"
            "function FI(C){var q=2+(C.ftR/255)*30;if(!C._FCA){C._FCA=new Float64Array(9);C._FCB=new Float64Array(9);C._FQ=-1;C._FVA=-1;C._FVB=-1}var va=C.ftVa,vb=C.ftVb;if(C._FQ!==q||C._FVA!==va){FCC(va,q,C._FCA);C._FVA=va}if(C._FQ!==q||C._FVB!==vb){FCC(vb,q,C._FCB);C._FVB=vb}C._FQ=q;var t=C.ftSp,omt=255-t;for(var i=0;i<3;i++){C.ftB0[i]=((C._FCA[i*3+0]*omt+C._FCB[i*3+0]*t)/255)|0;C.ftA1[i]=((C._FCA[i*3+1]*omt+C._FCB[i*3+1]*t)/255)|0;C.ftA2[i]=((C._FCA[i*3+2]*omt+C._FCB[i*3+2]*t)/255)|0}}\n");
    }
    if (uses_fm) {
        /* 4-op FM. 8 algos, mirrors fmRender4() in the editor. raw is the
         * carrier op0's pre-level sine; fmPrev = raw*F so feedback maths
         * matches 2-op exactly. */
        fprintf(f,
            "function FM4(C){var l0=C.fmL[0]*C.fmEnv[0]/F,l1=C.fmL[1]*C.fmEnv[1]/F,l2=C.fmL[2]*C.fmEnv[2]/F,l3=C.fmL[3]*C.fmEnv[3]/F;\n"
            "var mi=C.fmMi/255,fb=C.fmFb?C.fmPrev*C.fmFb/256:0;var s,raw,s3,s2,s1;\n"
            "switch(C.fmAlgo&7){\n"
            "case 0:s3=SA_(C.fmp[3]+fb)*l3;s2=SA_(C.fmp[2]+s3)*l2;s1=SA_(C.fmp[1]+s2)*l1;raw=SA_(C.fmp[0]+s1*mi);s=raw*l0/F;break;\n"
            "case 1:s3=SA_(C.fmp[3]+fb)*l3;s2=SA_(C.fmp[2])*l2;s1=SA_(C.fmp[1]+s3+s2)*l1;raw=SA_(C.fmp[0]+s1*mi);s=raw*l0/F;break;\n"
            "case 2:s3=SA_(C.fmp[3]+fb)*l3;s2=SA_(C.fmp[2]+s3)*l2;s1=SA_(C.fmp[1])*l1;raw=SA_(C.fmp[0]+(s2+s1)*mi);s=raw*l0/F;break;\n"
            "case 3:s3=SA_(C.fmp[3]+fb)*l3;s1=SA_(C.fmp[1]+s3)*l1;s2=SA_(C.fmp[2])*l2;raw=SA_(C.fmp[0]+(s1+s2)*mi);s=raw*l0/F;break;\n"
            "case 4:s3=SA_(C.fmp[3]+fb)*l3;s2=SA_(C.fmp[2])*l2;s1=SA_(C.fmp[1])*l1;raw=SA_(C.fmp[0]+(s3+s2+s1)*mi);s=raw*l0/F;break;\n"
            "case 5:{s3=SA_(C.fmp[3]+fb)*l3;s1=SA_(C.fmp[1])*l1;var r2=SA_(C.fmp[2]+s3*mi);raw=SA_(C.fmp[0]+s1*mi);s=(r2*l2+raw*l0)/F*0.5;break}\n"
            "case 6:{s3=SA_(C.fmp[3]+fb)*l3;var r2=SA_(C.fmp[2]+s3*mi),r1=SA_(C.fmp[1]);raw=SA_(C.fmp[0]);s=(r2*l2+r1*l1+raw*l0)/F/3;break}\n"
            "default:{var r3=SA_(C.fmp[3]+fb),r2=SA_(C.fmp[2]),r1=SA_(C.fmp[1]);raw=SA_(C.fmp[0]);s=(r3*l3+r2*l2+r1*l1+raw*l0)/F*0.25;break}}\n"
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
        "C.q=j[6];C.g=j[7];C.x=j[8];C.y=j[9];C.v=j[10]||255;C.rv=255;C.k=0;C.l=0;C.ps=0\n"
        "if(j[0]===3){C.h=0x7FFF;C.m=0;C.j=256>>(s/12)||1}%s%s%s%s}\n",
        uses_fm
            ? "\nif(j[0]===6){var fm=j[15];C.fmNo=j[11];C.fmAlgo=j[12]|0;C.fmFb=j[13];C.fmMi=j[14];C.fmPrev=0;for(var k=0;k<4;k++){var o_=fm[k];C.fmp[k]=0;C.fmR[k]=(o_[0]*16+(o_[1]&15))/16;C.fmf[k]=Math.round(C.b*C.fmR[k]);C.fmL[k]=Math.round(F*o_[2]/255);if((o_[3]|0)==0){C.fmEnv[k]=F;C.fmStg[k]=2}else{C.fmEnv[k]=0;C.fmStg[k]=1}}}"
            : "",
        uses_ks
            ? "\nif(j[0]===7){var ln=C.b>0?F/C.b|0:0;if(ln<4)ln=4;if(ln>1024)ln=1024;C.kl=ln;C.kp=0;C.kd=j[16]||0;var lf=(0x7FFF^(s*0x1D79&0xFFFF))&0xFFFF;if(!lf)lf=0x7FFF;for(var ki=0;ki<ln;ki++){var kbit=(lf^(lf>>1))&1;lf=((lf>>1)|(kbit<<14))&0xFFFF;C.kb[ki]=(lf&1)?16383:-16383}}"
            : "",
        uses_drum
            ? "\nif(j[0]===8){var dt=j[17]&7,al=dt===4?0:dt===5?2:dt,tn=j[18];if(tn>127)tn-=256;var dec=j[19],tone=j[20],snp=j[21],dn=s+tn;if(dn<0)dn=0;if(dn>95)dn=95;var df=nf(dn),tt;C.drAl=al;C.drAlOrig=dt;C.drP2=0;C.drZ1=0;C.drZ2=0;C.drLf=(0x7FFF^(s*0x3D7F&0xFFFF))&0xFFFF;if(!C.drLf)C.drLf=0x7FFF;"
              "if(al===0){C.drPe=df<<1;C.drPet=df>>1;C.drRate=Math.max(16,tone*256);C.drSnap=snp;C.drClk=384;tt=dec*200+1024;if(dt===4)tt*=2}"
              "else if(al===1){C.f=df>0?df:24<<2;C.drMix=snp;C.drBodyT=64+(tone>>1);tt=dec*120+1024}"
              "else if(al===2){var hp=snp*(F*15/16/255)|0;if(hp<F>>4)hp=F>>4;C.drPet=hp;C.drZ1=0;tt=dec*180+1024;if(dt===5)tt=90000+dec*400}"
              "else{C.drBurstLen=80+(snp>>1);C.drStage=0;C.drStageT=C.drBurstLen;tt=dec*160+2048}"
              "if(tt>0xFFFFFF)tt=0xFFFFFF;C.drTtl=tt}"
            : "",
        formant_trigger
    );
    (void)ks_idx; (void)drum_base; /* indices used via hardcoded layout above */
    (void)formant_base;
    fprintf(f,
        "function R(){for(c=0;c<N;c++){var q=O[op][c],C=ch[c];if(q>=np)continue\n"
        "var n=P(pn,q,c,cr),ii=P(pi,q,c,cr)||255,rv=P(pv,q,c,cr),fx=P(pf,q,c,cr),pm=P(pp,q,c,cr)\n"
        "C.ri=0;C.nc=0;C.nd=0\n"
        "var itp=fx==5,ind=fx==7&&(pm>>4)==0xD\n"
        "if(ind&&n>=2){C.dn=n;C.di=ii==255?C.i:ii;C.nd=pm&15}\n"
        "else if(n==1){C.t=4;if(C.fmStg)for(var k=0;k<4;k++)if(C.fmStg[k])C.fmStg[k]=4}else if(n>=2){if(itp){C.pt=nf(n-2)}\n"
        "else{if(ii==255)ii=C.i;if(ii<ni)TR(C,n,ii)}}\n"
        "if(rv)C.rv=rv\n"
        "if(fx==1){C.x=pm>>4;C.y=pm&15;C.k=0}\n"
        "else if(fx==2)C.l=pm<<2;else if(fx==3)C.l=-(pm<<2)\n"
        "else if(fx==4){C.vs=F/64*(pm>>4);C.vd=(pm&15)<<4}\n"
        "else if(fx==5)C.ps=pm<<2\n"
        "else if(fx==6)C.ri=pm\n"
        "else if(fx==7&&(pm>>4)==0xC)C.nc=pm&15\n"
        "else if(fx==8){C.ts=F/64*(pm>>4);C.td=(pm&15)<<4}\n"
        "else if(fx==9&&C.w===5)C.sp=(pm<<8)<<16\n"
        "else if(fx==0xB){jo=pm;jr=0}\n"
        "else if(fx==0xD){if(jo<0)jo=op+1;jr=pm}\n"
        "else if(fx==0xF&&pm){if(pm<0x20)tpr=pm;else{bpm=pm;spt=S*5/(pm*2)|0}}}}\n"
        "R()\n"
        "function K(){ct++;if(ct>=tpr){ct=0\n"
        "if(jo>=0){op=jo%%ol;cr=jr;jo=-1;jr=0}else{cr++\n"
        "if(cr>=pl[O[op][0]]){cr=0;if(++op>=ol)op=0}}R()}\n"
        "for(c=0;c<N;c++){var C=ch[c]\n"
        "if(C.nd&&ct==C.nd){if(C.di<ni)TR(C,C.dn,C.di);C.nd=0}\n"
        "if(C.nc&&ct==C.nc){C.e=0;C.t=0}\n"
        "if(C.ri&&ct>0&&ct%%C.ri==0){C.p=0;C.t=1;C.e=0;if(C.w>=3){C.h=0x7FFF;C.m=0}}\n"
        "if(C.g){C.b+=C.q<<2;if(C.b<1)C.b=1;C.g--}\n"
        "if(C.l){C.b+=C.l;if(C.b<1)C.b=1}\n"
        "if(C.pt&&C.ps){if(C.b<C.pt){C.b+=C.ps;if(C.b>C.pt)C.b=C.pt}else if(C.b>C.pt){C.b-=C.ps;if(C.b<C.pt)C.b=C.pt}}\n"
        "if(C.x|C.y){var n=C.n,t=C.k%%3;C.f=nf(t==1?n+C.x:t==2?n+C.y:n);C.k++}else C.f=C.b\n"
        "if(C.vd){C.f+=((C.vp&65535)*4-F*2>>8)*C.vd/F;C.vp+=C.vs}\n"
        "if(C.td){C.tm=((C.tp&65535)*4-F*2>>8)*C.td/F;C.tp+=C.ts}else C.tm=0\n"
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
            "if(C.w===6&&C.i<ni){var jj=I[C.i],fm=jj[15];C.fmFb=jj[13];C.fmMi=jj[14];C.fmNo=jj[11];C.fmAlgo=jj[12]|0;\n"
            "for(var k=0;k<4;k++){var oo=fm[k];C.fmR[k]=(oo[0]*16+(oo[1]&15))/16;C.fmf[k]=Math.round(C.f*C.fmR[k]);C.fmL[k]=Math.round(F*(oo[2]||0)/255);\n"
            "var st=C.fmStg[k],en=C.fmEnv[k],oa=oo[3]|0,od=oo[4]|0,os=oo[5]|0,oR=oo[6]|0;\n"
            "if(st===1){en+=F/(oa+1);if(en>=F){en=F;st=2}}else if(st===2){var g2=(F*os/255)|0;en-=(F-g2)/(od+1);if(en<=g2){en=g2;st=3}}else if(st===4){en-=en/(oR+1);if(en<64){en=0;st=0}}\n"
            "C.fmStg[k]=st;C.fmEnv[k]=en}}\n");
    }
    if (uses_ks) {
        fprintf(f, "if(C.w===7&&C.i<ni){var jk=I[C.i];C.kd=jk[16]||0}\n");
    }
    if (uses_drum) {
        fprintf(f,
            "if(C.w===8&&C.i<ni){var jd=I[C.i];C.drSnap=jd[21];C.drMix=jd[21];C.drRate=Math.max(16,jd[20]*256);if(C.drAl===2){var hpL=jd[21]*(F*15/16/255)|0;if(hpL<F>>4)hpL=F>>4;C.drPet=hpL}}\n");
    }
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
        "var v=0;for(c=0;c<N;c++){var C=ch[c];if(!C.t&&!C.e%s)continue\n"
        "var h=C.p,s;\n",
        uses_drum ? "&&!C.drTtl" : "");
    if (uses_fm) {
        /* nops=4 → FM4 (8 algos); nops=2 → simpler op1→op0 with feedback. */
        fprintf(f,
            "if(C.w===6){if(C.fmNo>=4){s=FM4(C)}else{var l1f=C.fmL[1]*C.fmEnv[1]/F,l0f=C.fmL[0]*C.fmEnv[0]/F;var mo=SA_(C.fmp[1])*(C.fmMi*l1f/255);if(C.fmFb)mo+=C.fmPrev*C.fmFb/256;var cr_=SA_(C.fmp[0]+mo);C.fmPrev=cr_*F;C.fmp[0]=(C.fmp[0]+C.fmf[0])%%F;C.fmp[1]=(C.fmp[1]+C.fmf[1])%%F;s=cr_*l0f/F}}else \n");
    }
    if (uses_ks) {
        fprintf(f,
            "if(C.w===7){if(C.kl<2)s=0;else{var kpp=C.kp,knx=kpp+1;if(knx>=C.kl)knx=0;var kcur=C.kb[kpp];C.kb[kpp]=(((kcur+C.kb[knx])>>1)*(255-C.kd))>>8;C.kp=knx;s=kcur/32768}}else \n");
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
                "if(C.drAl===0){var gp=C.drPe-C.drPet;C.drPe-=(gp*C.drRate)>>20;C.p=(C.p+C.drPe)%%F;var tri=C.p<F/2?(C.p*4-F):(F*3-C.p*4);o_=((tri*28000/F)|0);if(C.drClk>0){var nn=lfn();var pk=C.drSnap*128,ap=(pk*C.drClk/384)|0;o_+=(nn&1)?ap:-ap;C.drClk--}}");
        if (drum_algos & 2)
            fprintf(f,
                "%sif(C.drAl===1){var nn=lfn();var noi=(nn&1)?26000:-26000;C.p=(C.p+C.f)%%F;var bd=C.p<F/2?22000:-22000;if(C.drBodyT>0){bd=(bd*C.drBodyT/256)|0;C.drBodyT--}else bd=0;var mxB=C.drMix,mxN=255-mxB;o_=((noi*mxN+bd*mxB)/255)|0}",
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
        "if(!C.w)s=h<C.u?.5:-.5\n"
        "else if(C.w==1)s=h<F/2?(h*4-F)/F:(F*3-h*4)/F\n"
        "else if(C.w<3)s=(h*2-F)/F\n"
        "else if(C.w==4)s=SA_(h)\n"
        "else{C.m++;if(C.m>=C.j){C.m=0;var z=(C.h^(C.h>>1))&1;C.h=(C.h>>1)|(z<<14)}s=(C.h&1)?.5:-.5}\n"
        "var en=C.e+(C.tm?C.e*C.tm/F:0);if(en<0)en=0;if(en>F)en=F\n"
        "v+=s*en*C.v*C.rv/F/255/255;%sC.p=(C.p+C.f)%%F}out[i]=v>1?1:v<-1?-1:v}\n"
        "return{o:out,spt:spt,T:T}}\n",
        uses_drum ? "if(C.w!==8)" : "");

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

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s input.[birb|bin] [-o output_base] [--js]\n", prog);
    fprintf(stderr, "  Inputs:  .birb  text format\n");
    fprintf(stderr, "           .bsb   binary format (from editor)\n");
    fprintf(stderr, "  Outputs: output_base.bsb  (binary)\n");
    fprintf(stderr, "           output_base.h    (C header)\n");
    fprintf(stderr, "           --js             also emit .js for 4K demos\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *input = argv[1];
    const char *output_base = NULL;
    int emit_js = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_base = argv[++i];
        } else if (strcmp(argv[i], "--js") == 0) {
            emit_js = 1;
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

    return 0;
}
