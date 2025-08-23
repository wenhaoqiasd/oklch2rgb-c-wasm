// OKLCH（数值）-> sRGB（数值）转换器
// 实现说明：
// - 数学与矩阵遵循 Björn Ottosson 的 OKLab/OKLCH 参考：
//   https://bottosson.github.io/posts/oklab/
// - 输入：命令行参数
//     L C h [rel]
//     L ∈ [0..1]，C ≥ 0，h 为角度 [0..360)
//     rel（可选）∈ [0..1]：相对色度比例；提供该参数时将忽略 C
// - 输出：三个整数 "R G B"（0..255），gamma 编码的 sRGB，并已夹取

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

static double clamp(double x, double lo, double hi)
{
    if (x < lo)
        return lo;
    if (x > hi)
        return hi;
    return x;
}

// 解析一个浮点数（不允许 % 后缀）
static int parse_number(const char *s, double *out)
{
    while (isspace((unsigned char)*s))
        s++;
    if (strchr(s, '%') != NULL)
        return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (s == end)
        return 0;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return 0;
    *out = v;
    return 1;
}

static double linear_to_srgb(double u)
{
    if (u <= 0.0)
        return 0.0;
    if (u >= 1.0)
        return 1.0;
    if (u <= 0.0031308)
        return 12.92 * u;
    return 1.055 * pow(u, 1.0 / 2.4) - 0.055;
}

// ---- OKLCH -> 线性 sRGB 以及色域回退的辅助函数 ----
static void oklch_to_linear_rgb(double L, double C, double hdeg,
                                double *r_lin, double *g_lin, double *b_lin)
{
    // 包裹色相（范围归一到 0..360）
    double h = fmod(hdeg, 360.0);
    if (h < 0)
        h += 360.0;
    // OKLCH -> OKLab
    double hr = h * M_PI / 180.0;
    double a = C * cos(hr);
    double b = C * sin(hr);
    // OKLab -> LMS（非线性）
    double l = L + 0.3963377774 * a + 0.2158037573 * b;
    double m = L - 0.1055613458 * a - 0.0638541728 * b;
    double s = L - 0.0894841775 * a - 1.2914855480 * b;
    // 立方得到线性 LMS
    double l3 = l * l * l;
    double m3 = m * m * m;
    double s3 = s * s * s;
    // 由 LMS^3 转换为线性 sRGB
    *r_lin = +4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3;
    *g_lin = -1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3;
    *b_lin = -0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3;
}

static int is_linear_in_srgb_gamut(double r, double g, double b)
{
    const double eps = 1e-12; // 容忍极小的数值漂移
    return r >= -eps && r <= 1.0 + eps &&
           g >= -eps && g <= 1.0 + eps &&
           b >= -eps && b <= 1.0 + eps;
}

// 给定 L、h 与目标色度 C，寻找区间 [0, C] 内最大的 C'，
// 使得线性 sRGB 分量都落在 [0,1]。若已在色域内，直接返回 C。
static double find_gamut_safe_chroma(double L, double C, double hdeg)
{
    double r, g, b;
    oklch_to_linear_rgb(L, C, hdeg, &r, &g, &b);
    if (is_linear_in_srgb_gamut(r, g, b))
        return C;
    // 在缩放因子 k ∈ [0,1] 上进行二分搜索
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 30; i++)
    {
        double mid = 0.5 * (lo + hi);
        double r2, g2, b2;
        oklch_to_linear_rgb(L, C * mid, hdeg, &r2, &g2, &b2);
        if (is_linear_in_srgb_gamut(r2, g2, b2))
        {
            lo = mid; // 还能增大色度
        }
        else
        {
            hi = mid; // 过饱和
        }
    }
    return C * lo;
}

// 计算给定 L 与 h 在 sRGB 色域内可达到的最大色度（Cmax）。
// 策略：指数式增大 C，直到超出色域，再在该上界内用
// find_gamut_safe_chroma 做精细二分。这样无需假定固定上界。
static double max_chroma_for_srgb(double L, double hdeg)
{
    // 以较小的色度起步，逐步增大，直到超出色域。
    double C = 0.05;
    for (int i = 0; i < 12; i++)
    { // up to ~0.05 * 4096 = 204.8 as a hard cap
        // 最多约 ~0.05 * 4096 = 204.8 作为硬上限
        double r, g, b;
        oklch_to_linear_rgb(L, C, hdeg, &r, &g, &b);
        if (!is_linear_in_srgb_gamut(r, g, b))
        {
            // 在区间 [0, C] 内细化到边界
            return find_gamut_safe_chroma(L, C, hdeg);
        }
        C *= 2.0;
    }
    // 若仍在色域内（极不可能），也用二分法夹到边界。
    return find_gamut_safe_chroma(L, C, hdeg);
}

#ifdef __EMSCRIPTEN__
// 导出最小接口，供 JS 从 Wasm 直接调用。
// 输入：（L ∈ [0..1]，C ≥ 0，h 为角度）
// 输出：返回指向 3 个 int32 值 [R,G,B]（0..255）的指针。
static int32_t g_rgb_out[3];

__attribute__((export_name("oklch2rgb_calc_js")))
uint32_t
oklch2rgb_calc_js(double L, double C, double hdeg)
{
    // 归一化输入
    if (L < 0.0)
        L = 0.0;
    if (L > 1.0)
        L = 1.0;
    if (C < 0.0)
        C = 0.0;
    // 色域回退：如有需要，降低色度以适配 sRGB
    double Csafe = find_gamut_safe_chroma(L, C, hdeg);
    double r_lin, g_lin, b_lin;
    oklch_to_linear_rgb(L, Csafe, hdeg, &r_lin, &g_lin, &b_lin);
    // 编码为 sRGB 并夹取到 [0,1]
    double r = linear_to_srgb(r_lin);
    double g = linear_to_srgb(g_lin);
    double b2 = linear_to_srgb(b_lin);
    if (r < 0.0)
        r = 0.0;
    if (r > 1.0)
        r = 1.0;
    if (g < 0.0)
        g = 0.0;
    if (g > 1.0)
        g = 1.0;
    if (b2 < 0.0)
        b2 = 0.0;
    if (b2 > 1.0)
        b2 = 1.0;
    int R = (int)floor(r * 255.0 + 0.5);
    int G = (int)floor(g * 255.0 + 0.5);
    int B = (int)floor(b2 * 255.0 + 0.5);
    g_rgb_out[0] = R;
    g_rgb_out[1] = G;
    g_rgb_out[2] = B;
    return (uint32_t)(uintptr_t)g_rgb_out;
}

// JS 的相对色度版本：给定 L、h、rel（∈ [0..1]），忽略绝对 C。
// 计算 C = rel * Cmax(L,h)，并返回指向 [R,G,B]（0..255）的指针。
__attribute__((export_name("oklch2rgb_calc_rel_js")))
uint32_t
oklch2rgb_calc_rel_js(double L, double hdeg, double rel)
{
    if (L < 0.0)
        L = 0.0;
    if (L > 1.0)
        L = 1.0;
    if (rel < 0.0)
        rel = 0.0;
    if (rel > 1.0)
        rel = 1.0;

    double Cmax = max_chroma_for_srgb(L, hdeg);
    double C_use = rel * Cmax;
    // 最后一轮安全校正以抵消数值漂移
    double Csafe = find_gamut_safe_chroma(L, C_use, hdeg);

    double r_lin, g_lin, b_lin;
    oklch_to_linear_rgb(L, Csafe, hdeg, &r_lin, &g_lin, &b_lin);
    double r = linear_to_srgb(r_lin);
    double g = linear_to_srgb(g_lin);
    double b2 = linear_to_srgb(b_lin);
    if (r < 0.0)
        r = 0.0;
    if (r > 1.0)
        r = 1.0;
    if (g < 0.0)
        g = 0.0;
    if (g > 1.0)
        g = 1.0;
    if (b2 < 0.0)
        b2 = 0.0;
    if (b2 > 1.0)
        b2 = 1.0;
    int R = (int)floor(r * 255.0 + 0.5);
    int G = (int)floor(g * 255.0 + 0.5);
    int B = (int)floor(b2 * 255.0 + 0.5);
    g_rgb_out[0] = R;
    g_rgb_out[1] = G;
    g_rgb_out[2] = B;
    return (uint32_t)(uintptr_t)g_rgb_out;
}
#endif

#ifndef __EMSCRIPTEN__
int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5)
    {
        fprintf(stderr,
                "Usage:\n"
                "  %s L C h [rel]\n\n"
                "Notes:\n"
                "  - L in [0..1], C >= 0, h in degrees [0..360)\n"
                "  - rel (optional) in [0..1]. When provided, C is ignored and\n"
                "    chroma becomes rel * Cmax(L,h) where Cmax fits sRGB gamut.\n"
                "  - Output is sRGB 0..255 integers: R G B\n",
                argv[0]);
        return 1;
    }

    double L, C_in, hdeg;
    if (!parse_number(argv[1], &L) || !parse_number(argv[2], &C_in) || !parse_number(argv[3], &hdeg))
    {
        fprintf(stderr, "Failed to parse input. Expect: L C h [rel]\n");
        return 1;
    }
    // Normalize inputs
    if (L < 0.0)
        L = 0.0;
    if (L > 1.0)
        L = 1.0;
    if (C_in < 0.0)
        C_in = 0.0;
    // Wrap hue
    double h = fmod(hdeg, 360.0);
    if (h < 0)
        h += 360.0;

    // Decide chroma: either absolute (C_in) or relative (rel * Cmax)
    double C_use = C_in;
    if (argc == 5)
    {
        double rel;
        if (!parse_number(argv[4], &rel))
        {
            fprintf(stderr, "Failed to parse [rel]. Expect a number in [0..1].\n");
            return 1;
        }
        if (rel < 0.0)
            rel = 0.0;
        if (rel > 1.0)
            rel = 1.0;
        double Cmax = max_chroma_for_srgb(L, h);
        C_use = rel * Cmax;
    }

    // Gamut-safe conversion: scale C down if needed so linear sRGB ∈ [0,1]
    double Csafe = find_gamut_safe_chroma(L, C_use, h);
    double r_lin, g_lin, b_lin;
    oklch_to_linear_rgb(L, Csafe, h, &r_lin, &g_lin, &b_lin);
    double r = linear_to_srgb(r_lin);
    double g = linear_to_srgb(g_lin);
    double b2 = linear_to_srgb(b_lin);
    int R = (int)floor(clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    int G = (int)floor(clamp(g, 0.0, 1.0) * 255.0 + 0.5);
    int B = (int)floor(clamp(b2, 0.0, 1.0) * 255.0 + 0.5);

    // 纯数值输出：R G B
    printf("%d %d %d\n", R, G, B);
    return 0;
}
#endif
