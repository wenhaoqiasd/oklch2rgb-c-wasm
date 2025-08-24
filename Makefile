SHELL := /bin/bash

# Compiler settings
CC      ?= clang
CFLAGS  ?= -O3 -ffast-math -std=c11
NATIVE_EXTRA ?= -march=native

# Emscripten settings
EMCC    ?= emcc
EMFLAGS ?= -O3 -ffast-math -s STANDALONE_WASM=1 -Wl,--no-entry

# Files
NATIVE_BINS := oklch2rgb rgb2oklch extract-colors
WASM_DIR    := wasm
WASM_BINS   := $(WASM_DIR)/oklch2rgb.wasm $(WASM_DIR)/rgb2oklch.wasm $(WASM_DIR)/extract-colors.wasm

.PHONY: all native wasm test clean

all: native wasm test

native: $(NATIVE_BINS)

oklch2rgb: oklch2rgb.c
	$(CC) $(CFLAGS) $(NATIVE_EXTRA) $< -o $@

rgb2oklch: rgb2oklch.c
	$(CC) $(CFLAGS) $(NATIVE_EXTRA) $< -o $@

extract-colors: extract-colors.c
	$(CC) $(CFLAGS) $< -o $@ \
	  -framework ImageIO -framework CoreGraphics -framework CoreFoundation

wasm: $(WASM_BINS)

$(WASM_DIR)/oklch2rgb.wasm: oklch2rgb.c | $(WASM_DIR)/.dir
	$(EMCC) $(EMFLAGS) \
	  -Wl,--export=oklch2rgb_calc_js \
	  -Wl,--export=oklch2rgb_calc_rel_js \
	  $< -o $@

$(WASM_DIR)/rgb2oklch.wasm: rgb2oklch.c | $(WASM_DIR)/.dir
	$(EMCC) $(EMFLAGS) \
	  -Wl,--export=rgb2oklch_calc_js \
	  $< -o $@

$(WASM_DIR)/extract-colors.wasm: extract-colors.c | $(WASM_DIR)/.dir
	$(EMCC) $(EMFLAGS) \
	  -Wl,--export=get_pixels_buffer \
	  -Wl,--export=extract_colors_from_rgba_js \
	  $< -o $@

$(WASM_DIR)/.dir:
	mkdir -p $(WASM_DIR)
	touch $@

test: native
	@set -e; \
	OC_OUT=$$(./oklch2rgb 0.7 0.2 30); \
	if [[ "$$OC_OUT" == "255 101 81" ]]; then \
	  echo "[OK] oklch2rgb: $$OC_OUT"; \
	else \
	  echo "[FAIL] oklch2rgb => $$OC_OUT (expect 255 101 81)"; exit 2; \
	fi; \
	RO_OUT=$$(./rgb2oklch 255 255 255); \
	if [[ "$$RO_OUT" == "1 0 0" ]]; then \
	  echo "[OK] rgb2oklch: $$RO_OUT"; \
	else \
	  echo "[FAIL] rgb2oklch => $$RO_OUT (expect 1 0 0)"; exit 3; \
	fi; \
	if [[ -f m.png ]]; then \
	  ./extract-colors m.png >/dev/null && echo "[OK] extract-colors ran"; \
	else \
	  echo "[SKIP] extract-colors (m.png not found)"; \
	fi

clean:
	rm -f $(NATIVE_BINS)
	rm -f $(WASM_BINS)