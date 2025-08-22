// OKLCH (numeric) -> sRGB (numeric) converter
// Implementation notes:
// - Math and matrices follow Björn Ottosson's OKLab/OKLCH reference:
//   https://bottosson.github.io/posts/oklab/
// - Input: three CLI args L C h  (L in [0..1], C >= 0, h in degrees [0..360))
// - Output: three integers "R G B" (0..255), gamma-encoded sRGB, clamped

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

static double clamp(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

// Parse a floating number (no % suffix)
static int parse_number(const char* s, double* out) {
    while (isspace((unsigned char)*s)) s++;
    if (strchr(s, '%') != NULL) return 0;
    char* end = NULL;
    double v = strtod(s, &end);
    if (s == end) return 0;
    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

static double linear_to_srgb(double u) {
    if (u <= 0.0) return 0.0;
    if (u >= 1.0) return 1.0;
    if (u <= 0.0031308) return 12.92 * u;
    return 1.055 * pow(u, 1.0/2.4) - 0.055;
}

// ---- Helpers for OKLCH -> linear sRGB and gamut fallback ----
static void oklch_to_linear_rgb(double L, double C, double hdeg,
                                double* r_lin, double* g_lin, double* b_lin) {
    // Wrap hue
    double h = fmod(hdeg, 360.0);
    if (h < 0) h += 360.0;
    // OKLCH -> OKLab
    double hr = h * M_PI / 180.0;
    double a = C * cos(hr);
    double b = C * sin(hr);
    // OKLab -> LMS (nonlinear)
    double l = L + 0.3963377774*a + 0.2158037573*b;
    double m = L - 0.1055613458*a - 0.0638541728*b;
    double s = L - 0.0894841775*a - 1.2914855480*b;
    // cube to linear LMS
    double l3 = l*l*l;
    double m3 = m*m*m;
    double s3 = s*s*s;
    // Linear sRGB from LMS^3
    *r_lin = +4.0767416621*l3 - 3.3077115913*m3 + 0.2309699292*s3;
    *g_lin = -1.2684380046*l3 + 2.6097574011*m3 - 0.3413193965*s3;
    *b_lin = -0.0041960863*l3 - 0.7034186147*m3 + 1.7076147010*s3;
}

static int is_linear_in_srgb_gamut(double r, double g, double b) {
    const double eps = 1e-12; // tolerate tiny drift
    return r >= -eps && r <= 1.0 + eps &&
           g >= -eps && g <= 1.0 + eps &&
           b >= -eps && b <= 1.0 + eps;
}

// Given L, h, and a target chroma C, find the largest C' in [0, C]
// such that linear sRGB is within [0,1]. If already in gamut, returns C.
static double find_gamut_safe_chroma(double L, double C, double hdeg) {
    double r, g, b;
    oklch_to_linear_rgb(L, C, hdeg, &r, &g, &b);
    if (is_linear_in_srgb_gamut(r, g, b)) return C;
    // Binary search on scale factor k ∈ [0, 1]
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 30; i++) {
        double mid = 0.5 * (lo + hi);
        double r2, g2, b2;
        oklch_to_linear_rgb(L, C * mid, hdeg, &r2, &g2, &b2);
        if (is_linear_in_srgb_gamut(r2, g2, b2)) {
            lo = mid; // can increase chroma
        } else {
            hi = mid; // too saturated
        }
    }
    return C * lo;
}

#ifdef __EMSCRIPTEN__
// Export a minimal function for JS to call directly from Wasm.
// Input: (L in [0..1], C >=0, h in degrees)
// Output: returns a pointer to 3 int32 values [R,G,B] in 0..255.
static int32_t g_rgb_out[3];

__attribute__((export_name("oklch2rgb_calc_js")))
uint32_t oklch2rgb_calc_js(double L, double C, double hdeg) {
    // Normalize inputs
    if (L < 0.0) L = 0.0; if (L > 1.0) L = 1.0;
    if (C < 0.0) C = 0.0;
    // Gamut fallback: reduce chroma if needed to fit sRGB
    double Csafe = find_gamut_safe_chroma(L, C, hdeg);
    double r_lin, g_lin, b_lin;
    oklch_to_linear_rgb(L, Csafe, hdeg, &r_lin, &g_lin, &b_lin);
    // Encode to sRGB and clamp to [0,1]
    double r = linear_to_srgb(r_lin);
    double g = linear_to_srgb(g_lin);
    double b2 = linear_to_srgb(b_lin);
    if (r < 0.0) r = 0.0; if (r > 1.0) r = 1.0;
    if (g < 0.0) g = 0.0; if (g > 1.0) g = 1.0;
    if (b2 < 0.0) b2 = 0.0; if (b2 > 1.0) b2 = 1.0;
    int R = (int)floor(r * 255.0 + 0.5);
    int G = (int)floor(g * 255.0 + 0.5);
    int B = (int)floor(b2 * 255.0 + 0.5);
    g_rgb_out[0] = R; g_rgb_out[1] = G; g_rgb_out[2] = B;
    return (uint32_t)(uintptr_t)g_rgb_out;
}
#endif

#ifndef __EMSCRIPTEN__
int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr,
            "Usage:\n"
            "  %s L C h\n\n"
            "Notes:\n"
            "  - L in [0..1], C >= 0, h in degrees [0..360)\n"
            "  - Output is sRGB 0..255 integers: R G B\n",
            argv[0]);
        return 1;
    }

    double L, C, hdeg;
    if (!parse_number(argv[1], &L) || !parse_number(argv[2], &C) || !parse_number(argv[3], &hdeg)) {
        fprintf(stderr, "Failed to parse input. Expect: L C h\n");
        return 1;
    }
    // Normalize inputs
    if (L < 0.0) L = 0.0; if (L > 1.0) L = 1.0;
    if (C < 0.0) C = 0.0;
    // Wrap hue
    double h = fmod(hdeg, 360.0);
    if (h < 0) h += 360.0;

    // Gamut-safe conversion: scale C down if needed so linear sRGB ∈ [0,1]
    double Csafe = find_gamut_safe_chroma(L, C, h);
    double r_lin, g_lin, b_lin;
    oklch_to_linear_rgb(L, Csafe, h, &r_lin, &g_lin, &b_lin);
    double r = linear_to_srgb(r_lin);
    double g = linear_to_srgb(g_lin);
    double b2 = linear_to_srgb(b_lin);
    int R = (int)floor(clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    int G = (int)floor(clamp(g, 0.0, 1.0) * 255.0 + 0.5);
    int B = (int)floor(clamp(b2, 0.0, 1.0) * 255.0 + 0.5);

    // Plain numeric output: R G B
    printf("%d %d %d\n", R, G, B);
    return 0;
}
#endif
