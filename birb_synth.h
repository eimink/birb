/*
 * birb_synth.h — minimal chiptune synth engine
 * No stdlib, no malloc, no libm. Fixed-point 16.16 core; the FM feedback loop
 * and the reverb bus use floating point in the full engine (both strip-able).
 * Compiles to native + WASM.
 */
#ifndef BIRB_SYNTH_H
#define BIRB_SYNTH_H

/* Project version — single source of truth for the C tools' --version. */
#define BIRB_VERSION "4.1.0"

#ifdef __wasm__
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;
#else
#include <stdint.h>
#endif

/* ---------- configuration ---------- */

#ifndef BIRB_SAMPLE_RATE
#define BIRB_SAMPLE_RATE 44100
#endif

/* Channel count is compile-time configurable (min 4, max 16).
 * Runtime loader reads the per-song count from the flag byte and validates
 * it is <= BIRB_NUM_CHANNELS. Songs authored with fewer channels are padded
 * with empty order columns. Override via -DBIRB_NUM_CHANNELS=N. */
#ifndef BIRB_NUM_CHANNELS
#define BIRB_NUM_CHANNELS  4
#endif
#if BIRB_NUM_CHANNELS < 4 || BIRB_NUM_CHANNELS > 16
#error "BIRB_NUM_CHANNELS must be in 4..16"
#endif

#ifndef BIRB_MAX_PATTERNS
#define BIRB_MAX_PATTERNS  64
#endif
#ifndef BIRB_MAX_ROWS
#define BIRB_MAX_ROWS      64
#endif
#ifndef BIRB_MAX_INSTRUMENTS
#define BIRB_MAX_INSTRUMENTS 16
#endif
#ifndef BIRB_MAX_ORDER
#define BIRB_MAX_ORDER     128
#endif
#ifndef BIRB_MAX_SAMPLES
#define BIRB_MAX_SAMPLES   16
#endif
#ifndef BIRB_SAMPLE_POOL
#define BIRB_SAMPLE_POOL   (512 * 1024 / 2) /* 512KB / 2 bytes = int16 samples */
#endif

/* ---------- fixed-point 16.16 ---------- */

typedef int32_t fixed16;

#define FX_SHIFT   16
#define FX_ONE     (1 << FX_SHIFT)
#define FX_HALF    (1 << (FX_SHIFT - 1))
#define FX_MASK    (FX_ONE - 1)
#define FX_FROM_INT(x)  ((fixed16)((x) << FX_SHIFT))
#define FX_TO_INT(x)    ((x) >> FX_SHIFT)
#define FX_FRAC(x)      ((x) & FX_MASK)
#define FX_MUL(a, b)    ((fixed16)(((int64_t)(a) * (int64_t)(b)) >> FX_SHIFT))

/* float-to-fixed for compile-time constants only */
#define FX_FROM_FLOAT(f) ((fixed16)((f) * FX_ONE))

/* ---------- waveform types ---------- */

typedef enum {
    WAVE_PULSE,     /* variable duty cycle */
    WAVE_TRIANGLE,
    WAVE_SAWTOOTH,
    WAVE_NOISE,
    WAVE_SINE,
    WAVE_SAMPLE,    /* ADPCM sample playback */
    WAVE_COUNT
} birb_wave;

/* ---------- synth types ----------
 * Top-level dispatch in generate_sample(). SYNTH_BASIC and SYNTH_SAMPLE are
 * shipped; the remaining slots are reserved for future phases so the numeric
 * codes stay stable across the format. */
typedef enum {
    SYNTH_BASIC   = 0,   /* pulse/tri/saw/noise/sine (waveform field selects) */
    SYNTH_SAMPLE  = 1,   /* ADPCM sample playback */
    SYNTH_FM      = 2,   /* reserved (Phase 2) */
    SYNTH_KS      = 3,   /* reserved (Phase 3) */
    SYNTH_DRUM    = 4,   /* reserved (Phase 4) */
    SYNTH_FORMANT = 5    /* reserved (Phase 5) */
} birb_synth_type;

/* ---------- duty cycle presets ---------- */

typedef enum {
    DUTY_12 = FX_FROM_FLOAT(0.125),
    DUTY_25 = FX_FROM_FLOAT(0.25),
    DUTY_50 = FX_FROM_FLOAT(0.5),
    DUTY_75 = FX_FROM_FLOAT(0.75)
} birb_duty;

/* ---------- effect types ---------- */

typedef enum {
    FX_NONE = 0,
    FX_ARPEGGIO,       /* 1xy: cycle base, +x, +y semitones per tick */
    FX_PITCH_UP,       /* 2xx: slide pitch up, xx = speed */
    FX_PITCH_DOWN,     /* 3xx: slide pitch down, xx = speed */
    FX_VIBRATO,        /* 4xy: x = speed, y = depth */
    FX_TONE_PORTA,     /* 5xx: slide to target note at speed xx */
    FX_RETRIGGER,      /* 6xx: retrigger note every xx ticks */
    FX_EXTENDED,       /* 7xy: x=C note cut after y ticks, x=D note delay y ticks */
    FX_TREMOLO,        /* 8xy: x = speed, y = depth (volume LFO) */
    FX_SAMPLE_OFFSET,  /* 9xx: start sample at offset xx*256 samples */
    FX_UNUSED_A,
    FX_POS_JUMP,       /* Bxx: jump to order position xx */
    FX_UNUSED_C,
    FX_PAT_BREAK,      /* Dxx: advance to next order, start at row xx */
    FX_UNUSED_E,
    FX_SET_SPEED,      /* Fxx: xx<0x20 sets TPR, xx>=0x20 sets BPM */
    FX_COUNT
} birb_fx;

/* ---------- ADSR envelope ---------- */

typedef struct {
    uint8_t attack;    /* ticks to reach max volume */
    uint8_t decay;     /* ticks to reach sustain level */
    uint8_t sustain;   /* sustain volume level (0-255) */
    uint8_t release;   /* ticks to reach zero after note off */
} birb_adsr;

/* ---------- FM operator (per-op instrument params) ----------
 * Carrier is always op0; op1/2/3 are modulators. For 2-op, only op0 and op1
 * are used (op1 → op0, classic carrier + modulator). 4-op uses all four with
 * `algorithm` selecting the topology. */
#ifndef BIRB_NO_FM
typedef struct {
    uint8_t ratio_i;   /* integer part of frequency ratio (0..15) */
    uint8_t ratio_f;   /* fractional part in 1/16 units (so ratio = i + f/16) */
    uint8_t level;     /* operator output level 0-255 */
    birb_adsr adsr;    /* per-op envelope */
} birb_fm_op;

typedef struct {
    uint8_t    num_ops;     /* 2 or 4 */
    uint8_t    algorithm;   /* 0-7 topology for 4-op (unused for 2-op) */
    uint8_t    feedback;    /* op0 feedback 0-255 */
    uint8_t    mod_index;   /* global modulation index 0-255 (scales non-carrier ops) */
    birb_fm_op ops[4];
} birb_fm_inst;
#endif

/* ---------- instrument ---------- */

typedef struct {
    uint8_t   synth_type;    /* birb_synth_type — dispatch for generate_sample() */
    birb_wave waveform;      /* basic-synth waveform (ignored for other synth types) */
    fixed16   duty;          /* duty cycle for pulse wave */
    birb_adsr envelope;
    int8_t    pitch_env;     /* pitch change per tick (signed) */
    uint8_t   pitch_env_len; /* how many ticks pitch env lasts */
    uint8_t   arp_note1;     /* arpeggio semitone offset 1 */
    uint8_t   arp_note2;     /* arpeggio semitone offset 2 */
    uint8_t   volume;        /* instrument volume 0-255, scales envelope output */
    uint8_t   sample_idx;    /* index into song sample bank (when synth_type == SYNTH_SAMPLE) */
#ifndef BIRB_NO_REVERB
    uint8_t   reverb_send;   /* reverb send amount 0-255 (0 = fully dry) */
#endif
#ifndef BIRB_NO_FM
    birb_fm_inst fm;         /* FM params (only meaningful when synth_type == SYNTH_FM) */
#endif
#ifndef BIRB_NO_KS
    uint8_t   ks_damping;    /* KS pluck damping 0-255; higher = shorter sustain.
                              * Maps to T60 4.0 s .. 0.02 s exponentially and is
                              * pitch-compensated (see birb_ks_q24). */
#endif
#ifndef BIRB_NO_MASTER
    /* Per-voice dynamics. `drive` is level-matched soft saturation applied
     * before the mixer: it raises a voice's RMS without raising its peak, which
     * is the only way a high-crest-factor source (a KS pluck measures ~19 dB
     * crest, an FM sine ~3 dB) can hold its own against a sustained bass.
     * `duck_send` feeds this instrument into the sidechain bus; `duck_amt` is
     * how much this instrument gets ducked BY that bus. */
    uint8_t   drive;         /* 0 = clean, 255 = heavily saturated */
    uint8_t   duck_send;     /* 0-255, how much this voice drives the duck bus */
    uint8_t   duck_amt;      /* 0-255, how much this voice is ducked */
#endif
#ifndef BIRB_NO_DRUM
    /* Drum synth params (only meaningful when synth_type == SYNTH_DRUM). */
    uint8_t   drum_type;     /* 0=KICK, 1=SNARE, 2=HAT, 3=CLAP, 4=TOM, 5=CRASH */
    int8_t    drum_tune;     /* signed pitch offset (−128..127) */
    uint8_t   drum_decay;    /* 0-255 */
    uint8_t   drum_tone;     /* 0-255 */
    uint8_t   drum_snap;     /* 0-255 */
#endif
#ifndef BIRB_NO_FORMANT
    /* Formant filter params (only meaningful when synth_type == SYNTH_FORMANT).
     * Source waveform is the subset {PULSE, SAW, NOISE} run through three
     * resonant bandpass biquads at vowel formant frequencies. */
    uint8_t   formant_source_wave;  /* WAVE_PULSE / WAVE_SAWTOOTH / WAVE_NOISE */
    uint8_t   formant_duty;         /* pulse duty (0..3 code) if source is pulse */
    uint8_t   formant_vowel_a;      /* 0=A 1=E 2=I 3=O 4=U */
    uint8_t   formant_vowel_b;      /* sweep destination */
    uint8_t   formant_sweep_speed;  /* 0=static vowel_a, >0 sweeps A→B→A */
    uint8_t   formant_resonance;    /* 0-255 reserved; current table uses Q=8 */
    /* Biquad coefficients for vowel A and vowel B, baked at compile time by
     * birbc (or by the editor on export) rather than derived here. The runtime
     * therefore needs no sin/cos at all: the C engine used a fixed-point
     * approximation with a documented 1.7% error in omega while the JS engines
     * used exact Math.sin, so the same instrument produced measurably different
     * filters in each. Baking removes the trig AND makes them agree, because
     * both read the same numbers. [vowel][formant][b0,a1,a2]. */
    fixed16   formant_coef[2][3][3];
#endif
    char      name[32];      /* instrument name (editor only, not in core binary) */
} birb_instrument;

/* ---------- pattern row ---------- */

typedef struct {
    uint8_t note;       /* 0=empty, 1=note-off, 2..97 = C0..B7 */
    uint8_t instrument; /* instrument index (0-15), 0xFF = no change */
    uint8_t volume;     /* row volume: 0 = no change, 1-255 = volume level */
    uint8_t effect;     /* effect type (birb_fx) */
    uint8_t param;      /* effect parameter */
} birb_row;

#define BIRB_VOL_NONE 0

/* note encoding */
#define BIRB_NOTE_EMPTY   0
#define BIRB_NOTE_OFF     1
#define BIRB_NOTE_C0      2   /* note value = BIRB_NOTE_C0 + semitone (0..95) */

/* ---------- channel state ---------- */

typedef enum {
    ENV_OFF,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} birb_env_stage;

typedef struct {
    /* ---- common state (shared by all synth types) ---- */

    /* synth dispatch */
    uint8_t       synth_type;  /* birb_synth_type — selects the union arm */

    /* oscillator */
    fixed16       phase;       /* phase accumulator 0..FX_ONE */
    fixed16       freq;        /* phase increment per sample */
    fixed16       base_freq;   /* freq before effects */
    fixed16       base_duty;   /* duty before effects (basic + formant) */

    /* envelope */
    birb_env_stage env_stage;
    fixed16       env_level;   /* current volume 0..FX_ONE */
    birb_adsr     adsr;

    /* effects */
    uint8_t       cur_instrument;
    int8_t        pitch_env;
    uint8_t       pitch_env_ticks;
    uint8_t       arp_tick;
    uint8_t       arp_note1;
    uint8_t       arp_note2;
    uint8_t       base_note;   /* note without arpeggio */
    fixed16       vibrato_phase;
    fixed16       vibrato_speed;
    fixed16       vibrato_depth;
    fixed16       tremolo_phase;
    fixed16       tremolo_speed;
    fixed16       tremolo_depth;
    fixed16       tremolo_mod;    /* cached modulation value per tick */
    fixed16       pitch_slide;
    fixed16       duty_sweep;
    uint8_t       volume;      /* instrument volume 0-255 */
    uint8_t       row_vol;     /* per-row volume override 0-255 */
    fixed16       porta_target; /* tone portamento target freq */
    fixed16       porta_speed;  /* tone portamento speed */
    uint8_t       retrig_interval; /* retrigger every N ticks */
    uint8_t       note_cut_tick;   /* cut note after N ticks, 0=off */
    uint8_t       note_delay_tick; /* delay note trigger by N ticks */
    uint8_t       delayed_note;    /* note to trigger after delay */
    uint8_t       delayed_inst;    /* instrument for delayed note */
#ifndef BIRB_NO_REVERB
    uint8_t       reverb_send;     /* per-channel reverb send 0-255, copied from instrument */
#endif
#ifndef BIRB_NO_MASTER
    uint8_t       duck_send;       /* copied from instrument at trigger */
    uint8_t       duck_amt;
    /* Drive resolved at trigger time: pre is the saturator input gain, norm
     * rescales the output so peak level is unchanged and only RMS rises. */
    float         drive_pre;
    float         drive_norm;
#endif

    /* ---- type-specific state (tagged by synth_type) ---- */
    union {
        /* SYNTH_BASIC: pulse / triangle / sawtooth / noise / sine */
        struct {
            birb_wave waveform;
            fixed16   duty;        /* current duty cycle */
            uint16_t  lfsr;
            uint16_t  lfsr_period; /* samples between shifts */
            uint16_t  lfsr_count;
        } basic;
        /* SYNTH_SAMPLE: IMA-ADPCM sample playback */
        struct {
            uint32_t  sample_pos;    /* 16.16 fixed-point position */
            uint32_t  sample_speed;  /* 16.16 fixed-point playback rate */
            uint8_t   sample_idx;    /* which sample is playing */
            uint8_t   sample_active; /* 1 if sample currently playing */
        } sample;
#ifndef BIRB_NO_FM
        /* SYNTH_FM: 2-op (default) or 4-op FM synthesis. Carrier is op0;
         * op1..op3 are modulators. */
        struct {
            fixed16        op_phase[4]; /* per-op phase accumulator 0..FX_ONE */
            fixed16        op_freq[4];  /* per-op phase increment */
            double         op_env[4];   /* per-op envelope level 0..FX_ONE. double so the
                                         * A/D/R ramps accumulate like the editor's float64
                                         * (F/(a+1), not floored) — a floored integer ramp,
                                         * and even float32, drifted op levels enough to
                                         * desync high-feedback FM on the lowest notes. */
            fixed16        op_lvl[4];   /* per-op static level 0..FX_ONE (live-refreshed) */
            birb_env_stage op_stage[4]; /* per-op ADSR stage */
            double         prev_out;    /* op0 feedback memory: raw carrier sine ×FX_ONE.
                                         * double (not fixed16) so the FM feedback loop keeps
                                         * full precision and matches the editor's float64
                                         * math — fixed-point truncation drove a Nyquist
                                         * limit-cycle, and float32 still diverged on
                                         * high-feedback braaams, that desynced the timbre. */
            uint8_t        num_ops;     /* 2 or 4 */
            uint8_t        algorithm;   /* 0-7 (4-op only) */
            uint8_t        feedback;    /* op0 feedback 0-255 */
            uint8_t        mod_index;   /* global mod scaling 0-255 */
        } fm;
#endif
#ifndef BIRB_NO_KS
/* Delay-buffer length for Karplus-Strong. Default 1024 samples covers pitches
 * down to ~43 Hz at 44100 Hz; 4K WASM builds override this with 256 to shrink
 * the channel union (512 B/channel vs 2048 B/channel). Higher notes clamp the
 * used portion of the buffer anyway, so a smaller ceiling only limits the
 * lowest usable note. */
#ifndef BIRB_KS_BUF_SIZE
#define BIRB_KS_BUF_SIZE 1024
#endif
        /* SYNTH_KS: Karplus-Strong plucked-string synthesis. On trigger the
         * buffer is pre-filled with noise and read back with a low-pass +
         * damping filter, producing a decaying pluck. */
        struct {
            int16_t  buf[BIRB_KS_BUF_SIZE];  /* delay line, noise-filled on trigger */
            uint16_t buf_len;                /* active length, clamped to BIRB_KS_BUF_SIZE */
            uint16_t buf_pos;                /* read/write head */
            /* Per-period loop gain in 1/65536, derived at trigger time from the
             * instrument's damping byte AND buf_len (see birb_ks_loop_gain).
             * Storing the resolved gain rather than the raw damping byte is what
             * makes decay time pitch-independent: a long buffer needs a higher
             * per-period gain to reach the same T60 in seconds. */
            uint16_t loop_gain;
        } ks;
#endif
#ifndef BIRB_NO_DRUM
        /* SYNTH_DRUM: six algorithmic drums sharing a biquad + exp LUT. The
         * drum_type byte dispatches to KICK/SNARE/HAT/CLAP inline generators.
         * TOM reuses KICK, CRASH reuses HAT — differences baked in at trigger. */
        struct {
            uint8_t  drum_type;        /* 0..5 */
            uint8_t  stage;            /* multi-phase envelope stage (CLAP) */
            uint16_t stage_tick;       /* sample counter within stage (>255 for long bursts) */
            uint8_t  ttl_hi;           /* high byte of samples-until-end */
            uint8_t  _pad_drum;        /* keep 32-bit alignment for phase2 */
            fixed16  phase2;           /* second oscillator phase (body / mod) */
            fixed16  pitch_env;        /* current pitch envelope value */
            fixed16  pitch_env_target; /* target pitch for exp decay */
            int32_t  bq_z1[2];         /* two biquads' state */
            int32_t  bq_z2[2];
            uint16_t noise_lfsr;
            uint16_t ttl_lo;           /* remaining-samples low word (see ttl_hi) */
        } drum;
#endif
#ifndef BIRB_NO_FORMANT
        /* SYNTH_FORMANT: source oscillator (pulse/saw/noise) into three
         * parallel bandpass biquads at vowel formant frequencies. Coefficients
         * are kept per-channel so sweep between vowels can linearly interpolate
         * without hitting the vowel table every sample. */
        struct {
            uint16_t src_lfsr;         /* noise source (when src is WAVE_NOISE) */
            uint8_t  src_wave;         /* WAVE_PULSE / WAVE_SAWTOOTH / WAVE_NOISE */
            uint8_t  sweep_pos;        /* 0..255 interp A→B */
            int8_t   sweep_dir;        /* +1 / -1 */
            uint8_t  sweep_speed;      /* >0 sweeps, 0 = static vowel_a */
            uint8_t  vowel_a;
            uint8_t  vowel_b;
            uint8_t  recalc;           /* sample counter for re-interp */
            uint8_t  resonance;        /* 0..255 → Q 2..32 (cached from instrument) */
            /* Per-formant biquad state + current interpolated coefficients.
             * b1 = 0 and b2 = -b0 for BPF, so we store only b0, a1, a2. */
            int32_t  bq_z1[3];
            int32_t  bq_z2[3];
            fixed16  bq_b0[3];
            fixed16  bq_a1[3];
            fixed16  bq_a2[3];
        } formant;
#endif
    } u;
} birb_channel;

/* ---- compile-time size lock ----
 * Refuse to link if the channel struct grows unexpectedly. The value is
 * platform-dependent (enum/alignment), so track both a 64-bit macOS value
 * and a 32-bit WASM value. Update these when intentionally adding fields.
 * With KS enabled, the KS delay buffer dominates the union: 2*BIRB_KS_BUF_SIZE
 * plus ~6 bytes of housekeeping. Without KS, FM 2-op is the ceiling (~80 B).
 * Basic/sample remain the smaller arms. */
#ifndef BIRB_NO_KS
#if defined(__wasm__)
_Static_assert(sizeof(birb_channel) <= 2 * BIRB_KS_BUF_SIZE + 128, "birb_channel bloat (wasm, KS)");
#else
_Static_assert(sizeof(birb_channel) <= 2 * BIRB_KS_BUF_SIZE + 144, "birb_channel bloat (native, KS)");
#endif
#elif !defined(BIRB_NO_FM)
#if defined(__wasm__)
_Static_assert(sizeof(birb_channel) <= 224, "birb_channel bloat (wasm, FM)");
#else
_Static_assert(sizeof(birb_channel) <= 240, "birb_channel bloat (native, FM)");
#endif
#elif !defined(BIRB_NO_FORMANT)
/* FORMANT arm (no KS/FM) — ~88 B: 3 biquads × (2 state + 3 coeff) × 4B + housekeeping. */
#if defined(__wasm__)
_Static_assert(sizeof(birb_channel) <= 192, "birb_channel bloat (wasm, formant)");
#else
_Static_assert(sizeof(birb_channel) <= 208, "birb_channel bloat (native, formant)");
#endif
#elif !defined(BIRB_NO_DRUM)
/* DRUM arm alone (no FM / no KS / no formant) is ~40 B. */
#if defined(__wasm__)
_Static_assert(sizeof(birb_channel) <= 144, "birb_channel bloat (wasm, drum-only)");
#else
_Static_assert(sizeof(birb_channel) <= 160, "birb_channel bloat (native, drum-only)");
#endif
#else
#if defined(__wasm__)
_Static_assert(sizeof(birb_channel) <= 112, "birb_channel bloat (wasm)");
#else
_Static_assert(sizeof(birb_channel) <= 128, "birb_channel bloat (native)");
#endif
#endif

/* ---------- sample metadata ---------- */

typedef struct {
    uint32_t offset;      /* offset into sample_pool */
    uint32_t length;      /* number of samples */
    uint32_t loop_start;  /* 0xFFFFFFFF = no loop */
    uint32_t loop_end;
    uint8_t  base_note;   /* MIDI-ish note for unity pitch */
} birb_sample_meta;

/* ---------- song data (in-memory, parsed) ---------- */

typedef struct {
    uint8_t  bpm;
    uint8_t  ticks_per_row;
    uint8_t  num_patterns;
    uint8_t  num_instruments;
    uint8_t  order_length;
    uint8_t  num_samples;
#ifndef BIRB_NO_REVERB
    uint8_t  rev_size;   /* global reverb bus: tail length   0-255 */
    uint8_t  rev_damp;   /* global reverb bus: HF damping     0-255 */
    uint8_t  rev_wet;    /* global reverb bus: wet mix        0-255 (0 = off) */
#endif
#ifndef BIRB_NO_MASTER
    /* Master bus (see the MSTR section in birb_format.h). Defaults are applied
     * by birb_parse_song when the section is absent, so every song gets the
     * same gain structure whether or not it carries master data. */
    uint8_t  master_gain;   /* 0-255, gain = v/64 (so 64 = unity, 255 = 4x) */
    uint8_t  limit_thresh;  /* 0-255, limiter ceiling = v/255 (default 242) */
    uint8_t  limit_release; /* 0-255 ms release time      (default 50) */
    uint8_t  duck_release;  /* 0-255 ms sidechain release (default 120) */
#endif

    birb_instrument instruments[BIRB_MAX_INSTRUMENTS];
    uint8_t         order[BIRB_MAX_ORDER][BIRB_NUM_CHANNELS]; /* pattern index per channel per position */
    birb_row        patterns[BIRB_MAX_PATTERNS][BIRB_MAX_ROWS][BIRB_NUM_CHANNELS];
    uint8_t         pattern_lengths[BIRB_MAX_PATTERNS]; /* rows per pattern (default 64) */

#ifndef BIRB_NO_SAMPLES
    birb_sample_meta samples[BIRB_MAX_SAMPLES];
    int16_t         sample_pool[BIRB_SAMPLE_POOL];
    uint32_t        sample_pool_used;
#endif
} birb_song;

/* ---------- reverb send bus (mono Schroeder-lite: 4 damped combs + 2 allpass) ----------
 * Matches the web editor's makeReverb() exactly: same delay lengths, same
 * normalized-by-(1-fb) level so Size sets decay only, same 5.5 makeup, same
 * tanh master saturation. Float math in the ±1 domain for bit-parity with JS. */
#ifndef BIRB_NO_REVERB
#define BIRB_REV_NCOMB 4
#define BIRB_REV_NAP   2
#define BIRB_REV_CMAX  1356   /* longest comb; all comb lines allocated at this size */
#define BIRB_REV_AMAX  556    /* longest allpass */
#endif

/* ---------- player state ---------- */

typedef struct {
    birb_song    *song;
    birb_channel  channels[BIRB_NUM_CHANNELS];

#ifndef BIRB_NO_REVERB
    /* reverb send bus state (see comment above); buffers are the dominant
     * cost (~26 KB) but this engine is not size-constrained. */
    float rev_comb[BIRB_REV_NCOMB][BIRB_REV_CMAX];
    float rev_comb_lp[BIRB_REV_NCOMB];   /* per-comb damping lowpass memory */
    int   rev_comb_pos[BIRB_REV_NCOMB];
    float rev_ap[BIRB_REV_NAP][BIRB_REV_AMAX];
    int   rev_ap_pos[BIRB_REV_NAP];
#endif

#ifndef BIRB_NO_MASTER
    /* Master bus state. lim_env is a peak follower with instantaneous attack
     * and a one-pole release — a feedback limiter, so no lookahead buffer is
     * needed and the soft-saturator downstream absorbs the small overshoot.
     * duck_env is the sidechain follower; it is applied with a one-sample
     * delay so the whole mix still runs in a single pass. */
    float lim_env;
    float duck_env;
#endif

    /* sequencer */
    int           order_pos;     /* current position in order list */
    int           current_row;
    int           current_tick;
    int           samples_per_tick;
    int           tick_counter;  /* samples until next tick */

    /* for external sync */
    int           row_out;
    int           pattern_out;

    /* pending jumps from effects (applied at next row boundary) */
    int           jump_order;    /* -1 = none, else target order pos */
    int           jump_row;      /* row to start at in next order pos */
} birb_state;

/* ---------- public API ---------- */

void birb_init(birb_state *state, birb_song *song);
void birb_render(birb_state *state, int16_t *output, int num_samples);
int  birb_get_row(birb_state *state);
int  birb_get_pattern(birb_state *state);

/* ---------- note frequency lookup ---------- *
 * One canonical 12-entry octave-base table + octave shift, identical to the
 * editor and 4K players so all engines are bit-for-bit in tune. See the long
 * note in birb_synth.c — do not add a per-build note table. */
fixed16 birb_note_to_freq(int note);

/* sine approximation for vibrato */
fixed16 birb_sin_approx(fixed16 phase);

#endif /* BIRB_SYNTH_H */
