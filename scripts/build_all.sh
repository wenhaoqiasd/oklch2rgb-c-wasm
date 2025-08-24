#!/usr/bin/env bash
# One-shot builder for native binaries + WASM + quick smoke tests
# Platform: macOS (for extract-colors native frameworks)
# Shell: bash/zsh compatible
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="${SCRIPT_DIR%/scripts}"
cd "$ROOT_DIR"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

say() { printf "${YELLOW}==>${NC} %s\n" "$*"; }
ok() { printf "${GREEN}[OK]${NC} %s\n" " $*"; }
fail() { printf "${RED}[FAIL]${NC} %s\n" " $*"; }

# 1) Build native binaries (macOS)
say "Building native binaries (clang)"
clang -O3 -march=native -ffast-math -std=c11 oklch2rgb.c -o oklch2rgb
clang -O3 -march=native -ffast-math -std=c11 rgb2oklch.c -o rgb2oklch
# extract-colors needs Apple frameworks
clang -O3 -ffast-math -std=c11 extract-colors.c -o extract-colors \
  -framework ImageIO -framework CoreGraphics -framework CoreFoundation
ok "Native build done"

# 2) Build WebAssembly modules (emcc)
if ! command -v emcc >/dev/null 2>&1; then
  fail "emcc not found in PATH. Please install Emscripten or source emsdk_env.sh"
  exit 1
fi
mkdir -p wasm
say "Building WASM (standalone, no entry)"
# oklch2rgb.wasm
emcc -O3 -ffast-math -s STANDALONE_WASM=1 \
  -Wl,--no-entry \
  -Wl,--export=oklch2rgb_calc_js \
  -Wl,--export=oklch2rgb_calc_rel_js \
  oklch2rgb.c -o wasm/oklch2rgb.wasm
# rgb2oklch.wasm
emcc -O3 -ffast-math -s STANDALONE_WASM=1 \
  -Wl,--no-entry \
  -Wl,--export=rgb2oklch_calc_js \
  rgb2oklch.c -o wasm/rgb2oklch.wasm
# extract-colors.wasm
emcc -O3 -ffast-math -s STANDALONE_WASM=1 \
  -Wl,--no-entry \
  -Wl,--export=get_pixels_buffer \
  -Wl,--export=extract_colors_from_rgba_js \
  extract-colors.c -o wasm/extract-colors.wasm
ok "WASM build done"

# 3) Quick smoke tests
say "Running smoke tests"
OC_OUT=$(./oklch2rgb 0.7 0.2 30)
if [ "$OC_OUT" = "255 101 81" ]; then
  ok "oklch2rgb: 0.7 0.2 30 => $OC_OUT"
else
  fail "oklch2rgb unexpected output: $OC_OUT (expected: 255 101 81)"
  exit 2
fi

RO_OUT=$(./rgb2oklch 255 255 255)
if [ "$RO_OUT" = "1 0 0" ]; then
  ok "rgb2oklch: 255 255 255 => $RO_OUT"
else
  fail "rgb2oklch unexpected output: $RO_OUT (expected: 1 0 0)"
  exit 3
fi

if [ -f m.png ]; then
  # Only check the command runs and returns JSON-ish. Limit the output.
  ./extract-colors m.png >/dev/null
  ok "extract-colors: ran against m.png"
else
  say "m.png not found, skipping extract-colors smoke test"
fi

say "All done"
