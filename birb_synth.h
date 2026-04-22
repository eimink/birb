/*
 * birb_synth.h — minimal chiptune synth engine
 * No stdlib, no malloc, no floats. Compiles to native + WASM.
 */
#ifndef BIRB_SYNTH_H
#define BIRB_SYNTH_H

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

#define BIRB_NUM_CHANNELS  4
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

/* ---------- instrument ---------- */

typedef struct {
    birb_wave waveform;
    fixed16   duty;          /* duty cycle for pulse wave */
    birb_adsr envelope;
    int8_t    pitch_env;     /* pitch change per tick (signed) */
    uint8_t   pitch_env_len; /* how many ticks pitch env lasts */
    uint8_t   arp_note1;     /* arpeggio semitone offset 1 */
    uint8_t   arp_note2;     /* arpeggio semitone offset 2 */
    uint8_t   volume;        /* instrument volume 0-255, scales envelope output */
    uint8_t   sample_idx;    /* index into song sample bank (when waveform == WAVE_SAMPLE) */
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
    /* oscillator */
    fixed16       phase;       /* phase accumulator 0..FX_ONE */
    fixed16       freq;        /* phase increment per sample */
    fixed16       base_freq;   /* freq before effects */
    birb_wave     waveform;
    fixed16       duty;        /* current duty cycle */
    fixed16       base_duty;   /* duty before effects */

    /* noise LFSR */
    uint16_t      lfsr;
    uint16_t      lfsr_period; /* samples between shifts */
    uint16_t      lfsr_count;

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
    /* sample playback (WAVE_SAMPLE) */
    uint32_t      sample_pos;      /* 16.16 fixed-point position in sample buffer */
    uint32_t      sample_speed;    /* 16.16 fixed-point playback rate */
    uint8_t       sample_idx;      /* which sample is playing */
    uint8_t       sample_active;   /* 1 if sample currently playing */
} birb_channel;

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

    birb_instrument instruments[BIRB_MAX_INSTRUMENTS];
    uint8_t         order[BIRB_MAX_ORDER][BIRB_NUM_CHANNELS]; /* pattern index per channel per position */
    birb_row        patterns[BIRB_MAX_PATTERNS][BIRB_MAX_ROWS][BIRB_NUM_CHANNELS];
    uint8_t         pattern_lengths[BIRB_MAX_PATTERNS]; /* rows per pattern (default 64) */

    birb_sample_meta samples[BIRB_MAX_SAMPLES];
    int16_t         sample_pool[BIRB_SAMPLE_POOL];
    uint32_t        sample_pool_used;
} birb_song;

/* ---------- player state ---------- */

typedef struct {
    birb_song    *song;
    birb_channel  channels[BIRB_NUM_CHANNELS];

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

/* ---------- note frequency table (defined in birb_synth.c) ---------- */

extern const fixed16 birb_note_freq[96];

/* sine approximation for vibrato */
fixed16 birb_sin_approx(fixed16 phase);

#endif /* BIRB_SYNTH_H */
