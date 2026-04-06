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

    /* parse key:value pairs */
    const char *s = strstr(line, wavename);
    if (s) s += strlen(wavename);
    else return -1;

    while (s && *s) {
        s = skip_ws(s);
        if (*s == '\0' || *s == '\n' || *s == '#') break;

        if (s[0] == 'A' && s[1] == ':') {
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

    /* header */
    uint8_t hdr[8] = {
        BIRB_MAGIC_0, BIRB_MAGIC_1, BIRB_MAGIC_2, BIRB_MAGIC_3,
        song->bpm, song->ticks_per_row, song->num_instruments, song->num_patterns
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
            0 /* reserved */
        };
        fwrite(buf, 1, BIRB_INST_SIZE, f);
    }

    /* patterns (planar) */
    int total_pattern_bytes = 0;
    for (int p = 0; p < song->num_patterns; p++) {
        int nrows = song->pattern_lengths[p];
        if (nrows == 0) nrows = BIRB_MAX_ROWS;
        fwrite(&nrows, 1, 1, f);
        total_pattern_bytes++;

        /* notes plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++) {
                fwrite(&song->patterns[p][r][c].note, 1, 1, f);
                total_pattern_bytes++;
            }

        /* instrument plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++) {
                fwrite(&song->patterns[p][r][c].instrument, 1, 1, f);
                total_pattern_bytes++;
            }

        /* volume plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++) {
                fwrite(&song->patterns[p][r][c].volume, 1, 1, f);
                total_pattern_bytes++;
            }

        /* effect plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++) {
                fwrite(&song->patterns[p][r][c].effect, 1, 1, f);
                total_pattern_bytes++;
            }

        /* param plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++) {
                fwrite(&song->patterns[p][r][c].param, 1, 1, f);
                total_pattern_bytes++;
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

    /* instruments — flat arrays, 11 per instrument */
    fprintf(f, "I=[");
    for (int i = 0; i < song->num_instruments; i++) {
        birb_instrument *inst = &song->instruments[i];
        if (i) fprintf(f, ",");
        fprintf(f, "[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",
                (int)inst->waveform, birb_duty_encode(inst->duty),
                inst->envelope.attack, inst->envelope.decay,
                inst->envelope.sustain, inst->envelope.release,
                (int)inst->pitch_env, inst->pitch_env_len,
                inst->arp_note1, inst->arp_note2,
                inst->volume);
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
        "out=new Float32Array(T),ch=[],ct=0,cr=0,op=0,tc=0\n"
        "for(c=0;c<N;c++)ch[c]={p:0,f:0,b:0,w:0,n:0,u:F/2,e:0,t:0,a:0,d:0,s:0,r:0,q:0,g:0,x:0,y:0,k:0,l:0,h:0x7FFF,j:16,m:0,i:0,v:255,rv:255,pt:0,ps:0,ri:0,nc:0,nd:0,dn:0,di:0,vp:0,vs:0,vd:0}\n"
        "function TR(C,n,ii){C.i=ii;var s=n-2,j=I[ii]\n"
        "C.n=s;C.b=nf(s);C.f=C.b;C.p=0;C.w=j[0];C.u=dv[j[1]&3]\n"
        "C.a=j[2];C.d=j[3];C.s=j[4];C.r=j[5];C.t=1;C.e=0\n"
        "C.q=j[6];C.g=j[7];C.x=j[8];C.y=j[9];C.v=j[10]||255;C.rv=255;C.k=0;C.l=0;C.ps=0\n"
        "if(j[0]>=3){C.h=0x7FFF;C.m=0;C.j=256>>(s/12)||1}}\n"
        "function R(){for(c=0;c<N;c++){var q=O[op][c],C=ch[c];if(q>=np)continue\n"
        "var n=P(pn,q,c,cr),ii=P(pi,q,c,cr)||255,rv=P(pv,q,c,cr),fx=P(pf,q,c,cr),pm=P(pp,q,c,cr)\n"
        "C.ri=0;C.nc=0;C.nd=0\n"
        "var itp=fx==5,ind=fx==7&&(pm>>4)==0xD\n"
        "if(ind&&n>=2){C.dn=n;C.di=ii==255?C.i:ii;C.nd=pm&15}\n"
        "else if(n==1)C.t=4;else if(n>=2){if(itp){C.pt=nf(n-2)}\n"
        "else{if(ii==255)ii=C.i;if(ii<ni)TR(C,n,ii)}}\n"
        "if(rv)C.rv=rv\n"
        "if(fx==1){C.x=pm>>4;C.y=pm&15;C.k=0}\n"
        "else if(fx==2)C.l=pm<<2;else if(fx==3)C.l=-(pm<<2)\n"
        "else if(fx==4){C.vs=F/64*(pm>>4);C.vd=(pm&15)<<4}\n"
        "else if(fx==5)C.ps=pm<<2\n"
        "else if(fx==6)C.ri=pm\n"
        "else if(fx==7&&(pm>>4)==0xC)C.nc=pm&15}}\n"
        "R()\n"
        "function K(){ct++;if(ct>=tpr){ct=0;cr++\n"
        "if(cr>=pl[O[op][0]]){cr=0;if(++op>=ol)op=0}R()}\n"
        "for(c=0;c<N;c++){var C=ch[c]\n"
        "if(C.nd&&ct==C.nd){if(C.di<ni)TR(C,C.dn,C.di);C.nd=0}\n"
        "if(C.nc&&ct==C.nc){C.e=0;C.t=0}\n"
        "if(C.ri&&ct>0&&ct%%C.ri==0){C.p=0;C.t=1;C.e=0;if(C.w>=3){C.h=0x7FFF;C.m=0}}\n"
        "if(C.g){C.b+=C.q<<2;if(C.b<1)C.b=1;C.g--}\n"
        "if(C.l){C.b+=C.l;if(C.b<1)C.b=1}\n"
        "if(C.pt&&C.ps){if(C.b<C.pt){C.b+=C.ps;if(C.b>C.pt)C.b=C.pt}else if(C.b>C.pt){C.b-=C.ps;if(C.b<C.pt)C.b=C.pt}}\n"
        "if(C.x|C.y){var n=C.n,t=C.k%%3;C.f=nf(t==1?n+C.x:t==2?n+C.y:n);C.k++}else C.f=C.b\n"
        "if(C.vd){C.f+=((C.vp&65535)*4-F*2>>8)*C.vd/F;C.vp+=C.vs}\n"
        "var e=C.t;if(e==1){C.e+=F/(C.a+1);if(C.e>=F){C.e=F;C.t=2}}\n"
        "else if(e==2){var g=F*C.s/255;C.e-=(F-g)/(C.d+1);if(C.e<=g){C.e=g;C.t=3}}\n"
        "else if(e==4){C.e-=C.e/(C.r+1);if(C.e<64){C.e=0;C.t=0}}}}\n"
        "for(i=0;i<T;i++){if(tc<=0){K();tc=spt}tc--\n"
        "var v=0;for(c=0;c<N;c++){var C=ch[c];if(!C.t&&!C.e)continue\n"
        "var h=C.p,s;if(!C.w)s=h<C.u?.5:-.5\n"
        "else if(C.w==1)s=h<F/2?(h*4-F)/F:(F*3-h*4)/F\n"
        "else if(C.w<3)s=(h*2-F)/F\n"
        "else{C.m++;if(C.m>=C.j){C.m=0;var z=(C.h^(C.h>>1))&1;C.h=(C.h>>1)|(z<<14)}s=(C.h&1)?.5:-.5}\n"
        "v+=s*C.e*C.v*C.rv/F/255/255;C.p=(C.p+C.f)%%F}out[i]=v>1?1:v<-1?-1:v}\n"
        "return{o:out,spt:spt,T:T}}\n"
    );

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
