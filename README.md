# birb

Minimal chiptune synth engine and tracker for demoscene productions.

Software synthesizer with tracker-style composition — 4 channels by default, up to 16 — designed to embed into size-coded demos. A plain-oscillator engine + song data compresses to ~1-1.5KB under Brotli for 4K web intros.

## Components

- **birb synth** — C engine, no stdlib, no malloc, no libm. Fixed-point 16.16 integer core; the FM feedback loop and the reverb bus use hardware floating point in the full engine, and both compile out (`-DBIRB_NO_FM` / `-DBIRB_NO_REVERB`) for an integer-only build. Compiles to native and WASM.
- **birb tracker** — web-based tracker/editor, single HTML file. Runs locally or on any static host.
- **birbc** — compiler that converts `.birb` text or `.bsb` binary songs to `.bsb`, C headers, or self-contained JS for 4K demos.
- **midi2birb** — Python script to convert MIDI files to `.bsb`.

## Synth features

Ten voice types across six engines, chosen per instrument:

- **Oscillators** — pulse (12.5 / 25 / 50 / 75% duty), triangle, sawtooth, LFSR noise, sine (5th-order minimax)
- **FM** — 2-operator (carrier + modulator) and 4-operator with 8 algorithms; per-op ratio / level / ADSR, global feedback and mod index. Presets: Bass, Bell, Organ, Brass (2-op), Violin, BRAAAAM (4-op)
- **Karplus-Strong** — plucked/struck string, one damping control (low = ringy, high = muted click)
- **Algorithmic drums** — Kick / Snare / Hat / Clap / Tom / Crash; Tune / Decay / Tone / Snap re-purpose per algorithm. Preset kit (808/909 kicks, snare, rim, closed/open hat, clap, crash, ride)
- **Formant / vowel** — source wave (pulse/saw/noise) through 3 parallel bandpass biquads at vowel formant frequencies; pick vowel A and B (A/E/I/O/U) and a sweep speed to morph A↔B, resonance steers Q
- **Samples** — IMA-ADPCM WAV playback with pitch shifting and loop points

Per-instrument: ADSR envelope + volume, pitch envelope (kick sweeps / drops), a baked arpeggio pair, reverb send (0-255), and a name.

Global:

- **Reverb send bus** — Schroeder-lite (4 damped combs + 2 allpass) with size / damping / wet-mix; per-instrument sends feed it
- **Master soft-saturation** (tanh) — gentle limiting, no hard clip even when the reverb is drenched
- **Per-row volume column** (`00` = hold previous, `01`–`FF` = level)

### Effects (FT2-inspired)

| Effect | Param | Name |
|---|---|---|
| `1XY` | | Arpeggio — cycles base, +X, +Y semitones per tick |
| `2XX` | | Pitch up — slide pitch up at speed XX |
| `3XX` | | Pitch down — slide pitch down at speed XX |
| `4XY` | | Vibrato — X = speed, Y = depth |
| `5XX` | | Tone portamento — slide to target note |
| `6XX` | | Retrigger — re-trigger note every XX ticks |
| `7CX` | | Note cut — kill note after X ticks |
| `7DX` | | Note delay — trigger note after X ticks |
| `8XY` | | Tremolo — volume LFO |
| `9XX` | | Sample offset — start sample at XX × 256 samples |
| `BXX` | | Position jump — jump to order position XX |
| `DXX` | | Pattern break — advance to next order, start at row XX |
| `FXX` | | Set speed/BPM — XX < 0x20 sets TPR, ≥ 0x20 sets BPM |

## Tracker features

- Pattern grid with keyboard-driven note entry (tracker piano layout)
- Per-instrument editor for every voice type: FM operator matrix (2/4-op, algorithm, feedback, mod index), drum Tune/Decay/Tone/Snap, formant vowels + sweep + resonance, KS damping, sample loop points
- Global reverb panel (size / damping / wet) with per-instrument send sliders
- Drag-and-drop pattern sequencer with a freely reorderable order list; the playhead tracks position correctly through repeated patterns
- Copy / cut / paste (Ctrl+C/X/V) with row-range selection (Shift+arrows)
- Per-channel mute toggles; configurable channel count
- **Live parameter tweaking** during playback — drag any synth/envelope slider while the song plays and hear it within ~50ms (AudioWorklet); optional record mode where the cursor follows the playhead
- WAV upload with automatic stereo→mono downmix, resample to 44100Hz, and IMA-ADPCM encoding; sample library shared across instruments
- Visual ADSR envelope preview, waveform oscilloscope for samples
- Autosaving song persistence (localStorage) — loading a song or editing it persists automatically; no manual save needed to survive a reload
- Export: `.bsb`, `.js`, `.min.js` (packed + estimated Brotli size)
- Import: `.bsb`, `.bin`, `.birb`

## Targets

| Target | Size | Method |
|---|---|---|
| 4K web intro | ~1-1.5 KB Brotli | `birbc song.bsb --js` or tracker "Export .js" — self-contained function |
| Larger web demo | ~3 KB gzip | WASM engine + AudioWorklet (`web/birb.wasm` + `web/birb_processor.js`) |
| Native macOS | ~34 KB binary | CoreAudio playback via `birb_play` |
| Native (any platform) | — | Include `birb_synth.c` + `birb_format.h` + your song header, call `birb_render()` in your audio callback |

## Build

Requires clang. For WASM targets: `brew install llvm lld`. Editor minification and Brotli need `python3` and `brotli` (no npm/terser).

```bash
make all          # native tools (birb_wav, birbc, birb_play, birb_play_bin)
make web          # WASM + web assets (requires llvm+lld)
make editor-dist  # web/editor.min.html + .min.html.br (minify_editor.py + brotli)
make serve        # local web server on :8080
```

### Feature strips (size-coded builds)

Each synth engine compiles out independently, so a build carries only what a song uses:

`-DBIRB_NO_FM` · `-DBIRB_NO_KS` · `-DBIRB_NO_DRUM` · `-DBIRB_NO_FORMANT` · `-DBIRB_NO_SAMPLES` · `-DBIRB_NO_REVERB`

Reverb and FM are the float-bearing parts; strip both for an integer-only engine. The pre-wired 4K WASM variants (`web/birb4k_*.wasm`) use these; a `.bsb` self-declares which sections it needs so `birbc`/the exporter emit only the code in play.

## Usage

### Compose

Open `web/editor.html` (or the hosted version) in a browser. No server needed — everything runs client-side.

### Import a MIDI file

```bash
python3 midi2birb.py song.mid
# produces song.bsb — load it in the tracker to tune instruments
```

### Export for demos

```bash
# From the tracker:
#   Save .bsb        → binary song for distribution/conversion
#   Export .js       → self-contained function (readable)
#   Export .min.js   → minified + packed (smallest, with Brotli size estimate)

# Alternatively, use birbc on a .bsb or .birb:
./birbc song.bsb --js     # → song.js for web embedding
./birbc song.bsb          # → song.h for C embedding
```

### Integrate (WebGL 4K)

```js
// Include the generated song.js, then:
var ctx = new AudioContext({sampleRate: 44100});
var m = birb(ctx);
var ab = ctx.createBuffer(1, m.T, 44100);
ab.getChannelData(0).set(m.o);
var s = ctx.createBufferSource();
s.buffer = ab;
s.connect(ctx.destination);
s.loop = true;
s.start();
// Sync: m.spt = samples per tick; derive beat from ctx.currentTime
```

### Integrate (Native C)

```c
#include "birb_synth.h"
#include "birb_format.h"
#include "song.h"

birb_song song;
birb_state state;
birb_load(&song, birb_song_data, BIRB_SONG_DATA_SIZE);
birb_init(&state, &song);
// In audio callback:
birb_render(&state, buffer, num_samples);
```

See `INTEGRATION.md` for full integration details.

## File formats

- `.birb` — human-readable text format (for birbc input, tracker import)
- `.bsb` — compact binary: BRB1 magic, planar pattern layout for Brotli friendliness, plus optional sections for extended instrument data (FM / Karplus-Strong / drum / formant), the reverb bus (REVB), the sample bank (SMPL), and instrument names (NAME). Absent sections default off, so a plain oscillator song stays tiny.
- `.js` — self-contained JS with synth engine + song data (4K demo embed)
- `.min.js` — same, minified and token-packed
- `.h` — C header with embedded binary data

## Example songs

- `examples/sundstrom.bsb` — reference test song
- `examples/drums_test.birb` / `.bsb` — algorithmic drum kit
- `examples/ks_test.birb` / `.bsb` — Karplus-Strong plucks
- `examples/formant_test.birb`, `formant_simple.birb` — vowel / formant voices
- `examples/kick.wav`, `snare.wav`, `hat.wav` — test WAVs for sample loading (regenerate with `python3 examples/gen_test_wav.py`)

## License

(C) 2026 eimink / Wide Load ^ KVG. All rights reserved.
