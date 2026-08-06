#!/usr/bin/env node
/* editor_export — drive web/editor.html's own JS exporter headlessly.
 *
 *   import { exportSong } from './editor_export.mjs'
 *   const js = exportSong(bsbBytes, { smol: false })
 *
 * This is the path productions actually ship: the editor's export, or a
 * minified/packed form of it. birbc has a SEPARATE JS emitter, and the two can
 * drift — so a parity harness that only covers birbc is not covering the
 * player anyone runs.
 *
 * The editor is one 240KB <script> written against the DOM. Rather than
 * refactor it, the script is evaluated behind a stub just rich enough to let
 * the top-level code finish, then its own parseBin() and buildJS() are called.
 * Nothing here reimplements the editor — if it drifts, this breaks loudly,
 * which is the point.
 */

import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');

/* A DOM stub that answers everything with a chainable inert node. The editor
 * only needs it to survive load; the export path itself reads song state. */
function makeStub() {
    const node = new Proxy(function () {}, {
        get(_t, prop) {
            if (prop === 'value' || prop === 'textContent' || prop === 'innerHTML') return '';
            if (prop === 'checked') return false;
            if (prop === 'style' || prop === 'dataset' || prop === 'classList') return node;
            if (prop === 'children' || prop === 'options') return [];
            if (prop === 'length') return 0;
            if (prop === Symbol.toPrimitive) return () => '';
            return node;
        },
        set() { return true; },
        apply() { return node; },
        has() { return true; },
    });
    return node;
}

export function loadEditor() {
    const html = readFileSync(join(ROOT, 'web/editor.html'), 'utf8');
    const blocks = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]);
    if (!blocks.length) throw new Error('no <script> found in editor.html');
    const src = blocks.join('\n');

    const stub = makeStub();
    const document = new Proxy({}, {
        get(_t, prop) {
            if (prop === 'getElementById' || prop === 'querySelector' ||
                prop === 'createElement') return () => stub;
            if (prop === 'querySelectorAll' || prop === 'getElementsByClassName' ||
                prop === 'getElementsByTagName') return () => [];
            if (prop === 'addEventListener') return () => {};
            if (prop === 'body' || prop === 'documentElement' || prop === 'head') return stub;
            return stub;
        },
    });
    const win = {
        addEventListener() {}, removeEventListener() {},
        requestAnimationFrame() { return 0; }, cancelAnimationFrame() {},
        setTimeout() { return 0; }, setInterval() { return 0; },
        clearTimeout() {}, clearInterval() {},
        localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
        matchMedia: () => ({ matches: false, addListener() {}, addEventListener() {} }),
        AudioContext: function () { return stub; },
        devicePixelRatio: 1, innerWidth: 1280, innerHeight: 800,
        location: { href: '', search: '', hash: '' },
        navigator: { userAgent: 'node', platform: 'node' },
        alert() {}, confirm: () => false, prompt: () => null,
        URL: { createObjectURL: () => '', revokeObjectURL() {} },
        Blob: function () {},
challenge: null,
    };

    /* Browser globals the editor touches while drawing its UI at load. They are
     * all inert here — nothing in the export path draws anything. */
    const browserGlobals = {
        document, window: win, localStorage: win.localStorage,
        alert: win.alert, confirm: win.confirm, prompt: win.prompt,
        requestAnimationFrame: win.requestAnimationFrame,
        cancelAnimationFrame: win.cancelAnimationFrame,
        AudioContext: win.AudioContext, webkitAudioContext: win.AudioContext,
        Blob: win.Blob, URL: win.URL, navigator: win.navigator,
        matchMedia: win.matchMedia, location: win.location,
        getComputedStyle: () => ({ getPropertyValue: () => '', setProperty() {} }),
        Image: function () { return stub; },
        Audio: function () { return stub; },
        fetch: () => Promise.resolve({ ok: false, arrayBuffer: async () => new ArrayBuffer(0) }),
        performance: { now: () => 0 },
        ResizeObserver: function () { return { observe() {}, disconnect() {} }; },
        MutationObserver: function () { return { observe() {}, disconnect() {} }; },
        IntersectionObserver: function () { return { observe() {}, disconnect() {} }; },
        DOMParser: function () { return { parseFromString: () => document }; },
        FileReader: function () { return stub; },
        screen: { width: 1280, height: 800 },
        history: { pushState() {}, replaceState() {} },
        devicePixelRatio: 1,
        OffscreenCanvas: function () { return stub; },
    };

    /* Expose the two entry points we need plus the song state they read. */
    const names = Object.keys(browserGlobals);
    const factory = new Function(
        ...names,
        `${src}
         return {
             get parseBin() { return typeof parseBin === 'function' ? parseBin : null; },
             get buildJS()  { return typeof buildJS  === 'function' ? buildJS  : null; },
             get renderSong() { return typeof renderSong === 'function' ? renderSong : null; },
             get song()     { return typeof song !== 'undefined' ? song : null; },
             set song(v)    { if (typeof song !== 'undefined') song = v; },
             get NCH()      { return typeof NCH !== 'undefined' ? NCH : null; },
             set NCH(v)     { if (typeof NCH !== 'undefined') NCH = v; },
         };`);

    const api = factory(...names.map(n => browserGlobals[n]));

    if (!api.parseBin) throw new Error('editor.html: parseBin() not found');
    if (!api.buildJS) throw new Error('editor.html: buildJS() not found');
    return api;
}

export function exportSong(bytes, { smol = false } = {}) {
    const api = loadEditor();
    api.parseBin(bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes));
    return api.buildJS(smol);
}

/* The editor's own offline engine — the one the parity rule calls the spec.
 * The exporter and the engine are separate code in the same file, so covering
 * the export alone does not cover what the tracker actually sounds like. */
export function renderInEditor(bytes) {
    const api = loadEditor();
    if (!api.renderSong) throw new Error('editor.html: renderSong() not found');
    api.parseBin(bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes));
    const r = api.renderSong(false, -1);
    return r.out;
}

if (import.meta.url === `file://${process.argv[1]}`) {
    const file = process.argv[2];
    if (!file) { console.error('usage: editor_export.mjs song.bsb [--smol]'); process.exit(2); }
    const js = exportSong(readFileSync(file), { smol: process.argv.includes('--smol') });
    process.stdout.write(js);
}
