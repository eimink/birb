# Integrating birb into your demo

birb is a multi-channel chiptune synth (4 channels by default, up to 16). The musician composes in birb tracker and exports song data. You integrate the player into your demo engine.

## What you get from the musician

- **`.bsb` file** — binary song data (for native C demos or further conversion)
- **`.js` file** — self-contained JS with synth engine + song data (for web demos)
- **`.min.js` file** — same as above, minified and packed (for 4K intros)

## Web demo integration

### Quick start

Include the exported `.js` file in your HTML. It defines a single function called `birb()`.

```html
<script src="song.js"></script>
<script>
// Create AudioContext (you probably already have one for WebGL)
var ctx = new AudioContext({sampleRate: 44100});

// Render the song — this blocks briefly while it synthesizes offline
var m = birb(ctx);

// Create an AudioBuffer from the rendered samples
var buf = ctx.createBuffer(1, m.T, 44100);
buf.getChannelData(0).set(m.o);

// Play it
var src = ctx.createBufferSource();
src.buffer = buf;
src.connect(ctx.destination);
src.loop = true;
src.start();
</script>
```

That's it. The song plays and loops.

### What birb() returns

```js
var m = birb(ctx);
// m.o   — Float32Array of rendered mono samples
// m.T   — total number of samples
// m.spt — samples per tick (for sync)
```

### Sharing the AudioContext

If your demo already creates an `AudioContext` (e.g. for WebGL audio), pass it to birb:

```js
var ctx = new AudioContext({sampleRate: 44100});
var m = birb(ctx);  // uses your context, doesn't create a new one
```

birb requires a sample rate of 44100 Hz.

### Visual sync

To sync visuals to the music, use `m.spt` (samples per tick) and `ctx.currentTime`:

```js
var startTime = ctx.currentTime;

function render() {
    // Elapsed samples since playback started
    var elapsed = ((ctx.currentTime - startTime) * 44100) | 0;

    // Current position in the song (wraps on loop)
    var pos = elapsed % m.T;

    // Current tick number
    var tick = (pos / m.spt) | 0;

    // Use tick for syncing effects, transitions, etc.
    // Higher tick = further into the song
    // One tick ≈ one 16th note step (depends on BPM and TPR)

    requestAnimationFrame(render);
}
render();
```

### For 4K intros

Use the `.min.js` export. The export dialog shows the estimated Brotli size.

In your 4K build pipeline:
1. Concatenate `song.min.js` with your demo code
2. Run through your minifier (terser, uglify, etc.)
3. Brotli compress the final bundle

The birb engine + song data typically adds ~1-1.5 KB to your Brotli'd bundle. The exact overhead depends on song complexity and how much compression context is shared with your demo code.

Tip: birb's JS code shares common patterns with typical WebGL demo code (math operations, typed arrays, loops), so Brotli's dictionary window compresses them together. The marginal cost of birb in your bundle is less than its standalone compressed size.

### Render timing

`birb()` renders the entire song offline when called. For a typical 2-minute song at 44100 Hz, this takes 50-200ms depending on the device. Call it during your loading phase, not during the intro animation.

```js
// During loading screen
var m = birb(ctx);
var audioBuf = ctx.createBuffer(1, m.T, 44100);
audioBuf.getChannelData(0).set(m.o);

// When ready to start the demo
var src = ctx.createBufferSource();
src.buffer = audioBuf;
src.connect(ctx.destination);
src.loop = true;
src.start();
startTime = ctx.currentTime;
```

## Native C demo integration

### Files you need

From the birb project:
- `birb_synth.h` — header (types, API)
- `birb_synth.c` — synth engine
- `birb_format.h` — binary format loader

From the musician:
- `song.h` — generated C header with embedded song data (run `birbc song.bsb` to produce this)

### Compile

```bash
# Add to your build alongside your demo source
clang -Os birb_synth.c your_demo.c -framework AudioToolbox -framework CoreFoundation -o demo
```

For a size-coded native build, compile out the synth engines the song doesn't use — each is independent:

```
-DBIRB_NO_FM  -DBIRB_NO_KS  -DBIRB_NO_DRUM  -DBIRB_NO_FORMANT  -DBIRB_NO_SAMPLES  -DBIRB_NO_REVERB
```

Reverb and FM are the only floating-point parts; dropping both keeps the engine integer-only. Override the channel count with `-DBIRB_NUM_CHANNELS=N` (4–16). As a rough guide (arm64, `-Oz`), the oscillator-only engine is ~3 KB of code and the full engine ~8 KB; each optional voice adds 0.3–1.5 KB.

### Code

```c
#include "birb_synth.h"
#include "birb_format.h"
#include "song.h"  // contains birb_song_data[] and BIRB_SONG_DATA_SIZE

// Load and init
static birb_song song;
static birb_state state;

birb_load(&song, birb_song_data, BIRB_SONG_DATA_SIZE);
birb_init(&state, &song);

// In your audio callback (CoreAudio, ALSA, WASAPI, etc.)
void audio_callback(int16_t *buffer, int num_samples) {
    birb_render(&state, buffer, num_samples);
}
```

### Audio output format

- Mono, 16-bit signed integer (int16_t)
- 44100 Hz sample rate
- Buffer any size (typically 1024 or 2048 samples per callback)

### Sync

```c
int current_row = birb_get_row(&state);
int current_pattern = birb_get_pattern(&state);
```

Call these after `birb_render()` to get the current playback position.

### macOS CoreAudio example

See `birb_macos.c` in the project for a complete working example (~60 lines).

## WASM integration (larger web demos)

For demos where you want real-time streaming audio (not offline render):

1. Build the WASM module: `make web/birb.wasm`
2. Use `web/birb_processor.js` as the AudioWorklet
3. Load the `.bsb` binary and pass it to the worklet

See `web/index.html` for the full integration example. This approach uses more bandwidth (~5 KB total) but gives you streaming audio and lower latency.

## Song format notes

- The `.bsb` binary format is the interchange format between tracker and tools
- birbc can convert `.bsb` → `.js` (web), `.h` (C header)
- The tracker can load and save `.bsb` files directly
- MIDI files can be converted with `python3 midi2birb.py song.mid`

## Troubleshooting

**No sound**: Make sure `AudioContext` is created after a user gesture (click/keypress). Browsers block audio before interaction.

**Clicks/pops at loop point**: The song length should be exact. If you hear a glitch, the `.js` was exported from an older version — re-export from the tracker.

**Song too quiet/loud**: The musician controls the mix via per-instrument volume and the pattern volume column. Ask them to adjust the balance in the tracker.

**Wrong tempo**: birb uses tracker-style timing where BPM and ticks-per-row (TPR) together determine speed. The musician sets these in the tracker.
