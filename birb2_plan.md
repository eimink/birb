# birb2 — Advanced Synthesis Extension

## Context

birb1 is a 4-channel chiptune synth with basic waveforms (pulse, tri, saw, noise, sine). It works well for classic chiptune but can't compete with tools like 4klang for expressive demoscene music. birb2 extends the engine with FM synthesis, Karplus-Strong strings, ADPCM sample playback, and formant filtering — while keeping the demoscene size targets viable.

## Architecture

### Synthesis Types (tagged instrument)

Each instrument declares its synthesis type. The channel uses a union for type-specific state:

```
SYNTH_BASIC   = 0   birb1-compatible waveforms
SYNTH_FM      = 1   2-op or 4-op FM (DX7-style)
SYNTH_KS      = 2   Karplus-Strong plucked strings
SYNTH_SAMPLE  = 3   ADPCM sample playback
SYNTH_FORMANT = 4   formant-filtered waveform
```

The `generate_sample()` function dispatches on synth_type. Unused types are ifdef'd out for 4K builds.

### 8 Channels

`BIRB_NUM_CHANNELS` goes from 4 to 8. Order table, pattern data, and mixer loop all widen. Editor uses a channel bank view (Ch 1-4 / Ch 5-8 toggle) to keep the grid readable.

### Channel Struct (union-based)

Common state shared by all types: phase, freq, base_freq, ADSR envelope, effects (arp, vibrato, porta, pitch slide, retrig, cut, delay), volume, row_vol.

Type-specific state in a union:
- **Basic**: waveform, duty, LFSR (~14 bytes)
- **FM**: op_phase[4], op_ratio[4], op_env[4], op_adsr[4], mod_index, feedback, algorithm, prev_out (~80 bytes for 4-op)
- **KS**: delay buffer[1024], len, pos, damping (~2052 bytes, compile-time configurable)
- **Sample**: sample_idx, play_pos, pitch_ratio (~8 bytes)
- **Formant**: source waveform + LFSR + 3 biquad filters + sweep state (~100 bytes)

Channel size dominated by KS buffer. Set `BIRB_KS_BUF_SIZE=1024` for native, `256` for 4K WASM. 8 channels × ~2100 bytes = ~16.8 KB total (fits in WASM 512 KB).

### FM Synthesis

**2-op** (default): carrier + modulator. Modulator output offsets carrier phase. ~6-8 fixed-point multiplies per sample.

```
mod_out = sin(mod_phase) * mod_index * mod_envelope
sample = sin(carrier_phase + mod_out) * carrier_envelope
```

**4-op**: 4 operators with 8 algorithm topologies (covering the most useful DX7 configurations: all-series, all-parallel, Y-split, etc.). Algorithm stored as 3-bit field.

Uses existing `birb_sin_approx()`. If quality insufficient for deep modulation, fall back to a 256-entry sine LUT (512 bytes).

**Instrument binary** (2-op: 16 bytes, 4-op: 28 bytes):
```
type(1) num_ops(1) algorithm(1) feedback(1)
per-op: ratio(1) level(1) A(1) D(1) S(1) R(1)
master_volume(1)
```

### Karplus-Strong

Delay buffer sized by pitch (44100/freq). Filled with noise burst on trigger. Per-sample: read from buffer, low-pass filter (average adjacent samples weighted by damping), write back.

~50 lines of C. Excellent plucks, guitars, metallic percussion.

**Instrument binary** (8 bytes): type, damping, ADSR, volume.

### ADPCM Samples

IMA-ADPCM at 4 bits/sample. Decoder is ~30 lines + step table (89 entries, 178 bytes). User uploads WAV in editor, encoded to ADPCM client-side.

Embedded in BRB2 format as optional `SMPL` section — zero cost when not used.

**Instrument binary** (8 bytes): type, sample_idx, volume, ADSR.

### Formant Filter

3 resonant bandpass biquads at vowel formant frequencies. 5 presets (A, E, I, O, U). Sweep between two vowels at configurable speed.

Fixed-point biquad using Direct Form II Transposed with int64 intermediates for stability.

Coefficients precomputed for 5 vowels × 3 formants = 15 coefficient sets (300 bytes table).

**Instrument binary** (12 bytes): type, source_wave, duty, formant_a, formant_b, sweep_speed, resonance, ADSR, volume.

### Binary Format: BRB2

```
Header (8 bytes):   'B' 'R' 'B' '2' bpm tpr num_inst num_pat
Flags (1 byte):     channels(lo nibble) features(hi nibble, bit0=has samples)
Order:              order_len + [8 channels per position]
Instruments:        type byte + type-specific data (variable length)
Patterns:           5 planes × 8 channels × rows (planar, same as BRB1)
Optional SMPL:      'S' 'M' 'P' 'L' + sample bank
Optional NAME:      'N' 'A' 'M' 'E' + instrument names
```

BRB1 files load into channels 0-3 with SYNTH_BASIC. Full backward compat.

### JS Export: Conditional Inclusion

Detect which synth types the song actually uses. Only emit code for those types:

- Basic waveforms: ~300 bytes minified
- FM 2-op: ~200 bytes, 4-op: ~350 bytes
- KS: ~150 bytes
- ADPCM + sample: ~250 bytes
- Formant: ~300 bytes

Typical song (basic + FM + KS): ~1.35 KB Brotli. All features: ~1.8-2 KB Brotli.

### Editor Changes

- **Channel bank**: toggle Ch 1-4 / Ch 5-8 (keeps 4-channel grid width)
- **Instrument type selector**: dropdown above wave selector, swaps visible controls
- **FM panel**: algorithm visualization canvas (boxes + routing lines), per-operator ratio/level/ADSR, mod index, feedback. 8 algorithm presets as clickable thumbnails.
- **KS panel**: damping slider, noise type
- **Sample panel**: WAV upload, waveform canvas, draggable loop markers, base pitch selector
- **Formant panel**: source wave, vowel A/B dropdowns, sweep speed, resonance, filter response visualization

## Files to Modify/Create

| File | Action |
|---|---|
| `birb_synth2.h` | New header with tagged channel union, 8-ch, new types |
| `birb_synth2.c` | New engine with FM/KS/sample/formant generators |
| `birb_format2.h` | BRB2 format loader + BRB1 compat shim |
| `birb_synth.h/c` | Keep as-is for birb1 backward compat |
| `birbc.c` | Add BRB2 support, conditional JS export |
| `web/editor.html` | 8-ch grid, instrument type panels, FM viz, sample upload |
| `birb_4k2.c` | Size-optimized birb2 with conditional compilation |
| `Makefile` | New build targets for birb2 |

## Implementation Phases

### Phase 1: Foundation (8 channels + format + refactor)
- `birb_synth2.h` with tagged union channel struct, 8 channels
- `birb_format2.h` with BRB2 loader + BRB1 upgrade path
- `birb_synth2.c` with dispatch to SYNTH_BASIC (identical to birb1)
- Editor: 8-ch grid with bank selector, BRB2 export, updated inline synth
- birbc: BRB2 format support
- **Verify**: birb1 song sounds identical through birb2 engine

### Phase 2: FM Synthesis
- `generate_fm()` — 2-op first, then 4-op with algorithms
- FM instrument format in BRB2
- Editor FM panel with algorithm canvas
- FM code in editor inline synth + JS export (conditional)
- **Verify**: create FM bass, bell, organ sounds; export; compare Brotli size

### Phase 3: Karplus-Strong
- `generate_ks()` — delay buffer + LP filter
- KS trigger (noise burst fill)
- Editor KS panel
- KS in inline synth + JS export
- **Verify**: plucked strings, metallic hits

### Phase 4: ADPCM Samples
- IMA-ADPCM decoder
- SMPL section in BRB2 format
- `generate_sample_playback()` with pitch shifting + loop
- Editor sample upload UI (WAV→ADPCM in JS)
- Waveform display + loop markers
- **Verify**: drum kit, export with embedded samples

### Phase 5: Formant Filter
- Fixed-point biquad implementation
- Formant coefficient table (5 vowels × 3 filters)
- `generate_formant()` — source wave through 3 biquads
- Formant sweep interpolation
- Editor formant panel
- **Verify**: vowel pads, sweep between A→O

### Phase 6: Polish
- 4K WASM size optimization (`birb_4k2.c` with ifdef'd modules)
- Brotli size testing across representative songs
- Editor UX polish (FM presets, formant library)
- Update README, INTEGRATION.md, midi2birb.py for birb2

## Verification

Each phase: compile C engine (native + WASM), test in editor, export JS, measure Brotli. Reference test song: `examples/sundstrom.bsb` (upgraded to BRB2 on phase 1).

Size targets:
- Basic-only song: ~1.2 KB Brotli (same as birb1)
- Basic + FM: ~1.5 KB Brotli
- Full feature song: ~2-2.5 KB Brotli
- WASM engine: < 8 KB raw, < 4 KB Brotli
