# birb

Minimal chiptune synth engine and tracker for demoscene productions.

4-channel software synthesizer with tracker-style composition, designed to embed into size-coded demos. The engine + song data compresses to ~1KB under Brotli for 4K web intros.

## Components

- **birb synth** — C engine, no stdlib, no floats, fixed-point 16.16. Compiles to native and WASM.
- **birb tracker** — web-based tracker/editor for composing songs. Single HTML file, runs anywhere.
- **birbc** — compiler that converts songs to binary (.bsb), C headers, or self-contained JS for 4K demos.

## Synth features

- 4 channels (pulse/triangle/sawtooth/noise/sine)
- Variable pulse duty cycle (12.5%, 25%, 50%, 75%)
- ADSR envelope per instrument
- Per-instrument volume
- Per-row volume column with hold
- Pitch envelope (for kick drums, sweeps)
- Arpeggio, pitch slide, volume slide effects
- LFSR noise (snare, hi-hat)

## Targets

| Target | Size | Method |
|---|---|---|
| WebGL 4K demo | ~1KB Brotli | `birbc song.bsb --js` — single JS function |
| Larger web demo | ~3KB gzip | WASM engine + AudioWorklet |
| Native macOS | ~34KB binary | CoreAudio playback |

## Build

Requires clang. For WASM targets: `brew install llvm lld`. For minification: `npm install -g terser`.

```bash
make all          # native tools (birb_wav, birbc, birb_play)
make web          # WASM + web assets
make editor-dist  # minified editor
make serve        # local web server on :8080
```

## Usage

### Compose

Open `web/editor.html` in a browser. No server needed.

### Export for demos

```bash
# From editor: Export .bsb

# For WebGL 4K demos (JS, ~1KB Brotli):
./birbc song.bsb --js

# For native demos (C header):
./birbc song.bsb
# produces song.h with embedded byte array

# For WASM demos:
make web
# use web/birb.wasm + web/birb_processor.js
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
// Sync: m.spt = samples per tick
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

## File formats

- `.birb` — human-readable text format (for birbc input)
- `.bsb` — compact binary (editor save/load, birbc input/output)
- `.js` — self-contained JS with engine + song data (4K demo embed)
- `.h` — C header with embedded binary data

## License

(C) 2026 eimink / Wide Load ^ KVG. All rights reserved.
