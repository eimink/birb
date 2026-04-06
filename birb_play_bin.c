/*
 * birb_play_bin.c — load a .bin song file, render to WAV
 * Verifies the compile → binary → load → play round-trip.
 */
#include <stdio.h>
#include <stdlib.h>
#include "birb_synth.h"
#include "birb_format.h"

/* WAV writer (same as birb_wav.c) */
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
    fwrite("RIFF", 1, 4, f);
    write32(f, 36 + data_size);
    fwrite("WAVEfmt ", 1, 8, f);
    write32(f, 16);
    write16(f, 1); write16(f, 1);
    write32(f, BIRB_SAMPLE_RATE);
    write32(f, BIRB_SAMPLE_RATE * 2);
    write16(f, 2); write16(f, 16);
    fwrite("data", 1, 4, f);
    write32(f, data_size);
    fwrite(samples, 2, (size_t)num_samples, f);
    fclose(f);
    printf("Wrote %s (%d samples, %.1f seconds)\n", filename, num_samples,
           (float)num_samples / BIRB_SAMPLE_RATE);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s song.bin [output.wav]\n", argv[0]);
        return 1;
    }

    const char *bin_file = argv[1];
    const char *wav_file = (argc > 2) ? argv[2] : "output.wav";

    /* read binary file */
    FILE *f = fopen(bin_file, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open '%s'\n", bin_file); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    fread(data, 1, (size_t)size, f);
    fclose(f);
    printf("Loaded %s (%ld bytes)\n", bin_file, size);

    /* parse */
    static birb_song song;
    if (birb_load(&song, data, (int)size) < 0) {
        fprintf(stderr, "Error: invalid song data\n");
        free(data);
        return 1;
    }
    free(data);

    printf("Song: bpm=%d ticks=%d instruments=%d patterns=%d order=%d\n",
           song.bpm, song.ticks_per_row, song.num_instruments,
           song.num_patterns, song.order_length);

    /* render */
    static birb_state state;
    birb_init(&state, &song);

    int duration_samples = BIRB_SAMPLE_RATE * 11;
    int16_t *buffer = (int16_t *)malloc((size_t)duration_samples * 2);
    birb_render(&state, buffer, duration_samples);

    write_wav(wav_file, buffer, duration_samples);
    free(buffer);

    return 0;
}
