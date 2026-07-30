CC = clang
CFLAGS = -Os -Wall -Wextra -std=c11

# Homebrew LLVM for wasm32 target (Xcode clang lacks wasm backend)
WASM_CC = $(shell \
    if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; \
    elif [ -x /usr/local/opt/llvm/bin/clang ]; then echo /usr/local/opt/llvm/bin/clang; \
    else echo ""; fi)
WASM_FLAGS = --target=wasm32-unknown-unknown -nostdlib -Oz \
    -fuse-ld=/opt/homebrew/bin/wasm-ld \
    -Wl,--no-entry -Wl,--export-dynamic \
    -Wl,--initial-memory=786432 \
    -Wno-void-pointer-to-int-cast

# 4K build: size-optimized synth, explicit exports, smaller limits
WASM_4K_FLAGS = --target=wasm32-unknown-unknown -nostdlib -Oz \
    -fuse-ld=/opt/homebrew/bin/wasm-ld \
    -Wl,--no-entry \
    -Wl,--export=getSongBuf -Wl,--export=getOutputBuf \
    -Wl,--export=init -Wl,--export=render \
    -Wl,--export=getRow -Wl,--export=getPattern \
    -Wl,--initial-memory=524288 \
    -Wno-void-pointer-to-int-cast \
    -DBIRB_MAX_PATTERNS=16 -DBIRB_MAX_ROWS=32 \
    -DBIRB_MAX_INSTRUMENTS=8 -DBIRB_MAX_ORDER=32 \
    -DBIRB_MAX_SAMPLES=4 -DBIRB_SAMPLE_POOL=65536 \
    -DBIRB_KS_BUF_SIZE=256 \
    -DBIRB_NO_REVERB

# Reverb-enabled 4K flags: same as the lean 4K set but with the reverb bus.
# The reverb build pays float + ~26KB of comb/allpass buffers; the default 4K
# targets stay lean (BIRB_NO_REVERB above). Drop BIRB_NO_REVERB for a "with
# reverb" build of any 4K variant.

.PHONY: all clean test test-compiled web serve sizes tiers

all: birb_wav birbc birb_play

birb_wav: birb_synth.c birb_wav.c birb_synth.h
	$(CC) $(CFLAGS) birb_synth.c birb_wav.c -o birb_wav

birbc: birbc.c birb_synth.h birb_format.h
	$(CC) $(CFLAGS) birbc.c -o birbc

# Real-time CoreAudio player
birb_play: birb_synth.c birb_macos.c birb_synth.h birb_format.h
	$(CC) $(CFLAGS) birb_synth.c birb_macos.c -framework AudioToolbox -framework CoreFoundation -o birb_play

# Player that loads compiled binary song data
birb_play_bin: birb_synth.c birb_play_bin.c birb_synth.h birb_format.h
	$(CC) $(CFLAGS) birb_synth.c birb_play_bin.c -o birb_play_bin

# WASM synth engine
web/birb.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then \
		echo "Error: No wasm-capable clang found. Install with: brew install llvm"; \
		exit 1; \
	fi
	$(WASM_CC) $(WASM_FLAGS) birb_synth.c birb_wasm.c -o web/birb.wasm
	@ls -la web/birb.wasm
	@if command -v brotli >/dev/null 2>&1; then \
		brotli --best -f web/birb.wasm -o web/birb.wasm.br; \
		echo "Brotli: $$(wc -c < web/birb.wasm.br | tr -d ' ')b"; \
	fi

# Compile test song (all formats)
test_song.bin test_song.h test_song.js: test_song.birb birbc
	./birbc test_song.birb --js

# 4K WASM build without FM (for size measurement vs FM-enabled build)
web/birb4k_nofm.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then \
		echo "Error: No wasm-capable clang found. Install with: brew install llvm"; \
		exit 1; \
	fi
	$(WASM_CC) $(WASM_4K_FLAGS) -DBIRB_NO_FM birb_synth.c birb_wasm.c -o web/birb4k_nofm.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then \
		wasm-opt -Oz web/birb4k_nofm.wasm -o web/birb4k_nofm.wasm; \
	fi
	@ls -la web/birb4k_nofm.wasm
	@if command -v brotli >/dev/null 2>&1; then \
		brotli --best -f web/birb4k_nofm.wasm -o web/birb4k_nofm.wasm.br; \
		echo "brotli: $$(wc -c < web/birb4k_nofm.wasm.br | tr -d ' ')b"; \
	fi

# 4K WASM build without drums (for size measurement vs drum-enabled build)
web/birb4k_nodrum.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then \
		echo "Error: No wasm-capable clang found. Install with: brew install llvm"; \
		exit 1; \
	fi
	$(WASM_CC) $(WASM_4K_FLAGS) -DBIRB_NO_DRUM birb_synth.c birb_wasm.c -o web/birb4k_nodrum.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then \
		wasm-opt -Oz web/birb4k_nodrum.wasm -o web/birb4k_nodrum.wasm; \
	fi
	@ls -la web/birb4k_nodrum.wasm
	@if command -v brotli >/dev/null 2>&1; then \
		brotli --best -f web/birb4k_nodrum.wasm -o web/birb4k_nodrum.wasm.br; \
		echo "brotli: $$(wc -c < web/birb4k_nodrum.wasm.br | tr -d ' ')b"; \
	fi

# 4K WASM drum-only build: drums kept, everything else stripped.
web/birb4k_drumonly.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then \
		echo "Error: No wasm-capable clang found. Install with: brew install llvm"; \
		exit 1; \
	fi
	$(WASM_CC) $(WASM_4K_FLAGS) -DBIRB_NO_FM -DBIRB_NO_KS -DBIRB_NO_SAMPLES birb_synth.c birb_wasm.c -o web/birb4k_drumonly.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then \
		wasm-opt -Oz web/birb4k_drumonly.wasm -o web/birb4k_drumonly.wasm; \
	fi
	@ls -la web/birb4k_drumonly.wasm
	@if command -v brotli >/dev/null 2>&1; then \
		brotli --best -f web/birb4k_drumonly.wasm -o web/birb4k_drumonly.wasm.br; \
		echo "brotli: $$(wc -c < web/birb4k_drumonly.wasm.br | tr -d ' ')b"; \
	fi

# 4K WASM build without formant (for size measurement vs formant-enabled build)
web/birb4k_noformant.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then \
		echo "Error: No wasm-capable clang found. Install with: brew install llvm"; \
		exit 1; \
	fi
	$(WASM_CC) $(WASM_4K_FLAGS) -DBIRB_NO_FORMANT birb_synth.c birb_wasm.c -o web/birb4k_noformant.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then \
		wasm-opt -Oz web/birb4k_noformant.wasm -o web/birb4k_noformant.wasm; \
	fi
	@ls -la web/birb4k_noformant.wasm
	@if command -v brotli >/dev/null 2>&1; then \
		brotli --best -f web/birb4k_noformant.wasm -o web/birb4k_noformant.wasm.br; \
		echo "brotli: $$(wc -c < web/birb4k_noformant.wasm.br | tr -d ' ')b"; \
	fi

# 4K WASM build without samples (even smaller)
web/birb4k_nosamples.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then \
		echo "Error: No wasm-capable clang found. Install with: brew install llvm"; \
		exit 1; \
	fi
	$(WASM_CC) $(WASM_4K_FLAGS) -DBIRB_NO_SAMPLES birb_synth.c birb_wasm.c -o web/birb4k_nosamples.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then \
		wasm-opt -Oz web/birb4k_nosamples.wasm -o web/birb4k_nosamples.wasm; \
	fi
	@ls -la web/birb4k_nosamples.wasm
	@gzip -9 -k -f web/birb4k_nosamples.wasm && \
		echo "gzip:   $$(wc -c < web/birb4k_nosamples.wasm.gz | tr -d ' ')b" && rm web/birb4k_nosamples.wasm.gz
	@if command -v brotli >/dev/null 2>&1; then \
		brotli --best -f web/birb4k_nosamples.wasm -o web/birb4k_nosamples.wasm.br; \
		echo "brotli: $$(wc -c < web/birb4k_nosamples.wasm.br | tr -d ' ')b"; \
	fi

# 4K WASM build (size-optimized)
web/birb4k.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then \
		echo "Error: No wasm-capable clang found. Install with: brew install llvm"; \
		exit 1; \
	fi
	$(WASM_CC) $(WASM_4K_FLAGS) birb_synth.c birb_wasm.c -o web/birb4k.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then \
		wasm-opt -Oz web/birb4k.wasm -o web/birb4k.wasm; \
		echo "wasm-opt applied"; \
	fi
	@ls -la web/birb4k.wasm
	@gzip -9 -k -f web/birb4k.wasm && \
		echo "gzip:   $$(wc -c < web/birb4k.wasm.gz | tr -d ' ')b" && rm web/birb4k.wasm.gz
	@if command -v brotli >/dev/null 2>&1; then \
		brotli --best -f web/birb4k.wasm -o web/birb4k.wasm.br; \
		echo "brotli: $$(wc -c < web/birb4k.wasm.br | tr -d ' ')b"; \
	fi

# ---- Size-tier build matrix (4K WASM) ----
# Named tiers map synth feature sets to predictable Brotli sizes. Use these
# for intros/4K where the song only exercises a subset of the engine.

# Minimal: basic synth only, and the smol birb feature set - no master bus, so
# no drive, no ducking, no limiter. Without BIRB_NO_MASTER the channel carries
# drive_pre/drive_norm/duck_send/duck_amt and lands at 120 bytes, over the
# 112-byte wasm cap, so this tier did not build at all. Target: smallest.
web/birb_minimal.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then echo "Error: No wasm-capable clang. brew install llvm"; exit 1; fi
	$(WASM_CC) $(WASM_4K_FLAGS) -DBIRB_NO_SAMPLES -DBIRB_NO_FM -DBIRB_NO_KS -DBIRB_NO_DRUM -DBIRB_NO_FORMANT \
	    -DBIRB_NO_MASTER \
	    birb_synth.c birb_wasm.c -o web/birb_minimal.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then wasm-opt -Oz web/birb_minimal.wasm -o web/birb_minimal.wasm; fi
	@if command -v brotli >/dev/null 2>&1; then brotli --best -f web/birb_minimal.wasm -o web/birb_minimal.wasm.br; fi

# Drum kit: basic + drum only, no FM/KS/samples/formant. Drum-only demos.
web/birb_drumkit.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then echo "Error: No wasm-capable clang. brew install llvm"; exit 1; fi
	$(WASM_CC) $(WASM_4K_FLAGS) -DBIRB_NO_SAMPLES -DBIRB_NO_FM -DBIRB_NO_KS -DBIRB_NO_FORMANT \
	    birb_synth.c birb_wasm.c -o web/birb_drumkit.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then wasm-opt -Oz web/birb_drumkit.wasm -o web/birb_drumkit.wasm; fi
	@if command -v brotli >/dev/null 2>&1; then brotli --best -f web/birb_drumkit.wasm -o web/birb_drumkit.wasm.br; fi

# Standard: everything except samples — the "no-sample floor" tier.
web/birb_standard.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then echo "Error: No wasm-capable clang. brew install llvm"; exit 1; fi
	$(WASM_CC) $(WASM_4K_FLAGS) -DBIRB_NO_SAMPLES \
	    birb_synth.c birb_wasm.c -o web/birb_standard.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then wasm-opt -Oz web/birb_standard.wasm -o web/birb_standard.wasm; fi
	@if command -v brotli >/dev/null 2>&1; then brotli --best -f web/birb_standard.wasm -o web/birb_standard.wasm.br; fi

# Full: everything including ADPCM samples.
web/birb_full.wasm: birb_synth.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then echo "Error: No wasm-capable clang. brew install llvm"; exit 1; fi
	$(WASM_CC) $(WASM_4K_FLAGS) \
	    birb_synth.c birb_wasm.c -o web/birb_full.wasm
	@if command -v wasm-opt >/dev/null 2>&1; then wasm-opt -Oz web/birb_full.wasm -o web/birb_full.wasm; fi
	@if command -v brotli >/dev/null 2>&1; then brotli --best -f web/birb_full.wasm -o web/birb_full.wasm.br; fi

tiers: web/birb_minimal.wasm web/birb_drumkit.wasm web/birb_standard.wasm web/birb_full.wasm

# Print a size table across all tiers.
sizes: tiers
	@printf '\n%-18s %10s %10s\n' "TIER" "raw (B)" "brotli (B)"
	@printf '%-18s %10s %10s\n' "------------------" "----------" "----------"
	@for t in minimal drumkit standard full; do \
	    raw=$$(wc -c < web/birb_$$t.wasm | tr -d ' '); \
	    br="-"; \
	    if [ -f web/birb_$$t.wasm.br ]; then br=$$(wc -c < web/birb_$$t.wasm.br | tr -d ' '); fi; \
	    printf '%-18s %10s %10s\n' "$$t" "$$raw" "$$br"; \
	done
	@echo ""

# Build everything for web
web: web/birb.wasm web/birb4k.wasm test_song.bin test_song.js
	cp test_song.bin web/test_song.bin
	cp test_song.js web/test_song.js

# Minified editor for sharing
web/editor.min.html: web/editor.html minify_editor.py
	python3 minify_editor.py web/editor.html web/editor.min.html

# Brotli-compressed minified editor (served with Content-Encoding: br)
web/editor.min.html.br: web/editor.min.html
	@if command -v brotli >/dev/null 2>&1; then \
		brotli --best -f web/editor.min.html -o web/editor.min.html.br; \
		echo "web/editor.min.html.br: $$(wc -c < web/editor.min.html.br | tr -d ' ') bytes"; \
	else \
		echo "Error: brotli not installed. brew install brotli"; exit 1; \
	fi

editor-dist: web/editor.min.html web/editor.min.html.br

# Serve web directory (AudioWorklet requires HTTPS or localhost)
serve: web
	@echo "Open http://localhost:8080 in your browser"
	python3 -m http.server 8080 --directory web

test: birb_wav
	./birb_wav birb_test.wav
	@echo "Playing test output..."
	afplay birb_test.wav

test-compiled: birbc birb_play_bin test_song.bin
	./birb_play_bin test_song.bin test_compiled.wav
	@echo "Playing compiled song..."
	afplay test_compiled.wav

clean:
	rm -f birb_wav birbc birb_play birb_play_bin birb_test.wav test_compiled.wav
	rm -f test_song.bin test_song.h test_song.js
	rm -f web/birb.wasm web/birb.wasm.br web/test_song.bin web/test_song.js
	rm -f web/birb4k.wasm web/birb4k.wasm.br
	rm -f web/birb4k_nofm.wasm web/birb4k_nofm.wasm.br
	rm -f web/birb4k_noks.wasm web/birb4k_noks.wasm.br
	rm -f web/birb4k_nodrum.wasm web/birb4k_nodrum.wasm.br
	rm -f web/birb4k_drumonly.wasm web/birb4k_drumonly.wasm.br
	rm -f web/birb4k_nosamples.wasm web/birb4k_nosamples.wasm.br
	rm -f web/birb4k_noformant.wasm web/birb4k_noformant.wasm.br
	rm -f web/birb_minimal.wasm web/birb_minimal.wasm.br
	rm -f web/birb_drumkit.wasm web/birb_drumkit.wasm.br
	rm -f web/birb_standard.wasm web/birb_standard.wasm.br
	rm -f web/birb_full.wasm web/birb_full.wasm.br
	rm -f web/editor.min.html web/editor.min.html.br
