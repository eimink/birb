/*
 * birb_4k.c — ultra-compact synth for 4K web demos
 * Single file, everything inlined, no external deps.
 * Target: <1KB WASM after Brotli, including song data.
 */

/* --- types --- */
typedef unsigned char u8;
typedef signed char i8;
typedef short i16;
typedef unsigned short u16;
typedef int i32;
typedef unsigned int u32;
typedef long long i64;

#define SR 44100
#define NCH 4
#define FX1 65536  /* 1.0 in 16.16 fixed */
#define FXH 32768  /* 0.5 */

/* --- song limits (override with -D) --- */
#ifndef MAXPAT
#define MAXPAT 16
#endif
#ifndef MAXROW
#define MAXROW 32
#endif
#ifndef MAXINST
#define MAXINST 8
#endif
#ifndef MAXORD
#define MAXORD 32
#endif

/* --- data layout (binary, matches birb_format) --- */
/* The song data is uploaded by JS into song_buf, then parsed by init(). */

static u8 song_buf[4096];
static i16 out_buf[128];

/* --- instrument --- */
typedef struct {
    u8 wave, duty, a, d, s, r;
    i8 pe; u8 pe_len;
    u8 arp1, arp2;
} Inst;

/* --- channel --- */
typedef struct {
    i32 phase, freq, base_freq;
    u8 wave, base_note;
    i32 duty, env;
    u8 env_st; /* 0=off 1=A 2=D 3=S 4=R */
    u8 a, d, s, r;
    i8 pe; u8 pe_t;
    u8 arp1, arp2, arp_t;
    i32 slide;
    u16 lfsr, lfsr_p, lfsr_c;
    u8 ci; /* current instrument */
    u8 rv; /* row volume */
    i8 vs; /* volume slide per tick */
} Ch;

/* --- state --- */
static Inst inst[MAXINST];
static u8 order[MAXORD][NCH];
static u8 pat_note[MAXPAT][MAXROW][NCH];
static u8 pat_inst[MAXPAT][MAXROW][NCH];
static u8 pat_fx[MAXPAT][MAXROW][NCH];
static u8 pat_prm[MAXPAT][MAXROW][NCH];
static u8 pat_len[MAXPAT];
static Ch ch[NCH];
static u8 bpm, tpr, n_inst, n_pat, ord_len;
static i32 spt, tick_ctr; /* samples per tick, tick counter */
static i32 cur_tick, cur_row, ord_pos;
static i32 sync_row, sync_pat;

/* --- note freq: 12-entry base table, shift for octaves --- */
static const i32 bf[12] = {24,26,27,29,31,32,34,36,38,41,43,46};

static i32 nf(i32 n) {
    if (n < 0) n = 0;
    if (n > 95) n = 95;
    return bf[n % 12] << (n / 12);
}

/* --- waveforms --- */
static i32 gen(Ch *c) {
    i32 p = c->phase;
    switch (c->wave) {
        case 0: return p < c->duty ? 16383 : -16383; /* pulse */
        case 1: /* triangle */
            return p < FXH ?
                (p * 4 - FX1) * 32767 / FX1 :
                (FX1 * 3 - p * 4) * 32767 / FX1;
        case 2: return (p * 2 - FX1) * 32767 / FX1; /* saw */
        default: /* noise */ {
            c->lfsr_c++;
            if (c->lfsr_c >= c->lfsr_p) {
                c->lfsr_c = 0;
                u16 b = (c->lfsr ^ (c->lfsr >> 1)) & 1;
                c->lfsr = (c->lfsr >> 1) | (b << 14);
            }
            return (c->lfsr & 1) ? 16383 : -16383;
        }
    }
}

/* --- envelope tick --- */
static void env(Ch *c) {
    switch (c->env_st) {
        case 1: /* attack */
            c->env += FX1 / (c->a + 1);
            if (c->env >= FX1) { c->env = FX1; c->env_st = 2; }
            break;
        case 2: { /* decay */
            i32 t = FX1 * c->s / 255;
            c->env -= (FX1 - t) / (c->d + 1);
            if (c->env <= t) { c->env = t; c->env_st = 3; }
            break;
        }
        case 4: /* release */
            c->env -= c->env / (c->r + 1);
            if (c->env < 64) { c->env = 0; c->env_st = 0; }
            break;
    }
}

/* --- trigger --- */
static void trig(Ch *c, u8 note, Inst *in) {
    i32 semi = note - 2;
    c->base_note = semi;
    c->base_freq = nf(semi);
    c->freq = c->base_freq;
    c->phase = 0;
    c->wave = in->wave;
    /* decode duty: 0→12.5% 1→25% 2→50% 3→75% */
    static const i32 dv[4] = {8192, 16384, 32768, 49152};
    c->duty = dv[in->duty & 3];
    c->a = in->a; c->d = in->d; c->s = in->s; c->r = in->r;
    c->env_st = 1; c->env = 0;
    c->pe = in->pe; c->pe_t = in->pe_len;
    c->arp1 = in->arp1; c->arp2 = in->arp2; c->arp_t = 0;
    c->slide = 0;
    if (in->wave >= 3) { c->lfsr = 0x7FFF; c->lfsr_c = 0; c->lfsr_p = 256 >> (semi / 12); if (!c->lfsr_p) c->lfsr_p = 1; }
}

/* --- process row --- */
static void row(void) {
    for (i32 c = 0; c < NCH; c++) {
        i32 pi = order[ord_pos][c];
        if (pi >= n_pat) continue;
        u8 n = pat_note[pi][cur_row][c];
        if (n == 1) { ch[c].env_st = 4; } /* note off */
        else if (n >= 2) {
            u8 ii = pat_inst[pi][cur_row][c];
            if (ii == 0xFF) ii = ch[c].ci;
            if (ii < n_inst) { ch[c].ci = ii; trig(&ch[c], n, &inst[ii]); }
        }
        u8 fx = pat_fx[pi][cur_row][c];
        u8 pm = pat_prm[pi][cur_row][c];
        if (fx == 1) { ch[c].arp1 = (pm >> 4); ch[c].arp2 = pm & 0xF; ch[c].arp_t = 0; }
        else if (fx == 2) { ch[c].slide = (i32)pm << 2; }
        else if (fx == 3) { ch[c].slide = -((i32)pm << 2); }
        else if (fx == 6) { i32 up=(pm>>4)&0xF,dn=pm&0xF; ch[c].vs=(i8)(up?up:-dn); }
    }
}

/* --- tick --- */
static void tick(void) {
    cur_tick++;
    if (cur_tick >= tpr) {
        cur_tick = 0;
        cur_row++;
        i32 pl = pat_len[order[ord_pos][0]];
        if (!pl) pl = MAXROW;
        if (cur_row >= pl) {
            cur_row = 0;
            ord_pos++;
            if (ord_pos >= ord_len) ord_pos = 0;
        }
        row();
        sync_row = cur_row;
        sync_pat = ord_pos;
    }
    for (i32 c = 0; c < NCH; c++) {
        Ch *p = &ch[c];
        if (p->pe_t) { p->base_freq += (i32)p->pe << 2; if (p->base_freq < 1) p->base_freq = 1; p->pe_t--; }
        if (p->slide) { p->base_freq += p->slide; if (p->base_freq < 1) p->base_freq = 1; }
        if (p->arp1 | p->arp2) {
            i32 n = p->base_note, t = p->arp_t % 3;
            if (t == 1) n += p->arp1; else if (t == 2) n += p->arp2;
            p->freq = nf(n); p->arp_t++;
        } else p->freq = p->base_freq;
        env(p);
    }
}

/* --- exports --- */

#define EXP __attribute__((export_name(

__attribute__((export_name("getSongBuf")))
u32 getSongBuf(void) { return (u32)(void*)song_buf; }

__attribute__((export_name("getOutputBuf")))
u32 getOutputBuf(void) { return (u32)(void*)out_buf; }

__attribute__((export_name("init")))
i32 init(i32 sz) {
    u8 *d = song_buf;
    if (sz < 8 || d[0]!='B'||d[1]!='R'||d[2]!='B'||d[3]!='1') return -1;
    bpm = d[4]; tpr = d[5]; n_inst = d[6]; n_pat = d[7];
    i32 p = 8;
    ord_len = d[p++];
    for (i32 i = 0; i < ord_len; i++)
        for (i32 c = 0; c < NCH; c++) order[i][c] = d[p++];
    for (i32 i = 0; i < n_inst; i++) {
        inst[i] = (Inst){d[p],d[p+1],d[p+2],d[p+3],d[p+4],d[p+5],(i8)d[p+6],d[p+7],d[p+8],d[p+9]};
        p += 12;
    }
    for (i32 pi = 0; pi < n_pat; pi++) {
        i32 nr = d[p++]; pat_len[pi] = nr;
        for (i32 c = 0; c < NCH; c++) for (i32 r = 0; r < nr; r++) pat_note[pi][r][c] = d[p++];
        for (i32 c = 0; c < NCH; c++) for (i32 r = 0; r < nr; r++) pat_inst[pi][r][c] = d[p++];
        for (i32 c = 0; c < NCH; c++) for (i32 r = 0; r < nr; r++) pat_fx[pi][r][c] = d[p++];
        for (i32 c = 0; c < NCH; c++) for (i32 r = 0; r < nr; r++) pat_prm[pi][r][c] = d[p++];
    }
    /* init state */
    for (i32 c = 0; c < NCH; c++) { ch[c].lfsr = 0x7FFF; ch[c].lfsr_p = 16; }
    i32 b = bpm ? bpm : 125;
    spt = SR * 5 / (b * 2);
    tick_ctr = 0; cur_tick = 0; cur_row = 0; ord_pos = 0;
    row(); /* process row 0 immediately */
    return 0;
}

__attribute__((export_name("render")))
void render(i32 n) {
    if (n > 128) n = 128;
    for (i32 i = 0; i < n; i++) {
        if (tick_ctr <= 0) { tick(); tick_ctr = spt; }
        tick_ctr--;
        i32 mix = 0;
        for (i32 c = 0; c < NCH; c++) {
            Ch *p = &ch[c];
            if (!p->env_st && !p->env) continue;
            i32 s = gen(p);
            mix += (s * (p->env >> 8)) >> 8;
            p->phase += p->freq;
            if (p->phase >= FX1) p->phase -= FX1;
        }
        if (mix > 32767) mix = 32767;
        if (mix < -32767) mix = -32767;
        out_buf[i] = mix;
    }
}

__attribute__((export_name("getRow")))
i32 getRow(void) { return sync_row; }

__attribute__((export_name("getPattern")))
i32 getPattern(void) { return sync_pat; }
