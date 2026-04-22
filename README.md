# birb

Minimal chiptune synth engine and tracker for demoscene productions.

4-channel software synthesizer with tracker-style composition, designed to embed into size-coded demos. The engine + song data compresses to ~1-1.5KB under Brotli for 4K web intros.

## Components

- **birb synth** — C engine, no stdlib, no malloc, no floats, fixed-point 16.16. Compiles to native and WASM.
- **birb tracker** — web-based tracker/editor, single HTML file. Runs locally or on any static host.
- **birbc** — compiler that converts `.birb` text or `.bsb` binary songs to `.bsb`, C headers, or self-contained JS for 4K demos.
- **midi2birb** — Python script to convert MIDI files to `.bsb`.

## Synth features

- 5 waveforms: pulse (4 duty cycles), triangle, sawtooth, noise (LFSR), sine
- **IMA-ADPCM sample playback** with pitch shifting and loop points
- ADSR envelope per instrument + per-instrument volume
- Per-row volume column (0 = hold, 01-FF = level)
- Pitch envelope (for kick drums, sweeps)
- Named instruments

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
- Drag-and-drop pattern sequencer with freely reorderable order list
- Copy / cut / paste (Ctrl+C/X/V) with row-range selection (Shift+arrows)
- Per-channel mute toggles
- **Live parameter tweaking** during playback — drag ADSR/volume sliders while the song plays and hear changes within ~50ms (AudioWorklet)
- WAV upload with automatic stereo→mono downmix, resample to 44100Hz, and IMA-ADPCM encoding
- Sample library shared across instruments (same sample can power multiple instruments with different ADSRs)
- Visual ADSR envelope preview, waveform oscilloscope for samples
- Song persistence (localStorage)
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

Requires clang. For WASM targets: `brew install llvm lld`. For minification: `npm install -g terser`.

```bash
make all          # native tools (birb_wav, birbc, birb_play, birb_play_bin)
make web          # WASM + web assets (requires llvm+lld)
make editor-dist  # produces web/editor.min.html (requires terser)
make serve        # local web server on :8080
```

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
- `.bsb` — compact binary: BRB1 magic, planar pattern layout for Brotli friendliness, optional SMPL (sample bank) and NAME (instrument names) sections
- `.js` — self-contained JS with synth engine + song data (4K demo embed)
- `.min.js` — same, minified and token-packed
- `.h` — C header with embedded binary data

## Example songs

- `examples/sundstrom.bsb` — reference test song
- `examples/dnb_demo.birb` — text-format D&B example
- `examples/kick.wav`, `snare.wav`, `hat.wav` — test WAVs for sample testing (regenerate with `python3 examples/gen_test_wav.py`)

## License

(C) 2026 eimink / Wide Load ^ KVG. All rights reserved.
