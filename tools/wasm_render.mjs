/* wasm_render — render a .bsb through birb_synth.c compiled to wasm32.
 *
 * Same engine source as the native path, different compiler and target. The
 * engine is integer-only so the two SHOULD agree bit-for-bit; this exists to
 * check that rather than assume it. Drives the same exports the AudioWorklet
 * uses (web/birb_processor.js): getSongBuf / init / render / getOutputBuf.
 */

import { readFileSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const QUANTUM = 128;   /* what the worklet asks for; keep the same call shape */

export function wasmAvailable(wasmPath = 'web/birb.wasm') {
    return existsSync(join(ROOT, wasmPath));
}

export function buildWasm(target = 'web/birb.wasm') {
    execFileSync('make', [target], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
}

export async function renderWasm(bsbPath, { wasmPath = 'web/birb.wasm', samples } = {}) {
    const bytes = readFileSync(wasmPath.startsWith('/') ? wasmPath : join(ROOT, wasmPath));
    const { instance } = await WebAssembly.instantiate(bytes, {});
    const w = instance.exports;
    const mem = () => new Uint8Array(w.memory.buffer);

    const song = new Uint8Array(readFileSync(bsbPath));
    mem().set(song, w.getSongBuf());
    if (w.init(song.byteLength) !== 0) {
        /* A wasm tier is a compile-time feature/limit set, so a refusal is a
         * build-config mismatch, not an engine divergence. Say which shape of
         * song it was so the two are not confused in the report. */
        const nch = 4 + 2 * (song[5] >> 5);
        let p = 8; const ol = song[p++]; p += ol * nch; p += song[6] * 12;
        let maxRows = 0;
        for (let i = 0; i < song[7]; i++) if (song[p + i] > maxRows) maxRows = song[p + i];
        throw new Error(`${wasmPath} build cannot hold this song ` +
            `(${nch}ch, ${song[7]} patterns, ${maxRows} rows, order ${ol})`);
    }

    const outPtr = w.getOutputBuf();
    /* The wasm build carries 4K-tuned limits (BIRB_MAX_ROWS etc.), so it can
     * legitimately refuse songs the native build accepts — that is a build
     * config difference, not a divergence, and the caller should say so. */
    const total = samples ?? guessLength(song);
    const out = new Float64Array(total);
    for (let done = 0; done < total; ) {
        const n = Math.min(QUANTUM, total - done);
        w.render(n);
        const s = new Int16Array(w.memory.buffer, outPtr, n);
        for (let i = 0; i < n; i++) out[done + i] = s[i] / 32768;
        done += n;
    }
    return out;
}

/* Mirror birb_render.c's accounting so both paths render the same span. */
function guessLength(d) {
    const bpm = d[4] || 125, tpr = (d[5] & 0x1F) || 6, np = d[7];
    const nch = 4 + 2 * (d[5] >> 5);
    let p = 8;
    const ol = d[p++];
    p += ol * nch;
    p += d[6] * 12;
    let maxRows = 1;
    for (let i = 0; i < np; i++) { const n = d[p + i]; if (n > maxRows) maxRows = n; }
    const spt = Math.floor(44100 * 5 / (bpm * 2));
    return ol * maxRows * tpr * spt;
}
