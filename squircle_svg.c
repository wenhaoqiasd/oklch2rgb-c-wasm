// squircle_svg.c
// 基于 squircle_svg.ts 的 C 版 SVG 生成器
// 使用: squircle_svg <shape> <width> <height> <radius>
// shape: "squircle" | "capsule"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>

typedef struct
{
  double r160, r103, r075, r010, r054, r020, r035, r096;
} RadiusVals;

static inline RadiusVals get_radius_values(double r)
{
  RadiusVals v;
  v.r160 = r * 1.6;
  v.r103 = r * 1.03995;
  v.r075 = r * 0.759921;
  v.r010 = r * 0.108993;
  v.r054 = r * 0.546009;
  v.r020 = r * 0.204867;
  v.r035 = r * 0.357847;
  v.r096 = r * 0.96;
  return v;
}

// 将双精度数四舍五入到小数点后三位并去除尾随0（快速格式化），返回写入长度
static size_t fmt3(double x, char *out, size_t out_size)
{
  if (out_size == 0)
    return 0;

  // 对齐 JavaScript Math.round 的行为：四舍五入，0.5 情况朝正无穷方向
  // 等价于 floor(x * 1000 + 0.5)
  double scaled = x * 1000.0;
  double rounded = floor(scaled + 0.5);

  // 规避 -0 的显示（与 JS 字符串化一致）
  if (rounded == 0.0)
    rounded = 0.0;

  long long s = (long long)rounded;
  int neg = (s < 0);
  unsigned long long us = (unsigned long long)(neg ? -s : s);
  unsigned long long ip = us / 1000ULL;
  unsigned long long frac = us % 1000ULL;

  char buf[64];
  size_t p = 0;
  if (neg)
    buf[p++] = '-';

  // 写整数部分（反向生成再反转）
  char tmp[32];
  size_t ti = 0;
  if (ip == 0ULL)
    tmp[ti++] = '0';
  else
  {
    while (ip > 0ULL && ti < sizeof(tmp))
    {
      tmp[ti++] = (char)('0' + (ip % 10ULL));
      ip /= 10ULL;
    }
  }
  for (size_t i = 0; i < ti && p < sizeof(buf); ++i)
    buf[p++] = tmp[ti - 1 - i];

  if (frac != 0ULL)
  {
    // 打印固定 3 位再去除末尾 0，避免丢失中间的 0（如 296.076）
    char ft3[3];
    ft3[0] = (char)('0' + (int)((frac / 100ULL) % 10ULL));
    ft3[1] = (char)('0' + (int)((frac / 10ULL) % 10ULL));
    ft3[2] = (char)('0' + (int)(frac % 10ULL));

    int flen = 3;
    while (flen > 0 && ft3[flen - 1] == '0')
      --flen; // 仅移除末尾 0

    if (flen > 0)
    {
      buf[p++] = '.';
      for (int i = 0; i < flen && p < (int)sizeof(buf); ++i)
        buf[p++] = ft3[i];
    }
  }

  size_t n = (p < out_size - 1) ? p : (out_size - 1);
  memcpy(out, buf, n);
  out[n] = '\0';
  return n;
}

typedef struct
{
  char *data;
  size_t len;
  size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb, size_t cap)
{
  sb->data = (char *)malloc(cap);
  sb->len = 0;
  sb->cap = cap;
  if (sb->data)
    sb->data[0] = '\0';
}

static void sb_free(StrBuf *sb)
{
  free(sb->data);
  sb->data = NULL;
  sb->len = sb->cap = 0;
}

static void sb_ensure(StrBuf *sb, size_t extra)
{
  if (sb->len + extra + 1 <= sb->cap)
    return;
  size_t ncap = sb->cap ? sb->cap : 256;
  while (sb->len + extra + 1 > ncap)
    ncap *= 2;
  char *nd = (char *)realloc(sb->data, ncap);
  if (!nd)
    return; // 失败时静默，后续写入会出错，但避免崩溃
  sb->data = nd;
  sb->cap = ncap;
}

static void sb_append(StrBuf *sb, const char *s)
{
  size_t sl = strlen(s);
  sb_ensure(sb, sl);
  if (!sb->data)
    return;
  memcpy(sb->data + sb->len, s, sl);
  sb->len += sl;
  sb->data[sb->len] = '\0';
}

static void sb_append_len(StrBuf *sb, const char *s, size_t sl)
{
  sb_ensure(sb, sl);
  if (!sb->data)
    return;
  memcpy(sb->data + sb->len, s, sl);
  sb->len += sl;
  sb->data[sb->len] = '\0';
}

#define SB_APP_LIT(sb, lit) sb_append_len((sb), (lit), sizeof(lit) - 1)

static void sb_appendf(StrBuf *sb, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  // 试探性写入
  char tmp[512];
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n < sizeof(tmp))
  {
    sb_append(sb, tmp);
    return;
  }
  // 需要更大缓冲
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf)
    return;
  va_start(ap, fmt);
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  va_end(ap);
  sb_append(sb, buf);
  free(buf);
}

typedef struct
{
  // 基本尺寸
  char w[32], h[32], r[32];
  // 半径派生
  char r160[32], r103[32], r075[32], r010[32], r054[32], r020[32], r035[32], r096[32];
  // 宽度相关
  char wm_r160[32], wm_r103[32], wm_r075[32], wm_r054[32], wm_r035[32], wm_r020[32], wm_r010[32];
  // 高度相关
  char hm_r160[32], hm_r103[32], hm_r075[32], hm_r054[32], hm_r035[32], hm_r020[32], hm_r010[32], hm_r096[32];
  // 缓存长度
  size_t w_len, h_len, r_len;
  size_t r160_len, r103_len, r075_len, r010_len, r054_len, r020_len, r035_len, r096_len;
  size_t wm_r160_len, wm_r103_len, wm_r075_len, wm_r054_len, wm_r035_len, wm_r020_len, wm_r010_len;
  size_t hm_r160_len, hm_r103_len, hm_r075_len, hm_r054_len, hm_r035_len, hm_r020_len, hm_r010_len, hm_r096_len;
} PreFmt;

static void precompute_fmt(double w, double h, double r, const RadiusVals *v, PreFmt *pf)
{
  pf->w_len = fmt3(w, pf->w, sizeof(pf->w));
  pf->h_len = fmt3(h, pf->h, sizeof(pf->h));
  pf->r_len = fmt3(r, pf->r, sizeof(pf->r));
  pf->r160_len = fmt3(v->r160, pf->r160, sizeof(pf->r160));
  pf->r103_len = fmt3(v->r103, pf->r103, sizeof(pf->r103));
  pf->r075_len = fmt3(v->r075, pf->r075, sizeof(pf->r075));
  pf->r010_len = fmt3(v->r010, pf->r010, sizeof(pf->r010));
  pf->r054_len = fmt3(v->r054, pf->r054, sizeof(pf->r054));
  pf->r020_len = fmt3(v->r020, pf->r020, sizeof(pf->r020));
  pf->r035_len = fmt3(v->r035, pf->r035, sizeof(pf->r035));
  pf->r096_len = fmt3(v->r096, pf->r096, sizeof(pf->r096));

  pf->wm_r160_len = fmt3(w - v->r160, pf->wm_r160, sizeof(pf->wm_r160));
  pf->wm_r103_len = fmt3(w - v->r103, pf->wm_r103, sizeof(pf->wm_r103));
  pf->wm_r075_len = fmt3(w - v->r075, pf->wm_r075, sizeof(pf->wm_r075));
  pf->wm_r054_len = fmt3(w - v->r054, pf->wm_r054, sizeof(pf->wm_r054));
  pf->wm_r035_len = fmt3(w - v->r035, pf->wm_r035, sizeof(pf->wm_r035));
  pf->wm_r020_len = fmt3(w - v->r020, pf->wm_r020, sizeof(pf->wm_r020));
  pf->wm_r010_len = fmt3(w - v->r010, pf->wm_r010, sizeof(pf->wm_r010));

  pf->hm_r160_len = fmt3(h - v->r160, pf->hm_r160, sizeof(pf->hm_r160));
  pf->hm_r103_len = fmt3(h - v->r103, pf->hm_r103, sizeof(pf->hm_r103));
  pf->hm_r075_len = fmt3(h - v->r075, pf->hm_r075, sizeof(pf->hm_r075));
  pf->hm_r054_len = fmt3(h - v->r054, pf->hm_r054, sizeof(pf->hm_r054));
  pf->hm_r035_len = fmt3(h - v->r035, pf->hm_r035, sizeof(pf->hm_r035));
  pf->hm_r020_len = fmt3(h - v->r020, pf->hm_r020, sizeof(pf->hm_r020));
  pf->hm_r010_len = fmt3(h - v->r010, pf->hm_r010, sizeof(pf->hm_r010));
  pf->hm_r096_len = fmt3(h - v->r096, pf->hm_r096, sizeof(pf->hm_r096));
}

static char *build_path_squircle(double w, double h, double r)
{
  RadiusVals v = get_radius_values(r);
  PreFmt pf;
  precompute_fmt(w, h, r, &v, &pf);
  StrBuf sb;
  sb_init(&sb, 2048);

  // 为避免过多格式占位，按段拼接
  SB_APP_LIT(&sb, "M0 ");
  sb_append_len(&sb, pf.r160, pf.r160_len);
  SB_APP_LIT(&sb, " C0 ");
  sb_append_len(&sb, pf.r103, pf.r103_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.r075, pf.r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r010, pf.r010_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r054, pf.r054_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.r020, pf.r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r035, pf.r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r035, pf.r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r020, pf.r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r054, pf.r054_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r010, pf.r010_len);

  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r075, pf.r075_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.r103, pf.r103_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.r160, pf.r160_len);
  SB_APP_LIT(&sb, " 0 H ");
  sb_append_len(&sb, pf.wm_r160, pf.wm_r160_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.wm_r103, pf.wm_r103_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.wm_r075, pf.wm_r075_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.wm_r054, pf.wm_r054_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r010, pf.r010_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.wm_r035, pf.wm_r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r020, pf.r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r020, pf.wm_r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r035, pf.r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r010, pf.wm_r010_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r054, pf.r054_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r075, pf.r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r103, pf.r103_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r160, pf.r160_len);

  SB_APP_LIT(&sb, " V ");
  sb_append_len(&sb, pf.hm_r160, pf.hm_r160_len);
  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r103, pf.hm_r103_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r075, pf.hm_r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r010, pf.wm_r010_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r054, pf.hm_r054_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.wm_r020, pf.wm_r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r035, pf.hm_r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r035, pf.wm_r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r020, pf.hm_r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r054, pf.wm_r054_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r010, pf.hm_r010_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.wm_r075, pf.wm_r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r103, pf.wm_r103_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r160, pf.wm_r160_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);

  SB_APP_LIT(&sb, " H ");
  sb_append_len(&sb, pf.r160, pf.r160_len);
  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.r103, pf.r103_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r075, pf.r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r054, pf.r054_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r010, pf.hm_r010_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.r035, pf.r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r020, pf.hm_r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r020, pf.r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r035, pf.hm_r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r010, pf.r010_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r054, pf.hm_r054_len);

  SB_APP_LIT(&sb, " C 0 ");
  sb_append_len(&sb, pf.hm_r075, pf.hm_r075_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.hm_r103, pf.hm_r103_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.hm_r160, pf.hm_r160_len);
  SB_APP_LIT(&sb, " V ");
  sb_append_len(&sb, pf.r160, pf.r160_len);
  SB_APP_LIT(&sb, " Z");

  return sb.data; // 交由调用者 free()
}

static char *build_path_capsule(double w, double h, double r)
{
  RadiusVals v = get_radius_values(r);
  PreFmt pf;
  precompute_fmt(w, h, r, &v, &pf);
  StrBuf sb;
  sb_init(&sb, 2048);

  SB_APP_LIT(&sb, "M ");
  sb_append_len(&sb, pf.wm_r160, pf.wm_r160_len);
  SB_APP_LIT(&sb, " 0 H ");
  sb_append_len(&sb, pf.r160, pf.r160_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.r103, pf.r103_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.r075, pf.r075_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.r054, pf.r054_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r010, pf.r010_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.r035, pf.r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r020, pf.r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r020, pf.r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r035, pf.r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r010, pf.r010_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r054, pf.r054_len);

  SB_APP_LIT(&sb, " C 0 ");
  sb_append_len(&sb, pf.r075, pf.r075_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.r096, pf.r096_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.r, pf.r_len);

  SB_APP_LIT(&sb, " C 0 ");
  sb_append_len(&sb, pf.hm_r096, pf.hm_r096_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.hm_r075, pf.hm_r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r010, pf.r010_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r054, pf.hm_r054_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.r020, pf.r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r035, pf.hm_r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r035, pf.r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r020, pf.hm_r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r054, pf.r054_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r010, pf.hm_r010_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.r075, pf.r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r103, pf.r103_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r160, pf.r160_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);

  SB_APP_LIT(&sb, " H ");
  sb_append_len(&sb, pf.wm_r160, pf.wm_r160_len);
  SB_APP_LIT(&sb, " H ");
  sb_append_len(&sb, pf.wm_r160, pf.wm_r160_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.wm_r103, pf.wm_r103_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r075, pf.wm_r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.h, pf.h_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r054, pf.wm_r054_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r010, pf.hm_r010_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.wm_r035, pf.wm_r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r020, pf.hm_r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r020, pf.wm_r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r035, pf.hm_r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r010, pf.wm_r010_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r054, pf.hm_r054_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r075, pf.hm_r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.hm_r096, pf.hm_r096_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r, pf.r_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r096, pf.r096_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.w, pf.w_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r075, pf.r075_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r010, pf.wm_r010_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r054, pf.r054_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.wm_r020, pf.wm_r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r035, pf.r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r035, pf.wm_r035_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r020, pf.r020_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.wm_r054, pf.wm_r054_len);
  SB_APP_LIT(&sb, " ");
  sb_append_len(&sb, pf.r010, pf.r010_len);

  SB_APP_LIT(&sb, " C ");
  sb_append_len(&sb, pf.wm_r075, pf.wm_r075_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.wm_r103, pf.wm_r103_len);
  SB_APP_LIT(&sb, " 0 ");
  sb_append_len(&sb, pf.wm_r160, pf.wm_r160_len);
  SB_APP_LIT(&sb, " 0 Z");

  return sb.data;
}

#ifdef __EMSCRIPTEN__
// 为 Wasm 导出：返回指向内部静态缓冲的指针（UTF-8, NUL 终止）
static char g_path_out[8192];

__attribute__((export_name("squircle_path_js")))
uint32_t
squircle_path_js(double w, double h, double r)
{
  char *tmp = build_path_squircle(w, h, r);
  if (!tmp)
    return 0;
  size_t n = strlen(tmp);
  if (n >= sizeof(g_path_out))
    n = sizeof(g_path_out) - 1;
  memcpy(g_path_out, tmp, n);
  g_path_out[n] = '\0';
  free(tmp);
  return (uint32_t)(uintptr_t)g_path_out;
}

__attribute__((export_name("capsule_path_js")))
uint32_t
capsule_path_js(double w, double h, double r)
{
  char *tmp = build_path_capsule(w, h, r);
  if (!tmp)
    return 0;
  size_t n = strlen(tmp);
  if (n >= sizeof(g_path_out))
    n = sizeof(g_path_out) - 1;
  memcpy(g_path_out, tmp, n);
  g_path_out[n] = '\0';
  free(tmp);
  return (uint32_t)(uintptr_t)g_path_out;
}
#endif

static int ieq(const char *a, const char *b)
{
  // 不区分大小写比较
  for (; *a && *b; ++a, ++b)
  {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return 0;
  }
  return *a == '\0' && *b == '\0';
}

static void print_usage(FILE *out)
{
  fprintf(out, "Usage: squircle_svg <shape> <width> <height> <radius>\n");
  fprintf(out, "  <shape>: squircle | capsule\n");
  fprintf(out, "  <width>/<height>/<radius>: number\n");
}

int main(int argc, char **argv)
{
  if (argc != 5)
  {
    print_usage(stderr);
    return 2;
  }
  const char *shape = argv[1];
  char *endp = NULL;
  double w = strtod(argv[2], &endp);
  if (endp == argv[2] || !isfinite(w) || w <= 0)
  {
    fprintf(stderr, "Invalid width\n");
    return 3;
  }
  double h = strtod(argv[3], &endp);
  if (endp == argv[3] || !isfinite(h) || h <= 0)
  {
    fprintf(stderr, "Invalid height\n");
    return 4;
  }
  double r = strtod(argv[4], &endp);
  if (endp == argv[4] || !isfinite(r) || r < 0)
  {
    fprintf(stderr, "Invalid radius\n");
    return 5;
  }

  char *path = NULL;
  if (ieq(shape, "squircle"))
  {
    path = build_path_squircle(w, h, r);
  }
  else if (ieq(shape, "capsule"))
  {
    path = build_path_capsule(w, h, r);
  }
  else
  {
    fprintf(stderr, "Invalid shape: %s\n", shape);
    print_usage(stderr);
    return 1;
  }
  if (!path)
  {
    fprintf(stderr, "Failed to build path\n");
    return 6;
  }

  // 输出纯 SVG path 数据（不含 <svg> 包装）
  fwrite(path, 1, strlen(path), stdout);
  fputc('\n', stdout);

  free(path);
  return 0;
}
