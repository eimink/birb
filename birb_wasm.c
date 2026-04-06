/*
 * birb_wasm.c — WASM entry points for the synth engine
 * Exports functions + buffer pointers for JS AudioWorklet.
 */
#include "birb_synth.h"
#include "birb_format.h"

#define OUTPUT_SAMPLES 128  /* AudioWorklet quantum */
#define SONG_BUF_SIZE  8192

static birb_song  g_song;
static birb_state g_state;
static int16_t    g_output[OUTPUT_SAMPLES];
static uint8_t    g_song_buf[SONG_BUF_SIZE];

#define EXPORT __attribute__((export_name(#name)))
/* clang wasm export attributes */

__attribute__((export_name("getSongBuf")))
unsigned int getSongBuf(void) {
    return (unsigned int)(void *)g_song_buf;
}

__attribute__((export_name("getOutputBuf")))
unsigned int getOutputBuf(void) {
    return (unsigned int)(void *)g_output;
}

__attribute__((export_name("init")))
int init(int song_size) {
    if (birb_load(&g_song, g_song_buf, song_size) < 0) return -1;
    birb_init(&g_state, &g_song);
    return 0;
}

__attribute__((export_name("render")))
void render(int num_samples) {
    if (num_samples > OUTPUT_SAMPLES) num_samples = OUTPUT_SAMPLES;
    birb_render(&g_state, g_output, num_samples);
}

__attribute__((export_name("getRow")))
int getRow(void) {
    return birb_get_row(&g_state);
}

__attribute__((export_name("getPattern")))
int getPattern(void) {
    return birb_get_pattern(&g_state);
}
