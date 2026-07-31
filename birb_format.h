/*
 * birb_format.h — binary song format spec + loader
 *
 * Binary layout (planar for Brotli):
 *
 *   Header (8 bytes):
 *     'B' 'R' 'B' '1'        magic + version
 *     bpm: u8
 *     ticks_per_row: u8
 *         Low 5 bits: ticks per row (1..31).
 *         High 3 bits: channel-count code — decoded as `4 + 2*code`
 *           (0→4, 1→6, 2→8, 3→10, 4→12, 5→14, 6→16, 7→reserved).
 *         Older files never set the high bits, so they decode as 4 channels.
 *     num_instruments: u8
 *     num_patterns: u8
 *
 *   Order:
 *     order_length: u8
 *     [ch0_pat … ch(N-1)_pat] × order_length, where N is the header's
 *     channel count. Loaders zero-pad any channels the build has beyond N.
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
 *     volume: u8         (per-instrument level, 0..255; 255 = full)
 *     sample_idx: u8     (index into the SMPL bank when waveform = WAVE_SAMPLE)
 *
 *   Pattern lengths:
 *     num_rows: u8 × num_patterns
 *
 *   Plane-empty flags: u8
 *     bit 0: notes plane is all-0 (i.e., no notes at all — unusual)
 *     bit 1: insts plane is all-0xFF (no instrument changes)
 *     bit 2: volumes plane is all-0 (no volume column used)
 *     bit 3: effects plane is all-0 (no effects)
 *     bit 4: params plane is all-0 (no effect params)
 *     Empty planes are omitted from the stream.
 *
 *   Plane data (present for each non-empty plane, in order 0..4):
 *     Organized as channel-major, pattern-major:
 *       For each channel 0..N-1 (N = header channel count):
 *         For each pattern 0..num_patterns-1:
 *           pattern_lengths[p] bytes of plane data
 *     This layout maximises LZ77 locality across pattern boundaries.
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
 *   Optional FM section (FM instrument params):
 *     'F' 'M' 'I' 'N'
 *     count: u8 (number of FM instruments)
 *     Per FM inst:
 *       inst_idx: u8          (which instrument slot — synth_type is set to SYNTH_FM)
 *       num_ops: u8           (2 or 4)
 *       algorithm: u8         (0-7, for 4-op)
 *       feedback: u8          (op0 feedback 0-255)
 *       mod_index: u8         (global mod scaling 0-255)
 *       reserved: u8          (alignment)
 *       per-op × num_ops (6 bytes each):
 *         ratio_i: u8
 *         ratio_f: u8         (fractional sixteenths: ratio = i + f/16)
 *         level: u8
 *         attack: u8
 *         decay: u8
 *         (sustain, release packed: high nibble | low nibble isn't worth it —
 *          just use 2 more bytes: sustain u8, release u8 → 8 bytes per op)
 *       Actual per-op size: 8 bytes.
 *     Loaders with -DBIRB_NO_FM skip the entire section cleanly.
 *
 *   Optional KSIN section (Karplus-Strong instrument params):
 *     'K' 'S' 'I' 'N'
 *     count: u8 (number of KS instruments)
 *     Per KS inst (2 bytes):
 *       inst_idx: u8  (loader sets this instrument's synth_type to SYNTH_KS)
 *       damping:  u8  (0-255; higher = shorter sustain)
 *     Loaders with -DBIRB_NO_KS skip the entire section cleanly.
 *
 *   Optional DRIN section (drum instrument params):
 *     'D' 'R' 'I' 'N'
 *     count: u8 (number of drum instruments)
 *     Per drum inst (6 bytes):
 *       inst_idx:   u8  (loader sets this instrument's synth_type to SYNTH_DRUM)
 *       drum_type:  u8  (0=KICK 1=SNARE 2=HAT 3=CLAP 4=TOM 5=CRASH; low 3 bits)
 *       drum_tune:  i8  (signed semitone offset)
 *       drum_decay: u8
 *       drum_tone:  u8
 *       drum_snap:  u8
 *     Loaders with -DBIRB_NO_DRUM skip the entire section cleanly.
 *
 *   Optional FRIN section (formant instrument params):
 *     'F' 'R' 'I' 'N'
 *     count: u8 (number of formant instruments)
 *     Per formant inst (79 bytes):
 *       inst_idx:     u8  (loader sets synth_type to SYNTH_FORMANT)
 *       source_wave:  u8  (WAVE_PULSE / WAVE_SAWTOOTH / WAVE_NOISE)
 *       duty:         u8  (duty code 0..3, relevant when source = pulse)
 *       vowel_a:      u8  (0=A 1=E 2=I 3=O 4=U)
 *       vowel_b:      u8
 *       sweep_speed:  u8  (0 = static vowel_a; >0 bounces A↔B)
 *       resonance:    u8  (0..255 → Q 2..32)
 *       coefficients: i32 × 18, little-endian — [vowel][formant][b0,a1,a2],
 *         baked by the compiler from the vowel pair and Q. The runtimes do no
 *         trig at all; see formant_coef in birb_synth.h for why.
 *     Loaders with -DBIRB_NO_FORMANT skip the entire section cleanly.
 *
 *   Optional NAME section (instrument names):
 *     'N' 'A' 'M' 'E'
 *     For each instrument: length-prefixed string (u8 len + chars, no null term)
 */
#ifndef BIRB_FORMAT_H
#define BIRB_FORMAT_H

/* FRIN record: 7 param bytes + 18 int32 baked biquad coefficients */
#define BIRB_FRIN_REC (7 + 18 * 4)

#include "birb_synth.h"

#define BIRB_MAGIC_0 'B'
#define BIRB_MAGIC_1 'R'
#define BIRB_MAGIC_2 'B'
#define BIRB_MAGIC_3 '1'

#define BIRB_INST_SIZE 12

/* Channel count encoding lives in the high 3 bits of the ticks_per_row byte.
 * Encoded value c yields (4 + 2*c) channels, so existing files with the high
 * bits clear decode as 4 channels. Code 7 is reserved. */
#define BIRB_TPR_MASK           0x1F
#define BIRB_CHANNELS_SHIFT     5
#define BIRB_CHANNELS_DECODE(b) (4 + 2 * (((b) >> BIRB_CHANNELS_SHIFT) & 0x7))
#define BIRB_CHANNELS_ENCODE(n) ((uint8_t)((((n) - 4) / 2) & 0x7))

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
    /* ticks_per_row byte carries the channel count in its high 3 bits. */
    uint8_t tpr_byte = data[5];
    song->ticks_per_row = tpr_byte & BIRB_TPR_MASK;
    int nch_song = BIRB_CHANNELS_DECODE(tpr_byte);
    if (nch_song > BIRB_NUM_CHANNELS) return -1; /* build can't hold this many */
    song->num_instruments = data[6];
    song->num_patterns = data[7];

    int pos = 8;

    /* order: read nch_song bytes per slot; zero-pad unused channels. */
    if (pos >= len) return -1;
    song->order_length = data[pos++];
    for (int i = 0; i < song->order_length && i < BIRB_MAX_ORDER; i++) {
        if (pos + nch_song > len) return -1;
        for (int c = 0; c < nch_song; c++)
            song->order[i][c] = data[pos++];
        for (int c = nch_song; c < BIRB_NUM_CHANNELS; c++)
            song->order[i][c] = 0xFF; /* no pattern */
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
        /* Derive synth_type from waveform (migration: old files store only
         * waveform, so WAVE_SAMPLE → SYNTH_SAMPLE, everything else → SYNTH_BASIC). */
        inst->synth_type = (inst->waveform == WAVE_SAMPLE) ? SYNTH_SAMPLE : SYNTH_BASIC;
        pos += BIRB_INST_SIZE;
    }

    /* Pattern lengths */
    int np = song->num_patterns;
    if (np > BIRB_MAX_PATTERNS) np = BIRB_MAX_PATTERNS;
    if (pos + np > len) return -1;
    for (int p = 0; p < np; p++)
        song->pattern_lengths[p] = data[pos++];

    /* Plane-empty flags */
    if (pos >= len) return -1;
    uint8_t plane_flags = data[pos++];

    /* Default values per plane: notes=0, insts=0xFF, vol=0, fx=0, prm=0 */
    static const uint8_t plane_defaults[5] = { 0, 0xFF, 0, 0, 0 };

    /* Initialize all pattern cells to defaults */
    for (int p = 0; p < np; p++) {
        int nrows = song->pattern_lengths[p];
        for (int r = 0; r < nrows; r++) {
            for (int c = 0; c < BIRB_NUM_CHANNELS; c++) {
                song->patterns[p][r][c].note = 0;
                song->patterns[p][r][c].instrument = 0xFF;
                song->patterns[p][r][c].volume = 0;
                song->patterns[p][r][c].effect = 0;
                song->patterns[p][r][c].param = 0;
            }
        }
    }

    /* For each plane, if not flagged empty, read channel-major pattern-major.
     * Stream only contains nch_song channels; the rest stay at their defaults. */
    for (int pl = 0; pl < 5; pl++) {
        if (plane_flags & (1 << pl)) continue; /* empty plane, use defaults */
        (void)plane_defaults;
        for (int c = 0; c < nch_song; c++) {
            for (int p = 0; p < np; p++) {
                int nrows = song->pattern_lengths[p];
                if (pos + nrows > len) return -1;
                for (int r = 0; r < nrows; r++) {
                    uint8_t v = data[pos++];
                    switch (pl) {
                        case 0: song->patterns[p][r][c].note = v; break;
                        case 1: song->patterns[p][r][c].instrument = v; break;
                        case 2: song->patterns[p][r][c].volume = v; break;
                        case 3: song->patterns[p][r][c].effect = v; break;
                        case 4: song->patterns[p][r][c].param = v; break;
                    }
                }
            }
        }
    }

    /* optional SMPL section — IMA-ADPCM sample bank */
    song->num_samples = 0;
#ifndef BIRB_NO_SAMPLES
    song->sample_pool_used = 0;
#endif
    if (pos + 4 <= len && data[pos] == 'S' && data[pos+1] == 'M' &&
        data[pos+2] == 'P' && data[pos+3] == 'L') {
        pos += 4;
#ifdef BIRB_NO_SAMPLES
        /* Skip SMPL section without decoding */
        if (pos < len) {
            int ns = data[pos++];
            for (int s = 0; s < ns; s++) {
                if (pos + 10 > len) break;
                uint32_t length = data[pos] | (data[pos+1] << 8);
                pos += 10;
                uint32_t nibble_bytes = (length + 1) / 2;
                if (pos + nibble_bytes > (uint32_t)len) break;
                pos += nibble_bytes;
            }
        }
#else
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
#endif /* BIRB_NO_SAMPLES */
    }

#ifndef BIRB_NO_FM
    /* optional FMIN section — FM instrument params.
     * Attached to existing instrument slots: each record starts with an
     * instrument index, and the loader promotes that instrument's
     * synth_type to SYNTH_FM. */
    if (pos + 4 <= len && data[pos] == 'F' && data[pos+1] == 'M' &&
        data[pos+2] == 'I' && data[pos+3] == 'N') {
        pos += 4;
        if (pos >= len) return -1;
        int fm_count = data[pos++];
        for (int f = 0; f < fm_count; f++) {
            if (pos + 6 > len) return -1;
            uint8_t idx = data[pos];
            uint8_t nops = data[pos+1];
            if (nops == 0) nops = 2;
            if (nops > 4) nops = 4;
            if (idx < BIRB_MAX_INSTRUMENTS) {
                birb_instrument *inst = &song->instruments[idx];
                inst->synth_type = SYNTH_FM;
                inst->fm.num_ops = nops;
                inst->fm.algorithm = data[pos+2];
                inst->fm.feedback = data[pos+3];
                inst->fm.mod_index = data[pos+4];
            }
            pos += 6;
            for (int o = 0; o < nops; o++) {
                if (pos + 8 > len) return -1;
                if (idx < BIRB_MAX_INSTRUMENTS) {
                    birb_instrument *inst = &song->instruments[idx];
                    inst->fm.ops[o].ratio_i = data[pos];
                    inst->fm.ops[o].ratio_f = data[pos+1];
                    inst->fm.ops[o].level   = data[pos+2];
                    inst->fm.ops[o].adsr.attack  = data[pos+3];
                    inst->fm.ops[o].adsr.decay   = data[pos+4];
                    inst->fm.ops[o].adsr.sustain = data[pos+5];
                    inst->fm.ops[o].adsr.release = data[pos+6];
                    /* data[pos+7] reserved */
                }
                pos += 8;
            }
        }
    }
#endif
    /* In BIRB_NO_FM builds, FMIN section (if any) is not parsed. The NAME
     * section parse below guards on the 'N' magic — if an FMIN section is
     * present ahead of NAME, NAME simply won't match and instrument names
     * are skipped. Songs authored without FM are unaffected. */

    /* optional KSIN section — Karplus-Strong instrument params.
     * Attached to existing instrument slots: the loader promotes that
     * instrument's synth_type to SYNTH_KS and records its damping. */
    if (pos + 4 <= len && data[pos] == 'K' && data[pos+1] == 'S' &&
        data[pos+2] == 'I' && data[pos+3] == 'N') {
        pos += 4;
#ifdef BIRB_NO_KS
        if (pos < len) {
            int ks_count = data[pos++];
            if (pos + 2 * ks_count > len) return -1;
            pos += 2 * ks_count; /* skip the whole section */
        }
#else
        if (pos >= len) return -1;
        int ks_count = data[pos++];
        for (int k = 0; k < ks_count; k++) {
            if (pos + 2 > len) return -1;
            uint8_t idx = data[pos];
            uint8_t damping = data[pos + 1];
            if (idx < BIRB_MAX_INSTRUMENTS) {
                birb_instrument *inst = &song->instruments[idx];
                inst->synth_type = SYNTH_KS;
                inst->ks_damping = damping;
            }
            pos += 2;
        }
#endif
    }

    /* optional DRIN section — drum instrument params. Attached to existing
     * instrument slots; loader promotes that slot's synth_type to SYNTH_DRUM. */
    if (pos + 4 <= len && data[pos] == 'D' && data[pos+1] == 'R' &&
        data[pos+2] == 'I' && data[pos+3] == 'N') {
        pos += 4;
#ifdef BIRB_NO_DRUM
        if (pos < len) {
            int dcount = data[pos++];
            if (pos + 6 * dcount > len) return -1;
            pos += 6 * dcount;
        }
#else
        if (pos >= len) return -1;
        int dcount = data[pos++];
        for (int k = 0; k < dcount; k++) {
            if (pos + 6 > len) return -1;
            uint8_t idx = data[pos];
            uint8_t dtype = data[pos + 1] & 0x07;
            int8_t tune = (int8_t)data[pos + 2];
            uint8_t decay = data[pos + 3];
            uint8_t tone = data[pos + 4];
            uint8_t snap = data[pos + 5];
            if (idx < BIRB_MAX_INSTRUMENTS) {
                birb_instrument *inst = &song->instruments[idx];
                inst->synth_type = SYNTH_DRUM;
                inst->drum_type = dtype;
                inst->drum_tune = tune;
                inst->drum_decay = decay;
                inst->drum_tone = tone;
                inst->drum_snap = snap;
            }
            pos += 6;
        }
#endif
    }

    /* optional FRIN section — formant instrument params. */
    if (pos + 4 <= len && data[pos] == 'F' && data[pos+1] == 'R' &&
        data[pos+2] == 'I' && data[pos+3] == 'N') {
        pos += 4;
#ifdef BIRB_NO_FORMANT
        if (pos < len) {
            int fcount = data[pos++];
            if (pos + BIRB_FRIN_REC * fcount > len) return -1;
            pos += BIRB_FRIN_REC * fcount;
        }
#else
        if (pos >= len) return -1;
        int fcount = data[pos++];
        for (int k = 0; k < fcount; k++) {
            if (pos + BIRB_FRIN_REC > len) return -1;
            uint8_t idx = data[pos];
            uint8_t sw  = data[pos + 1];
            uint8_t duty = data[pos + 2];
            uint8_t va  = data[pos + 3];
            uint8_t vb  = data[pos + 4];
            uint8_t sp  = data[pos + 5];
            uint8_t res = data[pos + 6];
            if (idx < BIRB_MAX_INSTRUMENTS) {
                birb_instrument *inst = &song->instruments[idx];
                inst->synth_type = SYNTH_FORMANT;
                inst->formant_source_wave = sw;
                inst->formant_duty = duty;
                inst->formant_vowel_a = va;
                inst->formant_vowel_b = vb;
                inst->formant_sweep_speed = sp;
                inst->formant_resonance = res;
            }
            /* 18 baked biquad coefficients, int32 LE: [vowel][formant][b0,a1,a2].
             * Computed by the compiler so no runtime trig is needed — see
             * formant_coef in birb_synth.h. */
            {
                int q = pos + 7;
                for (int v = 0; v < 2; v++)
                    for (int fi = 0; fi < 3; fi++)
                        for (int c = 0; c < 3; c++) {
                            int32_t val = (int32_t)((uint32_t)data[q]
                                        | ((uint32_t)data[q+1] << 8)
                                        | ((uint32_t)data[q+2] << 16)
                                        | ((uint32_t)data[q+3] << 24));
                            if (idx < BIRB_MAX_INSTRUMENTS)
                                song->instruments[idx].formant_coef[v][fi][c] = val;
                            q += 4;
                        }
            }
            pos += BIRB_FRIN_REC;
        }
#endif
    }

    /* optional REVB section — global reverb bus params + per-instrument sends.
     * 3 global bytes (size, damp, wet; each 0-255) then a count-prefixed
     * [inst_idx, send] table (same attach-by-index shape as KSIN). Absent
     * section → struct stays zeroed → reverb off (wet 0). */
    if (pos + 4 <= len && data[pos] == 'R' && data[pos+1] == 'E' &&
        data[pos+2] == 'V' && data[pos+3] == 'B') {
        pos += 4;
#ifdef BIRB_NO_REVERB
        if (pos + 3 > len) return -1;
        pos += 3;                        /* skip 3 global bytes */
        if (pos >= len) return -1;
        int rcount = data[pos++];
        if (pos + 2 * rcount > len) return -1;
        pos += 2 * rcount;               /* skip the [idx,send] table */
#else
        if (pos + 3 > len) return -1;
        song->rev_size = data[pos];
        song->rev_damp = data[pos + 1];
        song->rev_wet  = data[pos + 2];
        pos += 3;
        if (pos >= len) return -1;
        int rcount = data[pos++];
        for (int k = 0; k < rcount; k++) {
            if (pos + 2 > len) return -1;
            uint8_t idx = data[pos];
            uint8_t send = data[pos + 1];
            if (idx < BIRB_MAX_INSTRUMENTS)
                song->instruments[idx].reverb_send = send;
            pos += 2;
        }
#endif
    }

    /* optional MSTR section — master bus + per-instrument dynamics.
     * 4 global bytes (master gain, limiter threshold, limiter release ms,
     * sidechain release ms) then a count-prefixed [inst_idx, drive, duck_send,
     * duck_amt] table (same attach-by-index shape as KSIN/REVB). Absent section
     * leaves the struct zeroed; birb_init then fills in the defaults, so a song
     * without this section still gets the standard gain structure. */
    if (pos + 4 <= len && data[pos] == 'M' && data[pos+1] == 'S' &&
        data[pos+2] == 'T' && data[pos+3] == 'R') {
        pos += 4;
#ifdef BIRB_NO_MASTER
        if (pos + 4 > len) return -1;
        pos += 4;                        /* skip 4 global bytes */
        if (pos >= len) return -1;
        int mcount = data[pos++];
        if (pos + 4 * mcount > len) return -1;
        pos += 4 * mcount;               /* skip the per-instrument table */
#else
        if (pos + 4 > len) return -1;
        song->master_gain   = data[pos];
        song->limit_thresh  = data[pos + 1];
        song->limit_release = data[pos + 2];
        song->duck_release  = data[pos + 3];
        pos += 4;
        if (pos >= len) return -1;
        int mcount = data[pos++];
        for (int k = 0; k < mcount; k++) {
            if (pos + 4 > len) return -1;
            uint8_t idx = data[pos];
            if (idx < BIRB_MAX_INSTRUMENTS) {
                song->instruments[idx].drive     = data[pos + 1];
                song->instruments[idx].duck_send = data[pos + 2];
                song->instruments[idx].duck_amt  = data[pos + 3];
            }
            pos += 4;
        }
#endif
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
