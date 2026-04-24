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
    -Wl,--initial-memory=524288 \
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
    -DBIRB_MAX_SAMPLES=4 -DBIRB_SAMPLE_POOL=65536

.PHONY: all clean test test-compiled web serve

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

# 4K WASM build (size-optimized)
web/birb4k.wasm: birb_synth_mini.c birb_wasm.c birb_synth.h birb_format.h
	@if [ -z "$(WASM_CC)" ]; then \
		echo "Error: No wasm-capable clang found. Install with: brew install llvm"; \
		exit 1; \
	fi
	$(WASM_CC) $(WASM_4K_FLAGS) birb_synth_mini.c birb_wasm.c -o web/birb4k.wasm
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

# Build everything for web
web: web/birb.wasm web/birb4k.wasm test_song.bin test_song.js
	cp test_song.bin web/test_song.bin
	cp test_song.js web/test_song.js

# Minified editor for sharing
web/editor.min.html: web/editor.html minify_editor.py
	python3 minify_editor.py web/editor.html web/editor.min.html

editor-dist: web/editor.min.html

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
