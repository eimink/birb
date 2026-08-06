#!/usr/bin/env node
/* parity — render the corpus through every path and diff the PCM.
 *
 *   node tools/parity.mjs [song.bsb ...]
 *
 * PITCH.md §8 wants this as the gate for the Q32 widening: step one must be
 * bit-identical, and the only way to know is to render the same songs before
 * and after through every implementation and compare samples.
 *
 * It is also the standing check that the five render paths have not drifted
 * apart. birb4k.js drifting onto a superseded note table AND a superseded
 * pattern layout, unnoticed for months, is the argument for owning this
 * harness — it has since been removed as unused.
 *
 * Songs are taken from .birb sources so they regenerate with the current
 * birbc. A .bsb with no source cannot be trusted to represent the format.
 */

import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync, mkdirSync, writeFileSync, existsSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { exportSong, renderInEditor } from './editor_export.mjs';
import { renderWasm, wasmAvailable, buildWasm } from './wasm_render.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const WORK = join(ROOT, 'build/parity');

/* .birb sources regenerate with the current birbc. The two .bsb entries are
 * hand-authored tracker output that has no source and cannot be regenerated —
 * they are here precisely because they exercise things the generated corpus
 * does not: a reverb section, and six channels. */
const CORPUS = [
    'test_song.birb',
    'dnb_demo.birb',
    'examples/ks_test.birb',
    'examples/drums_test.birb',
    'examples/formant_test.birb',
    'examples/drums_all.birb',
    'examples/beat_dr.bsb',
    'examples/dnb_h.bsb',
];

const sh = (cmd, args) =>
    execFileSync(cmd, args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });

function loadRawS16(path) {
    const b = readFileSync(path);
    const n = b.length >> 1, out = new Float64Array(n);
    for (let i = 0; i < n; i++) out[i] = b.readInt16LE(i * 2) / 32768;
    return out;
}

const PATHS = {
    /* The path productions actually ship. editor.html has its own JS emitter,
     * separate from birbc's, so covering birbc alone covers nothing anyone
     * runs. Listed first because it is the one that matters. */
    'editor export': (bsb) => {
        const js = exportSong(readFileSync(bsb));
        return new Function(js + '; return birb;')()(0).o;
    },
    'editor --smol': (bsb) => {
        const js = exportSong(readFileSync(bsb), { smol: true });
        return new Function(js + '; return birb;')()(0).o;
    },
    /* The editor's own engine, not its exporter — two separate bodies of code
     * in the same file. The parity rule calls this one the spec. */
    'editor engine': (bsb) => renderInEditor(readFileSync(bsb)),
    'editor engine smol': (bsb) => renderInEditor(readFileSync(bsb), { smol: true }),
    /* birb_synth.c as wasm32, driven through the same exports the AudioWorklet
     * uses. Same source as the native path, different compiler and target —
     * the engine is integer-only so they should agree bit-for-bit.
     * Both tiers: the full build and the size-optimised 4K one, which is the
     * wasm equivalent of smol and carries tighter compile-time limits. */
    'wasm full': (bsb) => {
        if (!wasmAvailable('web/birb.wasm')) buildWasm('web/birb.wasm');
        return renderWasm(bsb, { wasmPath: 'web/birb.wasm' });
    },
    'wasm smol': (bsb) => {
        if (!wasmAvailable('web/birb_smol.wasm')) buildWasm('web/birb_smol.wasm');
        return renderWasm(bsb, { wasmPath: 'web/birb_smol.wasm' });
    },
    'birb_synth.c': (bsb, w) => {
        const raw = join(w, 'c.raw');
        sh(join(ROOT, 'tools/birb_render'), [bsb, raw]);
        return loadRawS16(raw);
    },
    'birbc --js': (bsb, w) => {
        sh(join(ROOT, 'birbc'), [bsb, '-o', join(w, 'lj'), '--js']);
        return new Function(readFileSync(join(w, 'lj.js'), 'utf8') + '; return birb;')()(0).o;
    },
    'birbc locked.c': (bsb, w) => {
        sh(join(ROOT, 'birbc'), [bsb, '-o', join(w, 'lc')]);
        sh('clang', ['-Os', '-std=c11', join(ROOT, 'tools/locked_driver.c'),
                     join(w, 'lc_locked.c'), '-o', join(w, 'lc_bin')]);
        sh(join(w, 'lc_bin'), [join(w, 'lc.raw')]);
        return loadRawS16(join(w, 'lc.raw'));
    },
    /* smol deliberately sounds different — no per-instrument volume, hard clip
     * instead of the limiter — so it will never match the engine. It is here to
     * catch it rendering NOTHING, which is what it did for as long as the mix
     * expression referenced a C.v that smol's trigger never assigns. */
    'birbc --smol': (bsb, w) => {
        sh(join(ROOT, 'birbc'), [bsb, '-o', join(w, 'sj'), '--js', '--smol']);
        return new Function(readFileSync(join(w, 'sj.js'), 'utf8') + '; return birb;')()(0).o;
    },
    'birbc --smol-c': (bsb, w) => {
        sh(join(ROOT, 'birbc'), [bsb, '-o', join(w, 'sc'), '--smol-c']);
        sh('clang', ['-Os', '-std=c11', join(ROOT, 'tools/locked_driver.c'),
                     join(w, 'sc_smol.c'), '-o', join(w, 'sc_bin')]);
        sh(join(w, 'sc_bin'), [join(w, 'sc.raw')]);
        return loadRawS16(join(w, 'sc.raw'));
    },
};

function compare(a, b) {
    const n = Math.min(a.length, b.length);
    let diff = 0, max = 0, sum = 0;
    for (let i = 0; i < n; i++) {
        const e = Math.abs(a[i] - b[i]);
        if (e > 1 / 32768) diff++;
        if (e > max) max = e;
        sum += e * e;
    }
    return {
        lenA: a.length, lenB: b.length,
        pct: n ? 100 * diff / n : 100,
        max, rmse: n ? Math.sqrt(sum / n) : NaN,
        identical: a.length === b.length && diff === 0,
    };
}

mkdirSync(WORK, { recursive: true });
const args = process.argv.slice(2);
const songs = args.filter(a => !a.startsWith('--')).length
    ? args.filter(a => !a.startsWith('--')) : CORPUS;
const names = Object.keys(PATHS);
const summary = {};
const renders = {};   /* keep PCM so the pairings below can be checked */

for (const song of songs) {
    const stem = basename(song).replace(/\.birb$/, '');
    console.log(`\n${song}`);
    let bsb;
    try {
        sh(join(ROOT, 'birbc'), [join(ROOT, song), '-o', join(WORK, stem), '--tracker']);
        bsb = join(WORK, `${stem}.bsb`);
    } catch (e) {
        console.log(`  cannot build: ${e.message.split('\n')[0].slice(0, 70)}`);
        continue;
    }

    const pcm = {};
    for (const name of names) {
        try {
            const r = await PATHS[name](bsb, WORK);   /* wasm path is async */
            if (!r || !r.length) throw new Error(`rendered ${r ? r.length : 0} samples`);
            pcm[name] = r;
            renders[`${stem}|${name}`] = r;
        } catch (e) {
            console.log(`  ${name.padEnd(15)} FAILED — ${e.message.split('\n')[0].slice(0, 50)}`);
        }
    }

    const have = names.filter(n => pcm[n]);
    const ref = have[0];
    if (!ref) continue;
    for (const name of have.slice(1)) {
        const c = compare(pcm[ref], pcm[name]);
        summary[`${stem}|${name}`] = c;
        const tag = c.identical ? 'IDENTICAL'
            : `${c.pct.toFixed(1)}% differ, max ${c.max.toFixed(4)}, rmse ${c.rmse.toFixed(5)}` +
              (c.lenA !== c.lenB ? `, length ${c.lenA} vs ${c.lenB}` : '');
        console.log(`  ${ref} vs ${name.padEnd(15)} ${tag}`);
    }
}

/* ---------- bit-identity gate ----------
 * PITCH.md §8: the Q32 widening must not change a single sample. Cross-class
 * numbers cannot show that — they only compare paths to each other, so a change
 * that shifts every path equally would slip through. This hashes each path's
 * PCM and diffs against a saved baseline.
 *
 *   node tools/parity.mjs --save-baseline    before the change
 *   node tools/parity.mjs                    after; any MOVED line is a failure
 */
{
    const file = join(WORK, '..', 'parity-baseline.json');
    const now = {};
    for (const k of Object.keys(renders)) {
        const r = renders[k], b = Buffer.allocUnsafe(r.length * 2);
        for (let i = 0; i < r.length; i++) {
            let v = Math.round(r[i] * 32768);
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            b.writeInt16LE(v, i * 2);
        }
        now[k] = createHash('sha256').update(b).digest('hex').slice(0, 16);
    }
    if (args.includes('--save-baseline')) {
        writeFileSync(file, JSON.stringify(now, null, 1));
        console.log(`\n\nbaseline saved: ${Object.keys(now).length} renders -> ${file}`);
    } else if (existsSync(file)) {
        const was = JSON.parse(readFileSync(file, 'utf8'));
        const moved = [], gone = [], added = [];
        for (const k of Object.keys(now)) {
            if (!(k in was)) added.push(k);
            else if (was[k] !== now[k]) moved.push(k);
        }
        for (const k of Object.keys(was)) if (!(k in now)) gone.push(k);
        console.log('\n\nbit-identity against saved baseline');
        if (!moved.length) console.log(`  ok — all ${Object.keys(now).length - added.length} renders byte-identical`);
        else { console.log(`  ${moved.length} render(s) MOVED:`); for (const k of moved) console.log(`    ${k}`); }
        if (added.length) console.log(`  ${added.length} new (not in baseline)`);
        if (gone.length) console.log(`  ${gone.length} missing since baseline: ${gone.join(', ')}`);
    } else {
        console.log('\n\nno baseline saved — run with --save-baseline first');
    }
}

/* ---------- equivalence classes ----------
 * The spec: everything in a class must SOUND THE SAME. Two classes.
 *
 * locked — the full-feature output. birbc's default locked C player, the C
 *   engine itself (what --tracker feeds), the editor's own playback engine and
 *   its plain JS export, birbc --js, and the wasm build (locked by default).
 *
 * smol — the traded-down output: no per-instrument volume, no master limiter.
 *   birbc --smol-c, birbc --smol, the wasm smol build, and the editor's export
 *   with the smol toggle on (plus its engine in the same mode).
 *
 * Every member is compared against the first, in LSB of a 16-bit sample. A
 * class is only satisfied when every member reads 0.
 */
const CLASSES = {
    locked: ['editor engine', 'editor export', 'birbc --js', 'birbc locked.c',
             'birb_synth.c', 'wasm full'],
    smol:   ['editor engine smol', 'editor --smol', 'birbc --smol',
             'birbc --smol-c', 'wasm smol'],
};

console.log('\n\nequivalence classes — every member must sound the same');
console.log('(worst deviation from the first member, in LSB of a 16-bit sample)');
for (const [cls, members] of Object.entries(CLASSES)) {
    console.log(`\n  ${cls}:`);
    const ref = members[0];
    for (const m of members) {
        const rows = [];
        for (const song of songs) {
            const stem = basename(song).replace(/\.(birb|bsb)$/, '');
            const a = renders[`${stem}|${ref}`], b = renders[`${stem}|${m}`];
            if (!a || !b) continue;
            rows.push([stem, Math.round(compare(a, b).max * 32768)]);
        }
        if (!rows.length) { console.log(`    ${m.padEnd(20)} not rendered`); continue; }
        const worst = rows.reduce((p, r) => r[1] > p[1] ? r : p);
        const flag = m === ref ? '(reference)' : worst[1] === 0 ? 'ok' : 'DIFFERS';
        console.log(`    ${m.padEnd(20)} ${String(worst[1]).padStart(6)} LSB  ` +
                    `${flag}${worst[1] ? '  worst on ' + worst[0] : ''}`);
    }
}
console.log('');

writeFileSync(join(WORK, 'parity.json'), JSON.stringify(summary, null, 1));
console.log(`\nfull data: ${join(WORK, 'parity.json')}`);
console.log('Note: paths differ in feature set (master bus, limiter), so a ' +
            'non-zero diff\nis not automatically a bug — but a diff that ' +
            'CHANGES across a commit is.');
