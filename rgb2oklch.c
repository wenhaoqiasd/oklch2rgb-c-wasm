// sRGB (numeric) -> OKLCH converter
// Implementation notes:
// - Math and matrices follow Björn Ottosson's OKLab/OKLCH reference:
//   https://bottosson.github.io/posts/oklab/
// - Conversion behavior aligns with evilmartians/oklch-picker (MIT) where applicable,
//   but this is an independent C reimplementation (no code copied).
//   Repo: https://github.com/evilmartians/oklch-picker (MIT License)
// - Input: three CLI args R G B (0–255, interpreted as sRGB 8-bit components)
// - Output: prints three numbers: "L C h" (when C≈0, h is forced to 0)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double r; // 0..255
    double g; // 0..255
    double b; // 0..255
} RGB255;

typedef struct {
    double L; // 0..1 (OKLab L)
    double C; // chroma
    double h; // hue in degrees [0,360)
} OKLCH;

static double clamp(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

// Parse plain number [0..255], no % allowed
static int parse_0_255_number(const char* s, double* out) {
    while (isspace((unsigned char)*s)) s++;
    // Disallow '%'
    if (strchr(s, '%') != NULL) return 0;
    char* end = NULL;
    double val = strtod(s, &end);
    if (s == end) return 0;
    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = clamp(val, 0.0, 255.0);
    return 1;
}

// No CSS parsing; only numeric CLI input is supported.

static double srgb_to_linear(double u) {
    // u is 0..1 sRGB
    if (u <= 0.04045) return u / 12.92;
    return pow((u + 0.055) / 1.055, 2.4);
}

static OKLCH rgb_to_oklch(RGB255 in) {
    // Normalize to 0..1 sRGB
    double rs = clamp(in.r / 255.0, 0.0, 1.0);
    double gs = clamp(in.g / 255.0, 0.0, 1.0);
    double bs = clamp(in.b / 255.0, 0.0, 1.0);

    // Linearize
    double r = srgb_to_linear(rs);
    double g = srgb_to_linear(gs);
    double b = srgb_to_linear(bs);

    // Convert linear sRGB to LMS via OKLab matrix
    double l_ = 0.4122214708*r + 0.5363325363*g + 0.0514459929*b;
    double m_ = 0.2119034982*r + 0.6806995451*g + 0.1073969566*b;
    double s_ = 0.0883024619*r + 0.2817188376*g + 0.6299787005*b;

    // cube roots
    double l = cbrt(l_);
    double m = cbrt(m_);
    double s = cbrt(s_);

    // OKLab
    double L = 0.2104542553*l + 0.7936177850*m - 0.0040720468*s;
    double a = 1.9779984951*l - 2.4285922050*m + 0.4505937099*s;
    double bb = 0.0259040371*l + 0.7827717662*m - 0.8086757660*s;

    // OKLCH
    double C = sqrt(a*a + bb*bb);
    double h = 0.0;
    if (C > 1e-12) {
        double hRad = atan2(bb, a);
        h = hRad * 180.0 / M_PI;
        if (h < 0) h += 360.0;
    } else {
        // For near achromatic, force h=0 to match desired standard
        C = 0.0;
        h = 0.0;
    }

    OKLCH out = (OKLCH){ L, C, h };
    return out;
}

static void trim_number(char* s) {
    // Remove trailing zeros and a trailing decimal point
    size_t n = strlen(s);
    if (n == 0) return;
    // If contains 'e' or 'E' leave as is
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == 'e' || s[i] == 'E') return;
    }
    // Trim zeros
    while (n > 0 && s[n-1] == '0') { s[--n] = '\0'; }
    if (n > 0 && s[n-1] == '.') { s[--n] = '\0'; }
}

static void print_oklch(const OKLCH* o) {
    // Format minimal numbers without trailing zeros
    char Ls[64], Cs[64], hs[64];
    // Clamp tiny values to zero to avoid "-0"
    double L = fabs(o->L) < 1e-15 ? 0.0 : o->L;
    double C = fabs(o->C) < 1e-15 ? 0.0 : o->C;
    double h = fabs(o->h) < 1e-15 ? 0.0 : o->h;
    snprintf(Ls, sizeof(Ls), "%.6f", L);
    snprintf(Cs, sizeof(Cs), "%.6f", C);
    snprintf(hs, sizeof(hs), "%.6f", h);
    trim_number(Ls);
    trim_number(Cs);
    trim_number(hs);
    if (strcmp(Cs, "0") == 0) {
        strcpy(hs, "0");
    }
    // Plain numeric output: L C h
    printf("%s %s %s\n", Ls, Cs, hs);
}

// ---- Minimal WASM export for JS (no Emscripten glue needed) ----
// When compiling for WebAssembly in a minimal setup, avoid requiring
// stdio/argv by exposing a pure function that returns results via memory.
// This keeps the Wasm module free of WASI/Emscripten runtime dependencies.
#ifdef __EMSCRIPTEN__
// A static buffer to hold the 3 double values [L, C, h].
static double g_oklch_out[3];

// Compute OKLCH from 8-bit sRGB and return a pointer (offset) into Wasm memory
// where 3 doubles are stored: [L, C, h]. The returned value is a 32-bit
// linear-memory address that JS can read with a Float64Array view.
// Signature chosen to be simple to call from JS without any malloc.
__attribute__((export_name("rgb2oklch_calc_js")))
uint32_t rgb2oklch_calc_js(int r, int g, int b) {
    RGB255 in = { (double)r, (double)g, (double)b };
    OKLCH o = rgb_to_oklch(in);
    g_oklch_out[0] = o.L;
    g_oklch_out[1] = o.C;
    g_oklch_out[2] = o.h;
    return (uint32_t)(uintptr_t)g_oklch_out;
}
#endif

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage:\n"
    "  %s R G B\n\n"
        "Notes:\n"
    "  - R,G,B: 0-255 numbers\n"
        "Examples:\n"
    "  %s 255 255 255            -> 1 0 0\n",
        prog, prog);
}

#ifndef __EMSCRIPTEN__
int main(int argc, char** argv) {
    RGB255 rgb;
    int ok = 0;
    if (argc == 4) {
    // R G B values (numbers 0..255)
    double R, G, B;
    if (!parse_0_255_number(argv[1], &R)) { usage(argv[0]); return 1; }
    if (!parse_0_255_number(argv[2], &G)) { usage(argv[0]); return 1; }
    if (!parse_0_255_number(argv[3], &B)) { usage(argv[0]); return 1; }
    rgb.r = R; rgb.g = G; rgb.b = B;
        ok = 1;
    } else {
        usage(argv[0]);
        return 1;
    }

    if (!ok) {
        fprintf(stderr, "Failed to parse input.\n");
        usage(argv[0]);
        return 1;
    }

    OKLCH o = rgb_to_oklch(rgb);
    print_oklch(&o);
    return 0;
}
#endif