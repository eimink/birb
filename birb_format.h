/*
 * birb_format.h — binary song format spec + loader
 *
 * Binary layout (planar for Brotli):
 *
 *   Header (8 bytes):
 *     'B' 'R' 'B' '1'        magic + version
 *     bpm: u8
 *     ticks_per_row: u8
 *     num_instruments: u8
 *     num_patterns: u8
 *
 *   Order:
 *     order_length: u8
 *     [ch0_pat, ch1_pat, ch2_pat, ch3_pat] × order_length
 *
 *   Instruments (num_instruments × 12 bytes):
 *     waveform: u8
 *     duty: u8           (0=12.5%, 1=25%, 2=50%, 3=75%)
 *     attack: u8
 *     decay: u8
 *     sustain: u8
 *     release: u8
 *     pitch_env: i8
 *     pitch_env_len: u8
 *     arp_note1: u8
 *     arp_note2: u8
 *     reserved: u8, u8
 *
 *   Patterns:
 *     For each pattern:
 *       num_rows: u8
 *       Planar data (5 planes × 4 channels × num_rows):
 *         notes, insts, volumes, fx, params (each: ch0..ch3 × rows)
 *
 *   Instrument names (optional trailing section):
 *     'N' 'A' 'M' 'E'    section marker
 *     For each instrument: length-prefixed string (u8 len + chars, no null term)
 */
#ifndef BIRB_FORMAT_H
#define BIRB_FORMAT_H

#include "birb_synth.h"

#define BIRB_MAGIC_0 'B'
#define BIRB_MAGIC_1 'R'
#define BIRB_MAGIC_2 'B'
#define BIRB_MAGIC_3 '1'

#define BIRB_INST_SIZE 12

/* Duty encoding for binary format */
#define BIRB_DUTY_12  0
#define BIRB_DUTY_25  1
#define BIRB_DUTY_50  2
#define BIRB_DUTY_75  3

static fixed16 birb_duty_decode(uint8_t d) {
    switch (d) {
        case BIRB_DUTY_12: return DUTY_12;
        case BIRB_DUTY_25: return DUTY_25;
        case BIRB_DUTY_50: return DUTY_50;
        case BIRB_DUTY_75: return DUTY_75;
        default:           return DUTY_50;
    }
}

static uint8_t birb_duty_encode(fixed16 d) {
    if (d <= DUTY_12 + 1000) return BIRB_DUTY_12;
    if (d <= DUTY_25 + 1000) return BIRB_DUTY_25;
    if (d <= DUTY_50 + 1000) return BIRB_DUTY_50;
    return BIRB_DUTY_75;
}

/*
 * Load a binary song into a birb_song struct.
 * Returns 0 on success, -1 on error.
 * data/len is the raw binary blob.
 */
static int birb_load(birb_song *song, const uint8_t *data, int len) {
    if (len < 8) return -1;
    if (data[0] != BIRB_MAGIC_0 || data[1] != BIRB_MAGIC_1 ||
        data[2] != BIRB_MAGIC_2 || data[3] != BIRB_MAGIC_3) return -1;

    /* zero out */
    for (int i = 0; i < (int)sizeof(birb_song); i++)
        ((uint8_t *)song)[i] = 0;

    song->bpm = data[4];
    song->ticks_per_row = data[5];
    song->num_instruments = data[6];
    song->num_patterns = data[7];

    int pos = 8;

    /* order */
    if (pos >= len) return -1;
    song->order_length = data[pos++];
    for (int i = 0; i < song->order_length && i < BIRB_MAX_ORDER; i++) {
        if (pos + 4 > len) return -1;
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            song->order[i][c] = data[pos++];
    }

    /* instruments */
    for (int i = 0; i < song->num_instruments && i < BIRB_MAX_INSTRUMENTS; i++) {
        if (pos + BIRB_INST_SIZE > len) return -1;
        birb_instrument *inst = &song->instruments[i];
        inst->waveform = (birb_wave)data[pos];
        inst->duty = birb_duty_decode(data[pos + 1]);
        inst->envelope.attack = data[pos + 2];
        inst->envelope.decay = data[pos + 3];
        inst->envelope.sustain = data[pos + 4];
        inst->envelope.release = data[pos + 5];
        inst->pitch_env = (int8_t)data[pos + 6];
        inst->pitch_env_len = data[pos + 7];
        inst->arp_note1 = data[pos + 8];
        inst->arp_note2 = data[pos + 9];
        inst->volume = data[pos + 10];
        pos += BIRB_INST_SIZE;
    }

    /* patterns */
    for (int p = 0; p < song->num_patterns && p < BIRB_MAX_PATTERNS; p++) {
        if (pos >= len) return -1;
        int nrows = data[pos++];
        song->pattern_lengths[p] = (uint8_t)nrows;
        int plane_size = nrows * BIRB_NUM_CHANNELS;
        if (pos + plane_size * 5 > len) return -1;

        /* notes plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++)
                song->patterns[p][r][c].note = data[pos++];

        /* instrument plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++)
                song->patterns[p][r][c].instrument = data[pos++];

        /* volume plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++)
                song->patterns[p][r][c].volume = data[pos++];

        /* effect plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++)
                song->patterns[p][r][c].effect = data[pos++];

        /* param plane */
        for (int c = 0; c < BIRB_NUM_CHANNELS; c++)
            for (int r = 0; r < nrows; r++)
                song->patterns[p][r][c].param = data[pos++];
    }

    /* optional NAME section */
    if (pos + 4 <= len && data[pos] == 'N' && data[pos+1] == 'A' &&
        data[pos+2] == 'M' && data[pos+3] == 'E') {
        pos += 4;
        for (int i = 0; i < song->num_instruments && i < BIRB_MAX_INSTRUMENTS; i++) {
            if (pos >= len) break;
            int slen = data[pos++];
            if (slen > 31) slen = 31;
            if (pos + slen > len) break;
            for (int j = 0; j < slen; j++)
                song->instruments[i].name[j] = (char)data[pos++];
            song->instruments[i].name[slen] = '\0';
        }
    }

    return 0;
}

#endif /* BIRB_FORMAT_H */
