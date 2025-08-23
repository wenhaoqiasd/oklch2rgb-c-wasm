// sRGB（数值）-> OKLCH 转换器
// 实现说明：
// - 数学与矩阵遵循 Björn Ottosson 的 OKLab/OKLCH 参考：
//   https://bottosson.github.io/posts/oklab/
// - 转换行为在适用处与 evilmartians/oklch-picker（MIT）保持一致，
//   但这是独立的 C 语言重写（未复制任何代码）。
//   仓库： https://github.com/evilmartians/oklch-picker （MIT 许可）
// - 输入：三个命令行参数 R G B（0–255，按 sRGB 8 位分量解释）
// - 输出：打印三个数值 "L C h"（当 C≈0 时，h 被强制为 0）

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

typedef struct
{
    double r; // 0..255
    double g; // 0..255
    double b; // 0..255
} RGB255;

typedef struct
{
    double L; // 0..1 (OKLab L)
    double C; // chroma
    double h; // hue in degrees [0,360)
} OKLCH;

static double clamp(double x, double lo, double hi)
{
    if (x < lo)
        return lo;
    if (x > hi)
        return hi;
    return x;
}

// 解析一个 0..255 的纯数字，不允许出现 %
static int parse_0_255_number(const char *s, double *out)
{
    while (isspace((unsigned char)*s))
        s++;
    // Disallow '%'
    // 不允许 '%'
    if (strchr(s, '%') != NULL)
        return 0;
    char *end = NULL;
    double val = strtod(s, &end);
    if (s == end)
        return 0;
    while (isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return 0;
    *out = clamp(val, 0.0, 255.0);
    return 1;
}

// 不做 CSS 解析；仅支持数值型命令行输入。

static double srgb_to_linear(double u)
{
    // u 为 0..1 的 sRGB
    if (u <= 0.04045)
        return u / 12.92;
    return pow((u + 0.055) / 1.055, 2.4);
}

static OKLCH rgb_to_oklch(RGB255 in)
{
    // 归一化到 0..1 的 sRGB
    double rs = clamp(in.r / 255.0, 0.0, 1.0);
    double gs = clamp(in.g / 255.0, 0.0, 1.0);
    double bs = clamp(in.b / 255.0, 0.0, 1.0);

    // 线性化
    double r = srgb_to_linear(rs);
    double g = srgb_to_linear(gs);
    double b = srgb_to_linear(bs);

    // 通过 OKLab 矩阵将线性 sRGB 转为 LMS
    double l_ = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    double m_ = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    double s_ = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;

    // 立方根
    double l = cbrt(l_);
    double m = cbrt(m_);
    double s = cbrt(s_);

    // OKLab
    double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    double bb = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;

    // OKLCH
    double C = sqrt(a * a + bb * bb);
    double h = 0.0;
    if (C > 1e-12)
    {
        double hRad = atan2(bb, a);
        h = hRad * 180.0 / M_PI;
        if (h < 0)
            h += 360.0;
    }
    else
    {
        // 近乎无彩（接近灰阶）时，强制 h=0 以符合预期标准
        C = 0.0;
        h = 0.0;
    }

    OKLCH out = (OKLCH){L, C, h};
    return out;
}

static void trim_number(char *s)
{
    // 去除末尾多余的 0 以及多余的小数点
    size_t n = strlen(s);
    if (n == 0)
        return;
    // 若包含 'e' 或 'E'，保持不变
    for (size_t i = 0; i < n; ++i)
    {
        if (s[i] == 'e' || s[i] == 'E')
            return;
    }
    // 修剪 0
    while (n > 0 && s[n - 1] == '0')
    {
        s[--n] = '\0';
    }
    if (n > 0 && s[n - 1] == '.')
    {
        s[--n] = '\0';
    }
}

static void print_oklch(const OKLCH *o)
{
    // 以精简格式输出数字，去除末尾 0
    char Ls[64], Cs[64], hs[64];
    // 将极小值夹到 0 以避免出现 "-0"
    double L = fabs(o->L) < 1e-15 ? 0.0 : o->L;
    double C = fabs(o->C) < 1e-15 ? 0.0 : o->C;
    double h = fabs(o->h) < 1e-15 ? 0.0 : o->h;
    snprintf(Ls, sizeof(Ls), "%.6f", L);
    snprintf(Cs, sizeof(Cs), "%.6f", C);
    snprintf(hs, sizeof(hs), "%.6f", h);
    trim_number(Ls);
    trim_number(Cs);
    trim_number(hs);
    if (strcmp(Cs, "0") == 0)
    {
        strcpy(hs, "0");
    }
    // 纯数值输出：L C h
    printf("%s %s %s\n", Ls, Cs, hs);
}

// ---- 面向 JS 的最简 WASM 导出（无需 Emscripten 胶水）----
// 以最小化设置编译到 WebAssembly 时，通过暴露一个纯函数并通过内存返回结果，
// 避免引入 stdio/argv 依赖。这样 Wasm 模块不依赖 WASI/Emscripten 运行时。
#ifdef __EMSCRIPTEN__
// 用于存放 [L, C, h] 三个 double 值的静态缓冲区。
static double g_oklch_out[3];

// 从 8 位 sRGB 计算 OKLCH，并返回一个指向 Wasm 线性内存的指针（偏移），
// 其中按顺序存放 3 个 double：[L, C, h]。返回值是 32 位的线性内存地址，
// JS 可通过 Float64Array 视图读取。该签名便于 JS 直接调用而无需 malloc。
__attribute__((export_name("rgb2oklch_calc_js")))
uint32_t
rgb2oklch_calc_js(int r, int g, int b)
{
    RGB255 in = {(double)r, (double)g, (double)b};
    OKLCH o = rgb_to_oklch(in);
    g_oklch_out[0] = o.L;
    g_oklch_out[1] = o.C;
    g_oklch_out[2] = o.h;
    return (uint32_t)(uintptr_t)g_oklch_out;
}
#endif

static void usage(const char *prog)
{
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
int main(int argc, char **argv)
{
    RGB255 rgb;
    int ok = 0;
    if (argc == 4)
    {
        // R G B values (numbers 0..255)
        double R, G, B;
        if (!parse_0_255_number(argv[1], &R))
        {
            usage(argv[0]);
            return 1;
        }
        if (!parse_0_255_number(argv[2], &G))
        {
            usage(argv[0]);
            return 1;
        }
        if (!parse_0_255_number(argv[3], &B))
        {
            usage(argv[0]);
            return 1;
        }
        rgb.r = R;
        rgb.g = G;
        rgb.b = B;
        ok = 1;
    }
    else
    {
        usage(argv[0]);
        return 1;
    }

    if (!ok)
    {
        fprintf(stderr, "Failed to parse input.\n");
        usage(argv[0]);
        return 1;
    }

    OKLCH o = rgb_to_oklch(rgb);
    print_oklch(&o);
    return 0;
}
#endif