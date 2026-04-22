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
 *   Optional SMPL section (IMA-ADPCM sample bank):
 *     'S' 'M' 'P' 'L'
 *     num_samples: u8
 *     Per sample:
 *       length: u16 (decoded sample count)
 *       loop_start: u16 (0xFFFF = no loop)
 *       loop_end: u16
 *       base_note: u8
 *       init_step_idx: u8 (0-88, high bit reserved)
 *       init_predictor: i16 (little-endian)
 *       data: u8[ceil(length/2)] (4-bit IMA-ADPCM packed, low nibble first)
 *
 *   Optional NAME section (instrument names):
 *     'N' 'A' 'M' 'E'
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
        inst->sample_idx = data[pos + 11];
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

    /* optional SMPL section — IMA-ADPCM sample bank */
    song->num_samples = 0;
    song->sample_pool_used = 0;
    if (pos + 4 <= len && data[pos] == 'S' && data[pos+1] == 'M' &&
        data[pos+2] == 'P' && data[pos+3] == 'L') {
        pos += 4;
        /* IMA-ADPCM step table (89 entries) */
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

        if (pos >= len) return 0;
        song->num_samples = data[pos++];
        if (song->num_samples > BIRB_MAX_SAMPLES) song->num_samples = BIRB_MAX_SAMPLES;

        for (int s = 0; s < song->num_samples; s++) {
            if (pos + 10 > len) { song->num_samples = s; break; }
            uint32_t length = data[pos] | (data[pos+1] << 8);
            uint32_t loop_start = data[pos+2] | (data[pos+3] << 8);
            uint32_t loop_end = data[pos+4] | (data[pos+5] << 8);
            uint8_t base_note = data[pos+6];
            /* data[pos+7] reserved */
            /* initial predictor + index (2 bytes) */
            int32_t predictor = (int16_t)(data[pos+8] | (data[pos+9] << 8));
            int step_idx = data[pos+7] & 0x7F;
            pos += 10;

            uint32_t nibble_bytes = (length + 1) / 2;
            if (pos + nibble_bytes > (uint32_t)len) { song->num_samples = s; break; }
            if (song->sample_pool_used + length > BIRB_SAMPLE_POOL) {
                song->num_samples = s; break;
            }

            birb_sample_meta *m = &song->samples[s];
            m->offset = song->sample_pool_used;
            m->length = length;
            m->loop_start = (loop_start == 0xFFFF) ? 0xFFFFFFFFu : loop_start;
            m->loop_end = loop_end;
            m->base_note = base_note;

            /* decode IMA-ADPCM into sample_pool */
            int16_t *out = &song->sample_pool[song->sample_pool_used];
            for (uint32_t i = 0; i < length; i++) {
                uint8_t byte = data[pos + (i >> 1)];
                uint8_t nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
                if (step_idx < 0) step_idx = 0;
                if (step_idx > 88) step_idx = 88;
                int step = adpcm_step[step_idx];
                int diff = step >> 3;
                if (nibble & 1) diff += step >> 2;
                if (nibble & 2) diff += step >> 1;
                if (nibble & 4) diff += step;
                if (nibble & 8) diff = -diff;
                predictor += diff;
                if (predictor > 32767) predictor = 32767;
                if (predictor < -32768) predictor = -32768;
                step_idx += adpcm_indexadj[nibble];
                out[i] = (int16_t)predictor;
            }
            pos += nibble_bytes;
            song->sample_pool_used += length;
        }
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
