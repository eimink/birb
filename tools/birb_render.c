/* birb_render — render a .bsb through birb_synth.c to raw 16-bit mono PCM.
 *
 *   birb_render song.bsb out.raw [num_samples]
 *
 * birb_wav renders a song built in code; the pitch and parity harnesses need
 * to render an arbitrary file, and to get PCM without a WAV header in the way.
 * Sample count defaults to the whole song as the sequencer sees it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host-side harness: hold the widest song the format allows so any .bsb the
 * tracker can author will load. Verified byte-identical to a 4-channel build
 * on 4-channel songs — silent channels contribute nothing to the mix.
 *
 * birb_synth.c is #included rather than linked, deliberately. BIRB_NUM_CHANNELS
 * sizes birb_state.channels[], so every translation unit must agree on it. When
 * this file defined 16 and birb_synth.c was compiled separately at the default
 * 4, the two disagreed on the struct layout and the tool rendered pure silence
 * — no warning, no crash. Folding both into one translation unit makes the
 * define authoritative and removes the need for a -D on the command line. */
#define BIRB_NUM_CHANNELS 16

#include "birb_synth.h"
#include "birb_format.h"
#include "birb_synth.c"

#define CHUNK 4096

int main(int argc, char **argv) {
    /* --dump-notes prints the engine's own note lookup, so the report can
     * compare tuning tables exactly instead of inferring them from audio.
     * At C-0 a half-second window is only eight cycles, which is not enough
     * signal to certify a 0.1-cent table acoustically. */
    if (argc >= 2 && strcmp(argv[1], "--dump-notes") == 0) {
        for (int n = 0; n < 96; n++)
            printf("%d %ld\n", n, (long)birb_note_to_freq(n));
        return 0;
    }
    /* --dump-ks <buf_len> <damping> <n> runs the real KS loop in isolation and
     * writes its raw output. Measuring KS pitch from a rendered song does not
     * work: it is a decaying filtered noise burst through an envelope and a
     * limiter, and a generic pitch estimator reads it wrong by up to 177 cents.
     * The loop itself is exactly periodic, so measure that instead. */
    if (argc >= 3 && strcmp(argv[1], "--dump-ks") == 0) {
        /* Takes the Q32 increment, not a length, so it exercises exactly what
         * trigger_note computes — including the fractional part that the
         * allpass realizes. */
        uint32_t inc = (uint32_t)strtoul(argv[2], NULL, 10);
        int damping = argc > 3 ? atoi(argv[3]) : 0;
        long count = argc > 4 ? atol(argv[4]) : 200000;
        uint64_t lq = inc ? (((uint64_t)1 << 48) / inc) : 0;
        int len = (int)(lq >> 16);
        uint32_t lfrac = (uint32_t)(lq & 0xFFFFu);
        if (len < 4 || len > BIRB_KS_BUF_SIZE) {
            fprintf(stderr, "buf_len %d out of 4..%d\n", len, BIRB_KS_BUF_SIZE);
            return 1;
        }
        static birb_channel ch;
        for (size_t i = 0; i < sizeof ch; i++) ((uint8_t *)&ch)[i] = 0;
        ch.synth_type = SYNTH_KS;
        ch.u.ks.buf_len = (uint16_t)len;
        ch.u.ks.buf_pos = 0;
        ch.u.ks.loop_gain = birb_ks_loop_gain((uint8_t)damping, (uint16_t)len);
#ifndef BIRB_LEGACY_PITCH
        {
            int32_t d_q16 = (int32_t)lfrac + 32768;
            int64_t nu = (int64_t)(65536 - d_q16) << 16;
            ch.u.ks.ap_c = (int16_t)(nu / (65536 + d_q16));
        }
#endif
        uint16_t lfsr = 0x7FFF;
        for (int i = 0; i < len; i++) {
            uint16_t b = (lfsr ^ (lfsr >> 1)) & 1;
            lfsr = (uint16_t)(((lfsr >> 1) | (b << 14)) & 0xFFFF);
            ch.u.ks.buf[i] = (lfsr & 1) ? 32767 : -32767;
        }
        for (long i = 0; i < count; i++) {
            int16_t v = generate_ks(&ch);
            fwrite(&v, sizeof v, 1, stdout);
        }
        fprintf(stderr, "ks loop len=%d frac=%u c=%d gain=%u samples=%ld\n",
                len, lfrac, ch.u.ks.ap_c, ch.u.ks.loop_gain, count);
        return 0;
    }

    if (argc < 3) {
        fprintf(stderr, "usage: %s song.bsb out.raw [num_samples]\n", argv[0]);
        fprintf(stderr, "       %s --dump-notes\n", argv[0]);
        fprintf(stderr, "       %s --dump-ks <q32_increment> [damping] [n] > out.raw\n", argv[0]);
        return 2;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)len);
    if (!data || fread(data, 1, (size_t)len, in) != (size_t)len) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    fclose(in);

    /* The channel count lives in the high 3 bits of the ticks_per_row byte and
     * decodes as 4 + 2*code. birb_load refuses a song wider than the build, so
     * report both — "not a valid .bsb" is otherwise indistinguishable from a
     * corrupt file when the real problem is a 6-channel song meeting a
     * 4-channel binary. */
    int song_ch = len > 5 ? 4 + 2 * (data[5] >> 5) : 0;

    static birb_song song;
    if (birb_load(&song, data, (int)len) != 0) {
        if (song_ch > BIRB_NUM_CHANNELS)
            fprintf(stderr, "%s: song has %d channels, this build holds %d "
                            "(rebuild with -DBIRB_NUM_CHANNELS=%d)\n",
                    argv[1], song_ch, BIRB_NUM_CHANNELS, song_ch);
        else
            fprintf(stderr, "%s: not a valid .bsb\n", argv[1]);
        return 1;
    }
    free(data);

    /* Song length in samples: order_length rows-blocks × rows × ticks × the
     * sequencer's samples-per-tick. Mirrors birbc's own accounting. */
    long total;
    if (argc > 3) {
        total = atol(argv[3]);
    } else {
        int max_rows = 1;
        for (int p = 0; p < song.num_patterns; p++)
            if (song.pattern_lengths[p] > max_rows) max_rows = song.pattern_lengths[p];
        long spt = (long)BIRB_SAMPLE_RATE * 5 / ((song.bpm ? song.bpm : 125) * 2);
        total = (long)song.order_length * max_rows *
                (song.ticks_per_row ? song.ticks_per_row : 6) * spt;
    }
    if (total <= 0) { fprintf(stderr, "nothing to render\n"); return 1; }

    static birb_state state;
    birb_init(&state, &song);

    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }

    static int16_t buf[CHUNK];
    for (long done = 0; done < total; ) {
        int n = (int)(total - done < CHUNK ? total - done : CHUNK);
        birb_render(&state, buf, n);
        fwrite(buf, sizeof(int16_t), (size_t)n, out);
        done += n;
    }
    fclose(out);

    fprintf(stderr, "%s: %ld samples, %d channels\n", argv[2], total, song_ch);
    return 0;
}
