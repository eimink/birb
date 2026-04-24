# birb — Synthesis Extension Plan

Additive extension to the existing `.bsb` format and engine. Not a version bump — extension stays `.bsb`, older files load unchanged. Filename "birb2_plan" is historical.

## Goals

Extend birb from a 4-channel chiptune synth to a full-featured tracker + synth covering FM, Karplus-Strong, algorithmic drums, and formant filtering — alongside the existing basic waveforms and ADPCM sample support. Every feature is compile-time gated so intro builds can strip down to whatever the song actually uses.

**Status reference:** `SYNTH_BASIC` and `SYNTH_SAMPLE` (ADPCM with `BIRB_NO_SAMPLES` gate) are already shipped. Use the existing `BIRB_NO_SAMPLES` pattern as the template for every new synth type.

## Synth types (all flag-gated)

| Type | Flag (define to exclude) | Status |
|---|---|---|
| `SYNTH_BASIC` | always included (base) | shipped |
| `SYNTH_SAMPLE` | `BIRB_NO_SAMPLES` | shipped |
| `SYNTH_FM` | `BIRB_NO_FM` | new |
| `SYNTH_KS` | `BIRB_NO_KS` | new |
| `SYNTH_DRUM` | `BIRB_NO_DRUM` | new |
| `SYNTH_FORMANT` | `BIRB_NO_FORMANT` | new |

Each flag must exclude: the dispatch arm in `generate_sample()`, the type-specific fields in the channel union, any type-specific tables (sine LUT, formant coefficients, exp LUT if only that type uses it), the format parser arm, and the editor panel in JS export. Add a `static_assert` on `sizeof(birb_channel)` for each flag combination we ship.

Basic synth is always in; everything else is opt-out. A 4K build can ship e.g. basic + drum only (`-DBIRB_NO_SAMPLES -DBIRB_NO_FM -DBIRB_NO_KS -DBIRB_NO_FORMANT`).

## Channel count

`BIRB_NUM_CHANNELS` is configurable at compile time: **min 4, max 16**. The existing flags byte already has a 4-bit channel field, which covers the full range. Loader reads channel count from the flag byte and validates against the compile-time max.

Editor uses a channel bank view: show 4 channels at a time, arrow buttons to page through banks. Grid width stays constant regardless of channel count.

Pattern memory: `patterns[MAX_PATTERNS][MAX_ROWS][NUM_CHANNELS]` scales with the configured channel count. A 16-channel build has 4× the pattern memory of 4-channel, so the 4K WASM target should stick to 4 or 8 channels; 16-channel is for the editor / full builds.

## Channel struct (union-based)

Common state (all types): phase, freq, base_freq, ADSR envelope, all effects (arp, vibrato, porta, pitch slide, retrig, cut, delay), volume, row_vol.

Type-specific state in a union, each arm ifdef'd by its exclusion flag:

- **Basic**: waveform, duty, LFSR (~14 bytes) — always present
- **Sample**: sample_idx, play_pos, pitch_ratio (~8 bytes) — `#ifndef BIRB_NO_SAMPLES`
- **FM**: op_phase[4], op_ratio[4], op_env[4], op_adsr[4], mod_index, feedback, algorithm, prev_out (~80 bytes) — `#ifndef BIRB_NO_FM`
- **KS**: delay buffer, len, pos, damping — `#ifndef BIRB_NO_KS`. Buffer sized via `BIRB_KS_BUF_SIZE` (default 1024, 256 for 4K WASM).
- **Drum**: drum_type, phase2, env_stage2, biquad_state[2], pitch_env_state (~32 bytes) — `#ifndef BIRB_NO_DRUM`
- **Formant**: source waveform state + 3 biquads + sweep state (~100 bytes) — `#ifndef BIRB_NO_FORMANT`

## FM synthesis

2-op default (carrier + modulator), 4-op optional with 8 algorithm topologies stored as 3-bit field. Uses existing `birb_sin_approx()`; fall back to 256-entry sine LUT (512 bytes) if quality insufficient.

```
mod_out = sin(mod_phase) * mod_index * mod_envelope
sample  = sin(carrier_phase + mod_out) * carrier_envelope
```

Instrument binary: 16 bytes (2-op) / 28 bytes (4-op). Fields: type, num_ops, algorithm, feedback, per-op (ratio, level, ADSR), master_volume.

## Karplus-Strong

Delay buffer sized by pitch (44100/freq), filled with noise burst on trigger. Per-sample: read, low-pass filter (weighted average of adjacent samples by damping), write back. ~50 lines of C.

Instrument binary: 8 bytes (type, damping, ADSR, volume).

## Drum synthesis

Six built-in algorithms sharing common primitives (biquad, exp decay LUT, noise LFSR). Purpose-built DSP routines rather than FM-with-noise tricks — 2-4 musical params per drum instead of 20 FM knobs.

Instrument binary: 8 bytes (type, drum_type, tune i8, decay, tone, snap, volume, reserved). drum_type in low 3 bits.

| Drum | Algorithm | Params |
|---|---|---|
| KICK | sine with exponential pitch decay (start→end freq) × AD envelope. TR-808 style. | tune=start freq, decay=amp, tone=pitch rate, snap=click mix |
| SNARE | noise + tuned body pulse through bandpass ~200Hz, aggressive envelope | tune=body freq, decay=noise env, tone=BP cutoff, snap=body/noise mix |
| HAT | 2-op FM at high irrational ratios (1:17.3, mod_index ~4) + highpass | decay=env length, tone=mod index, snap=HP cutoff |
| CLAP | bandpassed noise through 4-stage envelope (3 quick bursts + tail) | decay=tail, tone=BP center, snap=burst spacing |
| TOM | KICK algorithm, different default param range (higher pitch, longer decay) | same as KICK |
| CRASH | HAT algorithm with ~2s decay + slow amp LFO (~8Hz) for shimmer | same as HAT, extended |

Conditional JS export detects which drum types are used; unused algorithms are excluded.

## Formant filter

3 resonant bandpass biquads at vowel formant frequencies. 5 vowel presets (A, E, I, O, U). Sweep between two vowels at configurable speed. Fixed-point biquad, Direct Form II Transposed, int64 intermediates. Coefficient table: 5 vowels × 3 formants = 15 coefficient sets (~300 bytes).

Instrument binary: 12 bytes (type, source_wave, duty, formant_a, formant_b, sweep_speed, resonance, ADSR, volume).

## Shared primitives

- **Fixed-point biquad** — LP/HP/BP modes; used by drum (SNARE bandpass, HAT highpass) and formant
- **Exponential decay LUT** (64 entries, 128 bytes) — KICK pitch decay, optional smoother ADSR stages
- **Sine LUT** (256 entries, 512 bytes) — optional FM upgrade path from `birb_sin_approx()`
- **Noise LFSR** — already in basic synth

Each primitive compiled in only if at least one enabled synth type needs it.

## Format

Extension stays `.bsb`. No magic change, no version bump. All additions are backward-compatible section extensions. Existing flag byte carries channels (4 bits, 1-16) and feature bits; add feature bits for new optional sections as needed. Loaders compiled without a given flag treat sections they don't support as parse-and-skip; songs depending on disabled features are missing those instruments but don't crash.

## JS export: conditional inclusion

Detect synth types used in song; emit minimal engine. Approximate Brotli sizes:

- Basic: ~300 B
- FM 2-op: ~200 B, 4-op: ~350 B
- KS: ~150 B
- Drum full kit: ~350 B, subset (1-3 drums): ~150-250 B
- ADPCM + sample: ~250 B (already shipped)
- Formant: ~300 B

Typical songs: basic + FM + drums ~1.4 KB, drum-only pure-synth ~1.2 KB, all-features ~1.8-2 KB (without samples).

## Editor changes

- **Channel bank pager**: 4 channels visible at a time, arrows to page through up to 16
- **Instrument type dropdown**: Basic / Sample / FM / KS / Drum / Formant (swaps visible controls)
- **FM panel**: algorithm canvas, per-op ratio/level/ADSR, mod index, feedback, 8 preset thumbnails
- **KS panel**: damping slider, noise type
- **Drum panel**: 6-button drum type radio, 4 knobs (tune/decay/tone/snap), preview, preset library (808 Kick, 909 Kick, Snare, Rim, Closed/Open Hat, Clap, Crash, Ride — ~20 B each)
- **Formant panel**: source wave, vowel A/B dropdowns, sweep speed, resonance, filter viz
- Existing Sample panel stays unchanged

## Files

| File | Action |
|---|---|
| `birb_synth.h` | Tagged channel union, configurable `BIRB_NUM_CHANNELS` (4-16), new synth types behind flags |
| `birb_synth.c` | New generators: `generate_fm`, `generate_ks`, `generate_drum`, `generate_formant`. Dispatch on synth_type. |
| `birb_synth_mini.c` | Mirror changes for 4K WASM build |
| `birb_format.h` | Parse new instrument types, validate channel count against compile-time max |
| `birbc.c` | Export new synth types, conditional JS emission per used types |
| `web/editor.html` | Channel pager, new instrument panels, preset libraries |
| `Makefile` | Build matrix: size-tier targets with various flag combinations |
| `midi2birb.py` | GM drum mapping (36=kick, 38/40=snare, 42/46=hat, 49=crash, 51=ride, 39=clap) → SYNTH_DRUM |

All changes extend existing files — no parallel `birb_synth2.*`.

## Phases

### Phase 1: Channel count + union refactor
Make `BIRB_NUM_CHANNELS` configurable 4-16. Refactor channel struct to tagged union (SYNTH_BASIC and SYNTH_SAMPLE only initially). Loader reads channel count from flag byte. Editor: channel pager. Verify: existing songs load and sound identical at 4, 8, and 16 channels.

### Phase 2: FM synthesis (`BIRB_NO_FM` gate)
`generate_fm()` 2-op then 4-op. FM instrument format. Editor FM panel. Conditional JS export. Verify: FM bass/bell/organ, Brotli size deltas.

### Phase 3: Karplus-Strong (`BIRB_NO_KS` gate)
`generate_ks()` + noise burst trigger. Editor KS panel. Verify: plucked strings, metallic hits.

### Phase 4: Drum synthesis (`BIRB_NO_DRUM` gate)
Shared primitives (biquad, exp LUT). `generate_drum()` dispatch. KICK first (validates pitch-env + exp-LUT infrastructure), then SNARE, HAT, CLAP, TOM, CRASH. Editor drum panel with preset library. Verify: full kit, A/B against 808 samples, drum-only song hits ~1.2 KB Brotli target.

### Phase 5: Formant filter (`BIRB_NO_FORMANT` gate)
Fixed-point biquad (shared with drum). Vowel coefficient table. `generate_formant()`. Sweep interpolation. Editor formant panel. Verify: vowel pads, A→O sweep.

### Phase 6: Polish
Verify every flag combination compiles and passes `sizeof(birb_channel)` assertions. Size-tier build matrix in Makefile (minimal 4K, standard, full). Brotli baselines across representative songs. README + INTEGRATION.md updates.

## Size targets

- Basic-only: ~1.2 KB Brotli
- Basic + FM: ~1.5 KB
- Drum-only pure-synth: ~1.2 KB (no sample floor)
- All features, no samples: ~1.8 KB
- All features + samples: ~2-2.5 KB + sample data
- WASM engine: < 8 KB raw / < 4 KB Brotli (no samples); < 9 KB raw with samples

## Cider integration

Cider 16K/64K intro builds default to `-DBIRB_NO_SAMPLES` plus whatever other synth types the song doesn't use. Drum synth covers kit sounds with predictable size. Songs that need a specific sample can opt back in per-build.
