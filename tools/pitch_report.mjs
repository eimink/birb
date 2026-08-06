#!/usr/bin/env node
/* pitch_report — realized pitch, per note, per implementation.
 *
 *   node tools/pitch_report.mjs [--probes DIR] [--only impl,impl] [--self-test]
 *
 * Renders the single-note probes from tools/gen_probes.py through every render
 * path in the tree and reports cents error against equal temperament. This is
 * both the evidence for PITCH.md §2 and the acceptance test for §8 — run it
 * before touching anything and diff after.
 *
 * Song mixes cannot be used for this. Overlapping voices and unpitched
 * percussion give an estimator a handful of usable windows and a lot of octave
 * ambiguity; one note at a time is what makes the measurement exact.
 */

import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { exportSong } from './editor_export.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const SR = 44100;
const A4 = 440;
/* C-0 in equal temperament: A4 is note 57 (4*12 + 9). */
const ET = n => A4 * Math.pow(2, (n - 57) / 12);
const CENTS = (f, n) => 1200 * Math.log2(f / ET(n));

/* ---------- frequency estimation ----------
 * Two stages. Autocorrelation with a parabolic peak gives a coarse period good
 * to roughly 0.1%; that is nowhere near enough to talk about 0.1-cent table
 * accuracy, so it only serves to seed the second stage. The refinement
 * demodulates by the coarse estimate, integrates into blocks, and least-squares
 * fits the residual phase ramp — the slope is the frequency error directly.
 * Block length is capped so the coarse error cannot wrap a block's phase. */

function removeDC(x) {
    let m = 0;
    for (let i = 0; i < x.length; i++) m += x[i];
    m /= x.length;
    const y = new Float64Array(x.length);
    for (let i = 0; i < x.length; i++) y[i] = x[i] - m;
    return y;
}

/* Returns {period, pinned} — pinned means the peak landed on a search bound,
 * so the true period is outside the window and the number is not a
 * measurement. KS above C-6 does this: the delay line is a dozen samples and
 * the realized pitch leaves the +/-40% bracket entirely. */
function coarsePeriod(x, minLag, maxLag) {
    let best = -Infinity, bl = minLag;
    const ac = new Float64Array(maxLag + 2);
    for (let lag = minLag; lag <= maxLag; lag++) {
        let s = 0;
        const n = x.length - lag;
        for (let i = 0; i < n; i++) s += x[i] * x[i + lag];
        s /= n;                       /* bias-correct: long lags see fewer terms */
        ac[lag] = s;
        if (s > best) { best = s; bl = lag; }
    }
    if (bl <= minLag || bl >= maxLag) return { period: bl, pinned: true };
    const y0 = ac[bl - 1], y1 = ac[bl], y2 = ac[bl + 1];
    const d = y0 - 2 * y1 + y2;
    return { period: bl + (d ? 0.5 * (y0 - y2) / d : 0), pinned: false };
}

function refine(x, f0) {
    const period = SR / f0;
    /* Coarse error is <=0.5%; keep a block under 0.35 turns of that error. */
    let B = Math.floor(Math.min(x.length / 6, 0.35 * SR / (0.005 * f0), 64 * period));
    B = Math.max(B, Math.ceil(4 * period));
    const M = Math.floor(x.length / B);
    /* Too few cycles in the window for a phase ramp to mean anything — the
     * parabolic autocorrelation estimate is the better answer down here. */
    if (M < 4 || B < 8) return f0;

    const w = 2 * Math.PI * f0 / SR;
    const ph = new Float64Array(M);
    for (let b = 0; b < M; b++) {
        let re = 0, im = 0;
        for (let i = 0; i < B; i++) {
            const n = b * B + i, a = w * n;
            re += x[n] * Math.cos(a);
            im -= x[n] * Math.sin(a);
        }
        ph[b] = Math.atan2(im, re);
    }
    /* unwrap */
    for (let b = 1; b < M; b++) {
        let d = ph[b] - ph[b - 1];
        while (d > Math.PI) { ph[b] -= 2 * Math.PI; d = ph[b] - ph[b - 1]; }
        while (d < -Math.PI) { ph[b] += 2 * Math.PI; d = ph[b] - ph[b - 1]; }
    }
    /* least-squares slope, radians per block */
    let sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (let b = 0; b < M; b++) { sx += b; sy += ph[b]; sxx += b * b; sxy += b * ph[b]; }
    const denom = M * sxx - sx * sx;
    if (!denom) return f0;
    const slope = (M * sxy - sx * sy) / denom;
    return f0 + slope * SR / (2 * Math.PI * B);
}

function estimate(samples, expectedHz) {
    const x = removeDC(samples);
    let energy = 0;
    for (let i = 0; i < x.length; i++) energy += x[i] * x[i];
    if (Math.sqrt(energy / x.length) < 1e-4) return null;   /* silent */
    const p = SR / expectedHz;
    const minLag = Math.max(2, Math.floor(p * 0.7));
    const maxLag = Math.min(x.length >> 1, Math.ceil(p * 1.4));
    if (maxLag <= minLag + 2) return null;
    const { period, pinned } = coarsePeriod(x, minLag, maxLag);
    if (pinned) return { hz: SR / period, pinned: true };
    return { hz: refine(x, SR / period), pinned: false };
}

/* ---------- self-test ----------
 * Measures the estimator against synthetic tones of known frequency to
 * establish its noise floor per octave. The floor is set by window length in
 * cycles, so it is worst at C-0 and negligible above C-2. Acoustic numbers
 * below the floor mean nothing; exact tuning is certified by the note-table
 * comparison instead, which involves no audio at all. */
function selfTest(rowLen) {
    const floor = new Float64Array(96);
    let worst = 0, worstN = 0;
    for (let n = 0; n < 96; n++) {
        const f = ET(n) * Math.pow(2, 7 / 1200);      /* 7 cents sharp on purpose */
        const N = Math.floor(rowLen * 0.8);
        const x = new Float64Array(N);
        for (let i = 0; i < N; i++)                    /* fundamental + harmonics */
            x[i] = Math.sin(2 * Math.PI * f * i / SR)
                 + 0.4 * Math.sin(4 * Math.PI * f * i / SR)
                 + 0.2 * Math.sin(6 * Math.PI * f * i / SR);
        const got = estimate(x, ET(n));
        floor[n] = got === null ? NaN : Math.abs(1200 * Math.log2(got.hz / f));
        if (floor[n] > worst) { worst = floor[n]; worstN = n; }
    }
    console.log('estimator noise floor (synthetic tones, exact frequency known)');
    for (let o = 0; o < 8; o++) {
        const oct = Array.from(floor.slice(o * 12, o * 12 + 12));
        const mx = Math.max(...oct);
        console.log(`  octave ${o}: worst ${mx.toFixed(4).padStart(8)} cents` +
                    (mx > 0.05 ? '   <- window is short in cycles here' : ''));
    }
    console.log(`  overall ${worst.toFixed(4)} cents at note ${worstN}.` +
                ' Acoustic readings finer than this are noise;\n' +
                '  the note-table section below is exact and carries the tuning claim.\n');
    return { floor, worst };
}

/* ---------- render paths ---------- */
const sh = (cmd, args, opts = {}) =>
    execFileSync(cmd, args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'], ...opts });

function loadRawS16(path) {
    const b = readFileSync(path);
    const n = b.length >> 1, out = new Float64Array(n);
    for (let i = 0; i < n; i++) out[i] = b.readInt16LE(i * 2) / 32768;
    return out;
}


const PATHS = {
    /* The shipped path: editor.html's own exporter, driven headlessly. */
    'editor export': (bsb) => {
        const js = exportSong(readFileSync(bsb));
        return new Function(js + '; return birb;')()(0).o;
    },
    'birb_synth.c': (bsb, work) => {
        const raw = join(work, 'c.raw');
        sh(join(ROOT, 'tools/birb_render'), [bsb, raw]);
        return loadRawS16(raw);
    },
    'birbc --js': (bsb, work) => {
        sh(join(ROOT, 'birbc'), [bsb, '-o', join(work, 'lj'), '--js']);
        const src = readFileSync(join(work, 'lj.js'), 'utf8');
        return new Function(src + '; return birb;')()(0).o;
    },
    'birbc locked.c': (bsb, work) => {
        sh(join(ROOT, 'birbc'), [bsb, '-o', join(work, 'lc')]);
        const raw = join(work, 'lc.raw');
        sh('clang', ['-Os', '-std=c11', join(ROOT, 'tools/locked_driver.c'),
                     join(work, 'lc_locked.c'), '-o', join(work, 'lc_bin')]);
        sh(join(work, 'lc_bin'), [raw]);
        return loadRawS16(raw);
    },
};

/* ---------- note tables, exactly ----------
 * The acoustic measurement above catches wiring bugs but cannot certify a
 * 0.1-cent table at C-0 — half a second is eight cycles there. Each
 * implementation's lookup is compared directly instead. This is the check that
 * would have caught birb4k.js sitting on the superseded integer table. */

/* Pull `bf` and the body of `nf` straight out of a player and evaluate the
 * player's own expression against the player's own table. Regexes cannot do
 * the body — `(n/12)` closes a paren the lookup still needs — so balance. */
function jsLookup(file, bfRe) {
    const src = readFileSync(join(ROOT, file), 'utf8');
    const bf = src.match(bfRe);
    const at = src.search(/nf\s*=\s*n\s*=>\s*\(/);
    if (!bf || at < 0) return null;
    let i = src.indexOf('(', src.indexOf('=>', at));
    const start = i, n = src.length;
    for (let depth = 0; i < n; i++) {
        if (src[i] === '(') depth++;
        else if (src[i] === ')' && --depth === 0) { i++; break; }
    }
    const table = JSON.parse(bf[1].replace(/\s+/g, ''));
    return new Function('bf', `return n => ${src.slice(start, i)};`)(table);
}

const TABLES = {
    'birb_synth.c': () => {
        const out = sh(join(ROOT, 'tools/birb_render'), ['--dump-notes']).toString();
        const v = out.trim().split('\n').map(l => Number(l.split(' ')[1]));
        return n => v[n];
    },
    'birbc --js': (work) => {
        /* Evaluate the emitted player's OWN nf, never a copy of it. */
        const src = readFileSync(join(work, 'lj.js'), 'utf8');
        const bf = JSON.parse(src.match(/bf=(\[[^\]]*\])/)[1]);
        const at = src.search(/nf=n=>\(/);
        let i = src.indexOf('(', src.indexOf('=>', at));
        const start = i;
        for (let d = 0; i < src.length; i++) {
            if (src[i] === '(') d++;
            else if (src[i] === ')' && --d === 0) { i++; break; }
        }
        return new Function('bf', `return n => ${src.slice(start, i)};`)(bf);
    },
};

function tableReport(work) {
    console.log('note lookup, compared exactly (no audio involved)');
    console.log('increment -> Hz = inc * 44100 / 2**32; error vs equal temperament\n');
    const rows = [];
    for (const [name, get] of Object.entries(TABLES)) {
        let fn;
        try { fn = get(work); } catch (e) { rows.push([name, null, e.message.slice(0, 40)]); continue; }
        if (!fn) { rows.push([name, null, 'could not extract lookup']); continue; }
        let worst = 0, wn = 0, sum = 0;
        for (let n = 0; n < 96; n++) {
            const hz = fn(n) * SR / 2 ** 32;
            const e = CENTS(hz, n);
            sum += Math.abs(e);
            if (Math.abs(e) > Math.abs(worst)) { worst = e; wn = n; }
        }
        rows.push([name, { worst, wn, mean: sum / 96, fn }]);
    }
    const w = Math.max(...rows.map(r => r[0].length));
    for (const [name, r, err] of rows) {
        if (!r) { console.log(`  ${name.padEnd(w)}  UNAVAILABLE — ${err}`); continue; }
        console.log(`  ${name.padEnd(w)}  mean |e| ${r.mean.toFixed(3).padStart(7)}` +
                    `  worst ${r.worst.toFixed(2).padStart(7)} cents @ note ${r.wn}`);
    }
    /* pairwise agreement — divergence between paths is the real defect */
    const ok = rows.filter(r => r[1]);
    console.log('\n  pairwise: largest disagreement between implementations');
    let anyDiff = false;
    for (let i = 0; i < ok.length; i++)
        for (let j = i + 1; j < ok.length; j++) {
            let worst = 0, wn = 0;
            for (let n = 0; n < 96; n++) {
                const a = ok[i][1].fn(n), b = ok[j][1].fn(n);
                if (!a || !b) continue;
                const d = 1200 * Math.log2(a / b);
                if (Math.abs(d) > Math.abs(worst)) { worst = d; wn = n; }
            }
            const tag = Math.abs(worst) < 1e-9 ? 'identical' :
                        `${worst.toFixed(2)} cents @ note ${wn}`;
            if (Math.abs(worst) >= 1e-9) anyDiff = true;
            console.log(`    ${ok[i][0]} vs ${ok[j][0]}: ${tag}`);
        }
    console.log(anyDiff
        ? '\n  ^ any non-identical pair is a tuning fork in the tree.\n'
        : '\n  all implementations agree exactly.\n');
}

/* ---------- KS: delay-line clamping and decay ----------
 * KS pitch is set by an integer delay length, so it has two failure modes that
 * must be reported apart. Below ~43 Hz the length wants more than
 * BIRB_KS_BUF_SIZE and gets clamped — the voice plays a different note
 * entirely, which is a buffer tradeoff. Everywhere else the error is delay
 * truncation, which is the precision problem. Averaging them together hides
 * both. */
const KS_BUF = 1024;   /* BIRB_KS_BUF_SIZE default */

function ksLength(inc) { return inc > 0 ? Math.floor(2 ** 32 / inc) : 0; }

/* T60 from the RMS envelope: block the row, take log amplitude, least-squares
 * the slope in dB/s, extrapolate to -60 dB. */
function t60(x, blocks = 24) {
    const B = Math.floor(x.length / blocks);
    if (B < 64) return null;
    const t = [], db = [];
    for (let b = 0; b < blocks; b++) {
        let s = 0;
        for (let i = 0; i < B; i++) { const v = x[b * B + i]; s += v * v; }
        const rms = Math.sqrt(s / B);
        if (rms < 1e-6) break;
        t.push(b * B / SR);
        db.push(20 * Math.log10(rms));
    }
    if (t.length < 6) return null;
    let sx = 0, sy = 0, sxx = 0, sxy = 0;
    const m = t.length;
    for (let i = 0; i < m; i++) { sx += t[i]; sy += db[i]; sxx += t[i] * t[i]; sxy += t[i] * db[i]; }
    const den = m * sxx - sx * sx;
    if (!den) return null;
    const slope = (m * sxy - sx * sy) / den;     /* dB per second, negative */
    return slope < -0.5 ? -60 / slope : null;
}

function ksReport(pcm, tableFn) {
    const clamped = [], free = [];
    const decays = [];
    for (let n = 0; n < 96; n++) {
        const a = n * ROW + (ROW * 0.12 | 0), b = (n + 1) * ROW - (ROW * 0.08 | 0);
        if (b > pcm.length) break;
        const len = ksLength(tableFn(n));
        const seg = pcm.subarray(a, b);
        const f = estimate(seg, ET(n));
        const e = (f && !f.pinned) ? CENTS(f.hz, n) : null;
        (len < 4 || len > KS_BUF ? clamped : free).push(e);
        const d = t60(pcm.subarray(n * ROW, (n + 1) * ROW));
        if (d !== null && len >= 4 && len <= KS_BUF) decays.push([n, d]);
    }
    const stat = a => {
        const g = a.filter(v => v !== null);
        if (!g.length) return null;
        return {
            n: g.length,
            mean: g.reduce((s, v) => s + Math.abs(v), 0) / g.length,
            worst: g.reduce((p, v) => Math.abs(v) > Math.abs(p) ? v : p, 0),
        };
    };
    return { free: stat(free), clamped: stat(clamped), nClamped: clamped.length, decays };
}

/* ---------- 4xy vibrato depth ----------
 * PITCH.md §2.2: peak deviation is y/8 LSB truncated to an integer, so y=1..7
 * should be exactly zero and y=8..15 should all be the same. Measured by
 * pitch-tracking short windows across a row and taking the excursion. */
function vibratoReport(pcm, meta) {
    const V = meta.vibrato;
    const row = V.ticks * Math.floor(SR * 5 / (V.bpm * 2));
    const expect = ET(V.note);
    const out = [];
    for (let y = 0; y < V.rows; y++) {
        const a = y * row + (row * 0.1 | 0), b = (y + 1) * row - (row * 0.05 | 0);
        if (b > pcm.length) break;
        const win = 2048, hop = 512;
        const track = [];
        for (let o = a; o + win < b; o += hop) {
            const f = estimate(pcm.subarray(o, o + win), expect);
            if (f && !f.pinned) track.push(f.hz);
        }
        if (track.length < 8) { out.push(null); continue; }
        track.sort((p, q) => p - q);
        /* trim 10% each end so a single bad window cannot define the peak */
        const lo = track[Math.floor(track.length * 0.1)];
        const hi = track[Math.ceil(track.length * 0.9) - 1];
        out.push({ hz: (hi - lo) / 2, cents: 1200 * Math.log2(hi / lo) / 2 });
    }
    return out;
}

/* ---------- main ---------- */
const argv = process.argv.slice(2);
const arg = (flag, dflt) => {
    const i = argv.indexOf(flag);
    return i >= 0 && argv[i + 1] ? argv[i + 1] : dflt;
};
const SELFTEST_ROW = 22044;   /* matches gen_probes.py defaults */
if (argv.includes('--self-test')) {
    process.exit(selfTest(SELFTEST_ROW).worst < 2 ? 0 : 1);
}

const probeDir = arg('--probes', join(ROOT, 'build/probes'));
const only = arg('--only', null)?.split(',');
const meta = JSON.parse(readFileSync(join(probeDir, 'probes.json'), 'utf8'));
const ROW = meta.ticks * Math.floor(SR * 5 / (meta.bpm * 2));
const work = join(probeDir, 'work');
mkdirSync(work, { recursive: true });

const { floor: NOISE } = selfTest(ROW);

const impls = Object.keys(PATHS).filter(k => !only || only.includes(k));
const results = {};

for (const probe of meta.probes) {
    const birb = join(probeDir, `probe_${probe}.birb`);
    const bsb = join(work, `probe_${probe}.bsb`);
    sh(join(ROOT, 'birbc'), [birb, '-o', join(work, `probe_${probe}`), '--tracker']);

    for (const impl of impls) {
        let pcm;
        try {
            pcm = PATHS[impl](bsb, work);
        } catch (e) {
            results[`${probe}|${impl}`] = { error: e.message.split('\n')[0].slice(0, 60) };
            continue;
        }
        if (!pcm || pcm.length < ROW) {
            results[`${probe}|${impl}`] = {
                error: `rendered ${pcm ? pcm.length : 0} samples, expected ${ROW * meta.num_notes}`,
            };
            continue;
        }
        const errs = [], pinned = [];
        for (let n = 0; n < meta.num_notes; n++) {
            const a = n * ROW + (ROW * 0.12 | 0), b = (n + 1) * ROW - (ROW * 0.08 | 0);
            if (b > pcm.length) break;
            const f = estimate(pcm.subarray(a, b), ET(n));
            errs.push(f === null ? null : CENTS(f.hz, n));
            pinned.push(f !== null && f.pinned);
        }
        results[`${probe}|${impl}`] = { errs, pinned };
        if (probe === 'ks') results[`ks-detail|${impl}`] = { pcm };
    }
}

/* ---------- report ---------- */
tableReport(work);   /* needs an emitted player, so it runs after the loop */

const fmt = v => v === null || v === undefined ? '   —  ' : v.toFixed(2).padStart(6);
console.log('cents error vs equal temperament, by probe and implementation');
console.log('(worst = largest |error| over the notes that produced a pitch)\n');

const w = Math.max(...impls.map(s => s.length));
for (const probe of meta.probes) {
    console.log(`${probe}:`);
    for (const impl of impls) {
        const r = results[`${probe}|${impl}`];
        if (r.error) { console.log(`  ${impl.padEnd(w)}  FAILED — ${r.error}`); continue; }
        /* Pinned notes are out of the estimator's bracket entirely — count them,
         * do not average them in and call it a measurement. */
        const usable = r.errs.map((v, i) => (v !== null && !r.pinned[i]) ? v : null);
        const good = usable.filter(v => v !== null);
        const nPinned = r.pinned.filter(Boolean).length;
        const tail = nPinned ? `  (${nPinned} notes off-bracket, unmeasured)` : '';
        if (!good.length) {
            console.log(`  ${impl.padEnd(w)}  no measurable pitch${tail}`);
            continue;
        }
        const worst = good.reduce((p, v) => Math.abs(v) > Math.abs(p) ? v : p, 0);
        const mean = good.reduce((s, v) => s + Math.abs(v), 0) / good.length;
        const wn = usable.indexOf(worst);
        console.log(`  ${impl.padEnd(w)}  notes ${String(good.length).padStart(3)}` +
                    `  mean |e| ${fmt(mean)}  worst ${fmt(worst)} @ note ${wn}${tail}`);
    }
    console.log('');
}

/* ---------- KS detail ---------- */
{
    const tbl = TABLES['birb_synth.c']();
    console.log('KS delay line — clamping and truncation reported separately');
    console.log(`(clamped = wanted length outside 4..${KS_BUF}; that is a buffer` +
                ' tradeoff, not a precision one)\n');
    for (const impl of impls) {
        const d = results[`ks-detail|${impl}`];
        if (!d) { console.log(`  ${impl.padEnd(w)}  —`); continue; }
        const r = ksReport(d.pcm, tbl);
        const f = r.free, c = r.clamped;
        console.log(`  ${impl.padEnd(w)}  unclamped ${f ? `n=${String(f.n).padStart(2)}` +
            ` mean |e| ${f.mean.toFixed(2).padStart(6)} worst ${f.worst.toFixed(2).padStart(7)}` : 'none'}`);
        console.log(`  ${''.padEnd(w)}  clamped   ${c ? `n=${String(c.n).padStart(2)}` +
            ` mean |e| ${c.mean.toFixed(2).padStart(6)} worst ${c.worst.toFixed(2).padStart(7)}` : 'none'}`);
        if (r.decays.length) {
            const ds = r.decays.map(x => x[1]);
            const lo = Math.min(...ds), hi = Math.max(...ds);
            console.log(`  ${''.padEnd(w)}  T60 ${lo.toFixed(3)}..${hi.toFixed(3)} s ` +
                `over ${r.decays.length} notes — spread ${(hi / lo).toFixed(2)}x ` +
                `(pitch-independent decay wants 1.00x)`);
        }
    }
    console.log('');
}

/* ---------- 4xy ---------- */
if (meta.vibrato) {
    const vbirb = join(probeDir, 'probe_vibrato.birb');
    if (existsSync(vbirb)) {
        sh(join(ROOT, 'birbc'), [vbirb, '-o', join(work, 'probe_vibrato'), '--tracker']);
        const vbsb = join(work, 'probe_vibrato.bsb');
        console.log('4xy vibrato — peak pitch deviation by depth nibble y');
        console.log('(§2.2: y/8 LSB truncated to an integer, so y=1..7 should be' +
                    ' dead and y=8..15 identical)\n');
        console.log('     y ' + Array.from({ length: 16 }, (_, i) => i.toString(16))
            .map(s => s.padStart(6)).join(''));
        for (const impl of impls) {
            let pcm;
            try { pcm = PATHS[impl](vbsb, work); } catch { continue; }
            if (!pcm) continue;
            const v = vibratoReport(pcm, meta);
            const cells = v.map(x => x === null ? '   —  ' : x.cents.toFixed(2).padStart(6));
            console.log(`  ${impl.padEnd(w)}` + cells.join(''));
        }
        console.log('  (cents, peak deviation from centre)\n');
    }
}

/* per-octave detail for the canonical C engine, where the table error lives */
const base = results[`sine|birb_synth.c`];
if (base && base.errs) {
    console.log('birb_synth.c / sine — cents error by octave:');
    console.log('      ' + ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
        .map(s => s.padStart(6)).join(''));
    for (let o = 0; o < 8; o++) {
        const row = base.errs.slice(o * 12, o * 12 + 12).map(fmt).join('');
        console.log(`  oct${o} ${row}`);
    }
}
/* ks-detail entries hold raw PCM for the section above — megabytes of samples
 * that must not reach the JSON. */
const serialisable = Object.fromEntries(
    Object.entries(results).filter(([k]) => !k.startsWith('ks-detail|')));
writeFileSync(join(probeDir, 'pitch_report.json'), JSON.stringify(serialisable, null, 1));
console.log(`\nfull data: ${join(probeDir, 'pitch_report.json')}`);
