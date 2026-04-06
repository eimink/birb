/*
 * birb_wav.c — WAV file exporter + test song
 * This is a dev tool, stdlib is fine here.
 */
#include <stdio.h>
#include <string.h>
#include "birb_synth.h"

/* ---------- WAV writer ---------- */

static void write16(FILE *f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static void write32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void write_wav(const char *filename, int16_t *samples, int num_samples) {
    FILE *f = fopen(filename, "wb");
    if (!f) { printf("Error: cannot open %s\n", filename); return; }

    uint32_t data_size = (uint32_t)(num_samples * 2);
    uint32_t file_size = 36 + data_size;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    write32(f, file_size);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    write32(f, 16);                     /* chunk size */
    write16(f, 1);                      /* PCM */
    write16(f, 1);                      /* mono */
    write32(f, BIRB_SAMPLE_RATE);       /* sample rate */
    write32(f, BIRB_SAMPLE_RATE * 2);   /* byte rate */
    write16(f, 2);                      /* block align */
    write16(f, 16);                     /* bits per sample */

    /* data chunk */
    fwrite("data", 1, 4, f);
    write32(f, data_size);
    fwrite(samples, 2, (size_t)num_samples, f);

    fclose(f);
    printf("Wrote %s (%d samples, %.1f seconds)\n", filename, num_samples,
           (float)num_samples / BIRB_SAMPLE_RATE);
}

/* ---------- test song ---------- */
/*
 * A short chiptune demo exercising:
 * - Ch1: pulse lead melody with arpeggio
 * - Ch2: triangle bass line
 * - Ch3: sawtooth pad / counter melody
 * - Ch4: noise drums (kick, snare, hi-hat)
 *
 * 4 patterns, 16 rows each, BPM 140
 */

static void build_test_song(birb_song *song) {
    memset(song, 0, sizeof(birb_song));

    song->bpm = 140;
    song->ticks_per_row = 6;
    song->num_patterns = 4;
    song->num_instruments = 6;
    song->order_length = 4;

    /* pattern lengths */
    for (int i = 0; i < 4; i++) song->pattern_lengths[i] = 16;

    /* ---- instruments ---- */

    /* 0: pulse lead (50% duty, snappy) */
    song->instruments[0].waveform = WAVE_PULSE;
    song->instruments[0].duty = DUTY_50;
    song->instruments[0].envelope = (birb_adsr){ .attack = 1, .decay = 6, .sustain = 160, .release = 8 };

    /* 1: triangle bass */
    song->instruments[1].waveform = WAVE_TRIANGLE;
    song->instruments[1].duty = 0;
    song->instruments[1].envelope = (birb_adsr){ .attack = 0, .decay = 4, .sustain = 200, .release = 4 };

    /* 2: sawtooth pad */
    song->instruments[2].waveform = WAVE_SAWTOOTH;
    song->instruments[2].duty = 0;
    song->instruments[2].envelope = (birb_adsr){ .attack = 8, .decay = 4, .sustain = 140, .release = 12 };

    /* 3: kick drum — sine with fast pitch drop */
    song->instruments[3].waveform = WAVE_SINE;
    song->instruments[3].duty = 0;
    song->instruments[3].envelope = (birb_adsr){ .attack = 0, .decay = 8, .sustain = 0, .release = 0 };
    song->instruments[3].pitch_env = -12;
    song->instruments[3].pitch_env_len = 6;

    /* 4: snare — noise burst */
    song->instruments[4].waveform = WAVE_NOISE;
    song->instruments[4].duty = 0;
    song->instruments[4].envelope = (birb_adsr){ .attack = 0, .decay = 5, .sustain = 0, .release = 0 };

    /* 5: hi-hat — short noise */
    song->instruments[5].waveform = WAVE_NOISE;
    song->instruments[5].duty = 0;
    song->instruments[5].envelope = (birb_adsr){ .attack = 0, .decay = 2, .sustain = 0, .release = 0 };

    /* helper: note value from octave + semitone */
    #define N(name, oct) (BIRB_NOTE_C0 + (oct) * 12 + (name))
    #define C  0
    #define CS 1
    #define D  2
    #define DS 3
    #define E  4
    #define F  5
    #define FS 6
    #define G  7
    #define GS 8
    #define A  9
    #define AS 10
    #define B  11
    #define OFF BIRB_NOTE_OFF
    #define __ BIRB_NOTE_EMPTY

    /* ---- order list ---- */
    /* pattern 0,1,2,3 on all channels */
    for (int i = 0; i < 4; i++) {
        song->order[i][0] = (uint8_t)i;  /* ch1: lead */
        song->order[i][1] = (uint8_t)i;  /* ch2: bass */
        song->order[i][2] = (uint8_t)i;  /* ch3: pad */
        song->order[i][3] = (uint8_t)i;  /* ch4: drums */
    }

    /* ---- pattern 0: intro ---- */
    /* Row format: { note, instrument, effect, param } */

    /* Ch1 — pulse lead melody */
    song->patterns[0][0][0]  = (birb_row){ N(C,4),  0, FX_NONE, 0 };
    song->patterns[0][2][0]  = (birb_row){ N(E,4),  0, FX_NONE, 0 };
    song->patterns[0][4][0]  = (birb_row){ N(G,4),  0, FX_NONE, 0 };
    song->patterns[0][6][0]  = (birb_row){ N(E,4),  0, FX_NONE, 0 };
    song->patterns[0][8][0]  = (birb_row){ N(A,4),  0, FX_NONE, 0 };
    song->patterns[0][10][0] = (birb_row){ N(G,4),  0, FX_NONE, 0 };
    song->patterns[0][12][0] = (birb_row){ N(E,4),  0, FX_NONE, 0 };
    song->patterns[0][14][0] = (birb_row){ OFF,     0, FX_NONE, 0 };

    /* Ch2 — triangle bass */
    song->patterns[0][0][1]  = (birb_row){ N(C,2),  1, FX_NONE, 0 };
    song->patterns[0][4][1]  = (birb_row){ N(C,2),  1, FX_NONE, 0 };
    song->patterns[0][8][1]  = (birb_row){ N(A,1),  1, FX_NONE, 0 };
    song->patterns[0][12][1] = (birb_row){ N(G,1),  1, FX_NONE, 0 };

    /* Ch3 — saw pad (long notes) */
    song->patterns[0][0][2]  = (birb_row){ N(E,3),  2, FX_NONE, 0 };
    song->patterns[0][8][2]  = (birb_row){ N(C,3),  2, FX_NONE, 0 };

    /* Ch4 — drums: kick on 0,4,8,12; snare on 4,12; hat on even rows */
    song->patterns[0][0][3]  = (birb_row){ N(C,3),  3, FX_NONE, 0 }; /* kick */
    song->patterns[0][2][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 }; /* hat */
    song->patterns[0][4][3]  = (birb_row){ N(D,4),  4, FX_NONE, 0 }; /* snare */
    song->patterns[0][6][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 }; /* hat */
    song->patterns[0][8][3]  = (birb_row){ N(C,3),  3, FX_NONE, 0 }; /* kick */
    song->patterns[0][10][3] = (birb_row){ N(A,5),  5, FX_NONE, 0 }; /* hat */
    song->patterns[0][12][3] = (birb_row){ N(D,4),  4, FX_NONE, 0 }; /* snare */
    song->patterns[0][14][3] = (birb_row){ N(A,5),  5, FX_NONE, 0 }; /* hat */

    /* ---- pattern 1: development ---- */

    /* Ch1 — arpeggio chords */
    song->patterns[1][0][0]  = (birb_row){ N(C,4),  0, FX_ARPEGGIO, 0x47 }; /* C major arp */
    song->patterns[1][4][0]  = (birb_row){ N(A,3),  0, FX_ARPEGGIO, 0x37 }; /* Am arp */
    song->patterns[1][8][0]  = (birb_row){ N(F,3),  0, FX_ARPEGGIO, 0x47 }; /* F major arp */
    song->patterns[1][12][0] = (birb_row){ N(G,3),  0, FX_ARPEGGIO, 0x47 }; /* G major arp */

    /* Ch2 — walking bass */
    song->patterns[1][0][1]  = (birb_row){ N(C,2),  1, FX_NONE, 0 };
    song->patterns[1][2][1]  = (birb_row){ N(E,2),  1, FX_NONE, 0 };
    song->patterns[1][4][1]  = (birb_row){ N(A,1),  1, FX_NONE, 0 };
    song->patterns[1][6][1]  = (birb_row){ N(C,2),  1, FX_NONE, 0 };
    song->patterns[1][8][1]  = (birb_row){ N(F,1),  1, FX_NONE, 0 };
    song->patterns[1][10][1] = (birb_row){ N(A,1),  1, FX_NONE, 0 };
    song->patterns[1][12][1] = (birb_row){ N(G,1),  1, FX_NONE, 0 };
    song->patterns[1][14][1] = (birb_row){ N(B,1),  1, FX_NONE, 0 };

    /* Ch3 — saw with vibrato */
    song->patterns[1][0][2]  = (birb_row){ N(G,3),  2, FX_VIBRATO, 0x34 };
    song->patterns[1][8][2]  = (birb_row){ N(F,3),  2, FX_VIBRATO, 0x34 };

    /* Ch4 — same drum pattern */
    song->patterns[1][0][3]  = (birb_row){ N(C,3),  3, FX_NONE, 0 };
    song->patterns[1][2][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[1][4][3]  = (birb_row){ N(D,4),  4, FX_NONE, 0 };
    song->patterns[1][6][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[1][8][3]  = (birb_row){ N(C,3),  3, FX_NONE, 0 };
    song->patterns[1][10][3] = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[1][12][3] = (birb_row){ N(D,4),  4, FX_NONE, 0 };
    song->patterns[1][14][3] = (birb_row){ N(A,5),  5, FX_NONE, 0 };

    /* ---- pattern 2: bridge ---- */

    /* Ch1 — descending melody with pitch slides */
    song->patterns[2][0][0]  = (birb_row){ N(A,4),  0, FX_NONE, 0 };
    song->patterns[2][2][0]  = (birb_row){ N(G,4),  0, FX_PITCH_DOWN, 2 };
    song->patterns[2][4][0]  = (birb_row){ N(F,4),  0, FX_NONE, 0 };
    song->patterns[2][6][0]  = (birb_row){ N(E,4),  0, FX_PITCH_DOWN, 2 };
    song->patterns[2][8][0]  = (birb_row){ N(D,4),  0, FX_NONE, 0 };
    song->patterns[2][10][0] = (birb_row){ N(C,4),  0, FX_NONE, 0 };
    song->patterns[2][12][0] = (birb_row){ N(D,4),  0, FX_PITCH_UP, 3 };
    song->patterns[2][14][0] = (birb_row){ OFF,     0, FX_NONE, 0 };

    /* Ch2 — bass holds */
    song->patterns[2][0][1]  = (birb_row){ N(F,1),  1, FX_NONE, 0 };
    song->patterns[2][8][1]  = (birb_row){ N(G,1),  1, FX_NONE, 0 };

    /* Ch3 — pad with duty sweep */
    song->patterns[2][0][2]  = (birb_row){ N(A,3),  2, FX_NONE, 0 };
    song->patterns[2][8][2]  = (birb_row){ N(G,3),  2, FX_NONE, 0 };

    /* Ch4 — busier drums */
    song->patterns[2][0][3]  = (birb_row){ N(C,3),  3, FX_NONE, 0 };
    song->patterns[2][1][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[2][2][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[2][3][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[2][4][3]  = (birb_row){ N(D,4),  4, FX_NONE, 0 };
    song->patterns[2][5][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[2][6][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[2][7][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[2][8][3]  = (birb_row){ N(C,3),  3, FX_NONE, 0 };
    song->patterns[2][9][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[2][10][3] = (birb_row){ N(D,4),  4, FX_NONE, 0 };
    song->patterns[2][11][3] = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[2][12][3] = (birb_row){ N(C,3),  3, FX_NONE, 0 };
    song->patterns[2][13][3] = (birb_row){ N(D,4),  4, FX_NONE, 0 };
    song->patterns[2][14][3] = (birb_row){ N(D,4),  4, FX_NONE, 0 };
    song->patterns[2][15][3] = (birb_row){ N(D,4),  4, FX_NONE, 0 };

    /* ---- pattern 3: finale (repeat of 0 with variation) ---- */

    /* Ch1 — higher octave melody */
    song->patterns[3][0][0]  = (birb_row){ N(C,5),  0, FX_NONE, 0 };
    song->patterns[3][2][0]  = (birb_row){ N(E,5),  0, FX_NONE, 0 };
    song->patterns[3][4][0]  = (birb_row){ N(G,5),  0, FX_NONE, 0 };
    song->patterns[3][6][0]  = (birb_row){ N(E,5),  0, FX_NONE, 0 };
    song->patterns[3][8][0]  = (birb_row){ N(C,5),  0, FX_ARPEGGIO, 0x47 };
    song->patterns[3][12][0] = (birb_row){ OFF,     0, FX_NONE, 0 };

    /* Ch2 — bass */
    song->patterns[3][0][1]  = (birb_row){ N(C,2),  1, FX_NONE, 0 };
    song->patterns[3][4][1]  = (birb_row){ N(E,2),  1, FX_NONE, 0 };
    song->patterns[3][8][1]  = (birb_row){ N(G,1),  1, FX_NONE, 0 };
    song->patterns[3][12][1] = (birb_row){ N(C,2),  1, FX_NONE, 0 };

    /* Ch3 — pad */
    song->patterns[3][0][2]  = (birb_row){ N(E,3),  2, FX_VIBRATO, 0x24 };
    song->patterns[3][8][2]  = (birb_row){ N(C,3),  2, FX_VIBRATO, 0x24 };

    /* Ch4 — drums */
    song->patterns[3][0][3]  = (birb_row){ N(C,3),  3, FX_NONE, 0 };
    song->patterns[3][2][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[3][4][3]  = (birb_row){ N(D,4),  4, FX_NONE, 0 };
    song->patterns[3][6][3]  = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[3][8][3]  = (birb_row){ N(C,3),  3, FX_NONE, 0 };
    song->patterns[3][10][3] = (birb_row){ N(A,5),  5, FX_NONE, 0 };
    song->patterns[3][12][3] = (birb_row){ N(D,4),  4, FX_NONE, 0 };
    song->patterns[3][14][3] = (birb_row){ N(A,5),  5, FX_NONE, 0 };

    #undef N
    #undef C
    #undef CS
    #undef D
    #undef DS
    #undef E
    #undef F
    #undef FS
    #undef G
    #undef GS
    #undef A
    #undef AS
    #undef B
    #undef OFF
    #undef __
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    const char *filename = "birb_test.wav";
    if (argc > 1) filename = argv[1];

    /* build test song */
    static birb_song song;
    build_test_song(&song);

    /* init player */
    static birb_state state;
    birb_init(&state, &song);

    /* render ~11 seconds (4 patterns at 140 BPM, 16 rows each, 6 ticks/row) */
    int duration_samples = BIRB_SAMPLE_RATE * 11;
    static int16_t buffer[BIRB_SAMPLE_RATE * 11];

    birb_render(&state, buffer, duration_samples);

    /* write WAV */
    write_wav(filename, buffer, duration_samples);

    return 0;
}
