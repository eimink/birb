/*
 * birb4k.js — ultra-compact JS synth player for 4K web demos
 *
 * Usage:
 *   let b = birb4k(songDataArrayBuffer);
 *   b.play();          // start audio
 *   b.row / b.pat      // current position (for visual sync)
 *   b.stop();           // stop
 *
 * Design: renders offline into an AudioBuffer, then plays.
 * This avoids AudioWorklet entirely (no extra file, no COOP/COEP).
 * For a 3-min song at 44100Hz mono 32bit: ~10MB RAM. Fine for a demo.
 */
function birb4k(buf) {
    let d = new Uint8Array(buf), p = 8,
        bpm = d[4], tpr = d[5], ni = d[6], np = d[7],
        ol = d[p++], ord = [], inst = [], pn = [], pi = [], pf = [], pp = [], pl = [],
        SR = 44100, NCH = 4;

    // order
    for (let i = 0; i < ol; i++) {
        ord[i] = [];
        for (let c = 0; c < NCH; c++) ord[i][c] = d[p++];
    }
    // instruments
    for (let i = 0; i < ni; i++)
        inst[i] = { w: d[p], du: d[p + 1], a: d[p + 2], d: d[p + 3], s: d[p + 4], r: d[p + 5], pe: d[p + 6] > 127 ? d[p + 6] - 256 : d[p + 6], pl: d[p + 7], a1: d[p + 8], a2: d[p + 9] }, p += 12;

    // patterns (planar)
    for (let i = 0; i < np; i++) {
        let nr = d[p++]; pl[i] = nr;
        pn[i] = []; pi[i] = []; pf[i] = []; pp[i] = [];
        for (let c = 0; c < NCH; c++) { pn[i][c] = []; for (let r = 0; r < nr; r++) pn[i][c][r] = d[p++]; }
        for (let c = 0; c < NCH; c++) { pi[i][c] = []; for (let r = 0; r < nr; r++) pi[i][c][r] = d[p++]; }
        for (let c = 0; c < NCH; c++) { pf[i][c] = []; for (let r = 0; r < nr; r++) pf[i][c][r] = d[p++]; }
        for (let c = 0; c < NCH; c++) { pp[i][c] = []; for (let r = 0; r < nr; r++) pp[i][c][r] = d[p++]; }
    }

    // note freq: base octave, shift for higher
    let bf = [24, 26, 27, 29, 31, 32, 34, 36, 38, 41, 43, 46];
    let nf = n => (n < 0 ? n = 0 : n > 95 && (n = 95), bf[n % 12] << (n / 12));
    let FX = 65536, dv = [8192, 16384, 32768, 49152];

    // render entire song offline
    let spt = SR * 5 / ((bpm || 125) * 2) | 0,
        // estimate total samples: order_length * max_rows * tpr * spt
        maxRows = pl.reduce((a, b) => a > b ? a : b, 16),
        totalSamples = ol * maxRows * (tpr || 6) * spt,
        out = new Float32Array(totalSamples),
        ch = [];

    for (let c = 0; c < NCH; c++)
        ch[c] = { ph: 0, fr: 0, bf: 0, w: 0, bn: 0, du: FX / 2, en: 0, es: 0, a: 0, d: 0, s: 0, r: 0, pe: 0, pt: 0, a1: 0, a2: 0, at: 0, sl: 0, lf: 0x7FFF, lp: 16, lc: 0, ci: 0 };

    let cTick = -1, cRow = 0, oPos = 0, tCtr = 0;
    let syncData = []; // [sample] = {row, pat}

    function doRow() {
        for (let c = 0; c < NCH; c++) {
            let qi = ord[oPos][c]; if (qi >= np) continue;
            let n = pn[qi][c][cRow], ii = pi[qi][c][cRow], fx = pf[qi][c][cRow], pm = pp[qi][c][cRow];
            if (n == 1) ch[c].es = 4;
            else if (n >= 2) {
                if (ii == 255) ii = ch[c].ci;
                if (ii < ni) {
                    ch[c].ci = ii;
                    let ins = inst[ii], semi = n - 2;
                    ch[c].bn = semi; ch[c].bf = nf(semi); ch[c].fr = ch[c].bf;
                    ch[c].ph = 0; ch[c].w = ins.w; ch[c].du = dv[ins.du & 3];
                    ch[c].a = ins.a; ch[c].d = ins.d; ch[c].s = ins.s; ch[c].r = ins.r;
                    ch[c].es = 1; ch[c].en = 0;
                    ch[c].pe = ins.pe; ch[c].pt = ins.pl;
                    ch[c].a1 = ins.a1; ch[c].a2 = ins.a2; ch[c].at = 0; ch[c].sl = 0;
                    if (ins.w >= 3) { ch[c].lf = 0x7FFF; ch[c].lc = 0; ch[c].lp = 256 >> (semi / 12) || 1; }
                }
            }
            if (fx == 1) { ch[c].a1 = pm >> 4; ch[c].a2 = pm & 15; ch[c].at = 0; }
            else if (fx == 2) ch[c].sl = pm << 2;
            else if (fx == 3) ch[c].sl = -(pm << 2);
            else if (fx == 6) ch[c].en = FX * pm / 255;
        }
    }

    function doTick() {
        cTick++;
        if (cTick >= (tpr || 6)) {
            cTick = 0; cRow++;
            let plen = pl[ord[oPos][0]] || 32;
            if (cRow >= plen) { cRow = 0; oPos++; if (oPos >= ol) oPos = 0; }
            doRow();
        }
        for (let c = 0; c < NCH; c++) {
            let C = ch[c];
            if (C.pt) { C.bf += C.pe << 2; if (C.bf < 1) C.bf = 1; C.pt--; }
            if (C.sl) { C.bf += C.sl; if (C.bf < 1) C.bf = 1; }
            if (C.a1 | C.a2) {
                let n = C.bn, t = C.at % 3;
                if (t == 1) n += C.a1; else if (t == 2) n += C.a2;
                C.fr = nf(n); C.at++;
            } else C.fr = C.bf;
            // envelope
            let es = C.es;
            if (es == 1) { C.en += FX / (C.a + 1); if (C.en >= FX) { C.en = FX; C.es = 2; } }
            else if (es == 2) { let t = FX * C.s / 255; C.en -= (FX - t) / (C.d + 1); if (C.en <= t) { C.en = t; C.es = 3; } }
            else if (es == 4) { C.en -= C.en / (C.r + 1); if (C.en < 64) { C.en = 0; C.es = 0; } }
        }
    }

    // render loop
    for (let i = 0; i < totalSamples; i++) {
        if (tCtr <= 0) { doTick(); tCtr = spt; }
        tCtr--;
        let mix = 0;
        for (let c = 0; c < NCH; c++) {
            let C = ch[c];
            if (!C.es && !C.en) continue;
            let ph = C.ph, s;
            if (C.w == 0) s = ph < C.du ? .5 : -.5;
            else if (C.w == 1) s = ph < FX / 2 ? (ph * 4 - FX) / FX : (FX * 3 - ph * 4) / FX;
            else if (C.w == 2) s = (ph * 2 - FX) / FX;
            else { // noise
                C.lc++;
                if (C.lc >= C.lp) { C.lc = 0; let b = (C.lf ^ (C.lf >> 1)) & 1; C.lf = (C.lf >> 1) | (b << 14); }
                s = (C.lf & 1) ? .5 : -.5;
            }
            mix += s * C.en / FX;
            C.ph = (C.ph + C.fr) % FX;
        }
        out[i] = mix > 1 ? 1 : mix < -1 ? -1 : mix;

        // store sync data at row boundaries
        if (i % spt == 0) syncData[i / spt | 0] = { r: cRow, p: oPos };
    }

    // playback via AudioContext
    let ctx, src, state = { row: 0, pat: 0, playing: 0 };
    return {
        get row() { return state.row; },
        get pat() { return state.pat; },
        get playing() { return state.playing; },
        samples: out,
        syncData,
        spt,
        play() {
            ctx = new AudioContext({ sampleRate: SR });
            let ab = ctx.createBuffer(1, out.length, SR);
            ab.getChannelData(0).set(out);
            src = ctx.createBufferSource();
            src.buffer = ab;
            src.connect(ctx.destination);
            src.loop = true;
            src.start();
            state.playing = 1;
            // sync timer
            let t0 = ctx.currentTime;
            let update = () => {
                if (!state.playing) return;
                let elapsed = ((ctx.currentTime - t0) * SR) | 0;
                let tick = (elapsed / spt | 0) % syncData.length;
                if (syncData[tick]) { state.row = syncData[tick].r; state.pat = syncData[tick].p; }
                requestAnimationFrame(update);
            };
            update();
        },
        stop() {
            state.playing = 0;
            if (src) try { src.stop(); } catch (e) {}
            if (ctx) ctx.close();
        }
    };
}
