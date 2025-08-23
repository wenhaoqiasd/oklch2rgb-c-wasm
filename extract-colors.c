// 图片主色提取器（设计参考 npm 包 "extract-colors"，但为独立 C 实现，无上游代码拷贝）
// 平台：macOS（使用 CoreGraphics/ImageIO 读取图片）
//
// 功能：
// - 通过子采样控制目标像素预算（默认 64000）
// - 可选的 alpha 像素过滤（默认 alpha > 250）
// - 在 RGB（0..1 归一化）空间进行 K-Means 聚类，KMeans++ 初始化
// - 颜色合并：
//     * 归一化 RGB 距离（0..1，黑白≈1）
//     * H、S、L（HSL）维度的最小差值阈值
// - 输出 JSON 数组，包含：hex、red、green、blue、hue、intensity、lightness、saturation、area
//
// 命令行：
//   extract-colors <image_path>
//       [--pixels N] [--distance D]
//       [--saturationDistance S] [--lightnessDistance L] [--hueDistance H]
//       [--alphaThreshold A] [--maxColors K]
// 默认值与 extract-colors 的行为大体一致：
//   pixels=64000，distance=0.22，saturationDistance=0.2，
//   lightnessDistance=0.2，hueDistance=0.083333333（约 30°），
//   alphaThreshold=250，maxColors=16
//
// 在 macOS 上构建：
//   clang -O2 extract-colors.c -o extract-colors \
//     -framework ImageIO -framework CoreGraphics -framework CoreFoundation
//
// 说明：独立实现，仅参考其设计与输出格式。

#ifndef __EMSCRIPTEN__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

// 微优化辅助宏：分支预测与内联提示
#ifndef LIKELY
#define LIKELY(x) (__builtin_expect(!!(x), 1))
#define UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif
#ifndef INLINE
#define INLINE inline __attribute__((always_inline))
#endif

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

#ifdef ENABLE_OMP
#include <omp.h>
#endif

typedef struct
{
  int pixels;         // 目标样本像素预算
  double distance;    // 用于合并的归一化 RGB 距离阈值
  double satDist;     // 最小饱和度差异
  double lightDist;   // 最小亮度差异
  double hueDist;     // 最小色相弧差（0..1，1==360°）
  int alphaThreshold; // 像素纳入计算所需的最小 alpha（> 此阈值）
  int maxColors;      // K-Means 初始聚类数
} Options;

typedef struct
{
  int width;
  int height;
  uint8_t *rgba; // RGBA8，行优先，步长 stride = width*4
} Image;

typedef struct
{
  float r, g, b; // 0..1 归一化（内部计算使用 float，输出/累计时用 double）
} RGBf;

typedef struct
{
  RGBf color;
  double weight; // 聚类内样本数量（权重）
} Cluster;

// ColorAgg：合并后的颜色及其面积（weight），并缓存 HSL 以避免重复计算
typedef struct
{
  RGBf color;
  double weight;
  double h, s, l; // 缓存的 HSL 值（0..1）
} ColorAgg;

static INLINE double clampd(double x, double lo, double hi)
{
  if (x < lo)
    return lo;
  if (x > hi)
    return hi;
  return x;
}

static int load_image_rgba8(const char *path, Image *out)
{
#ifdef __EMSCRIPTEN__
  (void)path;
  (void)out;
  return 0; // 在 WebAssembly 中不提供文件加载
#else
  memset(out, 0, sizeof(*out));
  CFStringRef cfPath = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
  if (!cfPath)
    return 0;
  CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cfPath, kCFURLPOSIXPathStyle, false);
  CFRelease(cfPath);
  if (!url)
    return 0;

  CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
  CFRelease(url);
  if (!src)
    return 0;

  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
  CFRelease(src);
  if (!img)
    return 0;

  size_t w = CGImageGetWidth(img);
  size_t h = CGImageGetHeight(img);
  if (w == 0 || h == 0)
  {
    CGImageRelease(img);
    return 0;
  }

  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  if (!cs)
    cs = CGColorSpaceCreateDeviceRGB();
  CGBitmapInfo bi = kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big;
  size_t stride = w * 4;
  uint8_t *buf = (uint8_t *)malloc(h * stride);
  if (!buf)
  {
    CGImageRelease(img);
    if (cs)
      CGColorSpaceRelease(cs);
    return 0;
  }

  CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, stride, cs, bi);
  if (!ctx)
  {
    free(buf);
    CGImageRelease(img);
    if (cs)
      CGColorSpaceRelease(cs);
    return 0;
  }
  // 使用默认变换绘制（原点位于左上）
  CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
  CGContextFlush(ctx);

  out->width = (int)w;
  out->height = (int)h;
  out->rgba = buf;

  CGContextRelease(ctx);
  if (cs)
    CGColorSpaceRelease(cs);
  CGImageRelease(img);
  return 1;
#endif
}

static void free_image(Image *im)
{
  if (im && im->rgba)
  {
    free(im->rgba);
    im->rgba = NULL;
  }
}

// 将 RGB（0..1）转换为 H、S、L（0..1）。Hue ∈ [0,1)，S/L ∈ [0,1]。
static INLINE void rgb_to_hsl(double r, double g, double b, double *h, double *s, double *l)
{
  double maxv = fmax(r, fmax(g, b));
  double minv = fmin(r, fmin(g, b));
  double lval = 0.5 * (maxv + minv);
  double sval = 0.0;
  double hval = 0.0;
  if (maxv != minv)
  {
    double d = maxv - minv;
    sval = lval > 0.5 ? d / (2.0 - maxv - minv) : d / (maxv + minv);
    if (maxv == r)
    {
      hval = (g - b) / d + (g < b ? 6.0 : 0.0);
    }
    else if (maxv == g)
    {
      hval = (b - r) / d + 2.0;
    }
    else
    {
      hval = (r - g) / d + 4.0;
    }
    hval /= 6.0; // 归一化到 0..1
  }
  *h = hval;
  *s = sval;
  *l = lval;
}

static INLINE double hue_arc_dist(double h1, double h2)
{
  double d = fabs(h1 - h2);
  if (d > 0.5)
    d = 1.0 - d; // 沿色相环取最短弧长
  return d;
}

// 归一化 RGB 距离，范围 [0,1]，黑白约为 1。
static INLINE double rgb_norm_dist(RGBf a, RGBf b)
{
  double dr = (double)a.r - (double)b.r;
  double dg = (double)a.g - (double)b.g;
  double db = (double)a.b - (double)b.b;
  double e = sqrt(dr * dr + dg * dg + db * db);
  return e / 1.7320508075688772; // sqrt(3)
}

// 不开方、不归一的平方距离（用于“只比较大小”的场景，常数因子无关紧要）
static INLINE double rgb_dist2_raw(RGBf a, RGBf b)
{
  double dr = (double)a.r - (double)b.r;
  double dg = (double)a.g - (double)b.g;
  double db = (double)a.b - (double)b.b;
  return dr * dr + dg * dg + db * db; // 范围 [0, 3]
}

// KMeans++ 初始化：先随机一个中心，再按距离平方加权挑选其余中心
static void kmeans_pp_init(const RGBf *restrict samples, int n, Cluster *restrict clusters, int K)
{
  if (n <= 0 || K <= 0)
    return;
  int first = rand() % n;
  clusters[0].color = samples[first];
  clusters[0].weight = 0.0;

  double *dist2 = (double *)malloc((size_t)n * sizeof(double));
  if (!dist2)
  { // 退化路径：无法分配缓冲时，退回均匀随机选取
    for (int k = 1; k < K; ++k)
    {
      clusters[k].color = samples[rand() % n];
      clusters[k].weight = 0.0;
    }
    return;
  }

  // 使用未归一化的平方距离，避免重复 sqrt
  for (int i = 0; i < n; ++i)
    dist2[i] = rgb_dist2_raw(samples[i], clusters[0].color);

  for (int k = 1; k < K; ++k)
  {
    // 按 dist2 的权重选择新中心
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
      sum += dist2[i];
    if (sum <= 0)
    {
      clusters[k].color = samples[rand() % n];
      clusters[k].weight = 0.0;
      continue;
    }
    double r = ((double)rand() / (double)RAND_MAX) * sum;
    double acc = 0.0;
    int idx = 0;
    for (int i = 0; i < n; ++i)
    {
      acc += dist2[i];
      if (acc >= r)
      {
        idx = i;
        break;
      }
    }
    clusters[k].color = samples[idx];
    clusters[k].weight = 0.0;
    // 更新每个样本到最近中心的 dist2
    for (int i = 0; i < n; ++i)
    {
      double d = rgb_dist2_raw(samples[i], clusters[k].color);
      if (d < dist2[i])
        dist2[i] = d;
    }
  }
  free(dist2);
}

static void kmeans_run(const RGBf *samples, int n, Cluster *clusters, int K, int iters)
{
  if (n <= 0 || K <= 0)
    return;
  int *assign = (int *)malloc((size_t)n * sizeof(int)); // 记录每个样本当前所属簇
  if (!assign)
    return;
  for (int i = 0; i < n; ++i)
    assign[i] = -1;

  // 将簇累加缓冲移出迭代循环，避免反复分配/释放
  double *sr = (double *)calloc((size_t)K, sizeof(double));
  double *sg = (double *)calloc((size_t)K, sizeof(double));
  double *sb = (double *)calloc((size_t)K, sizeof(double));
  // SIMD 辅助：中心通道数组（float）
  float *cr = (float *)malloc((size_t)K * sizeof(float));
  float *cg = (float *)malloc((size_t)K * sizeof(float));
  float *cb = (float *)malloc((size_t)K * sizeof(float));
  // 记录每个样本到最近中心的平方距离，用于空簇重置
  double *bestd2 = (double *)malloc((size_t)n * sizeof(double));
  if (!sr || !sg || !sb)
  {
    if (sr)
      free(sr);
    if (sg)
      free(sg);
    if (sb)
      free(sb);
    free(assign);
    return;
  }
  if (!cr || !cg || !cb || !bestd2)
  {
    if (cr)
      free(cr);
    if (cg)
      free(cg);
    if (cb)
      free(cb);
    if (bestd2)
      free(bestd2);
    free(sr);
    free(sg);
    free(sb);
    free(assign);
    return;
  }

  // 初始化中心通道数组
  for (int k = 0; k < K; ++k)
  {
    cr[k] = clusters[k].color.r;
    cg[k] = clusters[k].color.g;
    cb[k] = clusters[k].color.b;
  }

  for (int it = 0; it < iters; ++it)
  {
    int changed = 0;

#ifndef ENABLE_OMP
    // ---- 串行路径：与原实现一致 ----
    // 重置各簇的累加器
    for (int k = 0; k < K; ++k)
    {
      clusters[k].weight = 0.0;
      sr[k] = sg[k] = sb[k] = 0.0;
    }

    // 赋值阶段：将样本分配到最近的中心（比较平方距离，避免 sqrt）
    for (int i = 0; i < n; ++i)
    {
      double best = 1e300;
      int bi = 0;
#if defined(__wasm_simd128__)
      // Wasm SIMD：一次比较 4 个中心
      const float sr0 = samples[i].r;
      const float sg0 = samples[i].g;
      const float sb0v = samples[i].b;
      v128_t vr = wasm_f32x4_splat(sr0);
      v128_t vg = wasm_f32x4_splat(sg0);
      v128_t vbv = wasm_f32x4_splat(sb0v);
      int k = 0;
      for (; k + 4 <= K; k += 4)
      {
        v128_t crv = wasm_v128_load(&cr[k]);
        v128_t cgv = wasm_v128_load(&cg[k]);
        v128_t cbv4 = wasm_v128_load(&cb[k]);
        v128_t dr = wasm_f32x4_sub(vr, crv);
        v128_t dg = wasm_f32x4_sub(vg, cgv);
        v128_t dbv = wasm_f32x4_sub(vbv, cbv4);
        v128_t d2v = wasm_f32x4_add(
            wasm_f32x4_mul(dr, dr),
            wasm_f32x4_add(wasm_f32x4_mul(dg, dg), wasm_f32x4_mul(dbv, dbv)));
        float d0 = wasm_f32x4_extract_lane(d2v, 0);
        if ((double)d0 < best)
        {
          best = (double)d0;
          bi = k + 0;
        }
        float d1 = wasm_f32x4_extract_lane(d2v, 1);
        if ((double)d1 < best)
        {
          best = (double)d1;
          bi = k + 1;
        }
        float d2 = wasm_f32x4_extract_lane(d2v, 2);
        if ((double)d2 < best)
        {
          best = (double)d2;
          bi = k + 2;
        }
        float d3 = wasm_f32x4_extract_lane(d2v, 3);
        if ((double)d3 < best)
        {
          best = (double)d3;
          bi = k + 3;
        }
      }
      for (; k < K; ++k)
      {
        double d2s = rgb_dist2_raw(samples[i], clusters[k].color);
        if (d2s < best)
        {
          best = d2s;
          bi = k;
        }
      }
#else
      for (int k = 0; k < K; ++k)
      {
        double d2s = rgb_dist2_raw(samples[i], clusters[k].color);
        if (d2s < best)
        {
          best = d2s;
          bi = k;
        }
      }
#endif
      bestd2[i] = best;
      if (assign[i] != bi)
      {
        assign[i] = bi;
        changed = 1;
      }
      sr[bi] += (double)samples[i].r;
      sg[bi] += (double)samples[i].g;
      sb[bi] += (double)samples[i].b;
      clusters[bi].weight += 1.0;
    }

    // 稳定性微调：空簇重置到最远样本
    for (int k = 0; k < K; ++k)
    {
      if (clusters[k].weight > 0.0)
        continue;
      // 找到最远样本
      int farIdx = -1;
      double farD2 = -1.0;
      for (int i = 0; i < n; ++i)
      {
        if (bestd2[i] > farD2)
        {
          farD2 = bestd2[i];
          farIdx = i;
        }
      }
      if (farIdx >= 0)
      {
        int old = assign[farIdx];
        if (old >= 0 && old < K)
        {
          sr[old] -= (double)samples[farIdx].r;
          sg[old] -= (double)samples[farIdx].g;
          sb[old] -= (double)samples[farIdx].b;
          if (clusters[old].weight > 0.0)
            clusters[old].weight -= 1.0;
        }
        assign[farIdx] = k;
        sr[k] += (double)samples[farIdx].r;
        sg[k] += (double)samples[farIdx].g;
        sb[k] += (double)samples[farIdx].b;
        clusters[k].weight += 1.0;
        changed = 1;
      }
    }

    // 更新阶段：用簇内均值更新中心
    for (int k = 0; k < K; ++k)
    {
      if (clusters[k].weight > 0.0)
      {
        clusters[k].color.r = (float)(sr[k] / clusters[k].weight);
        clusters[k].color.g = (float)(sg[k] / clusters[k].weight);
        clusters[k].color.b = (float)(sb[k] / clusters[k].weight);
      }
      // 刷新中心通道数组（供下一轮 SIMD 使用）
      cr[k] = clusters[k].color.r;
      cg[k] = clusters[k].color.g;
      cb[k] = clusters[k].color.b;
    }

#else // ENABLE_OMP
    // ---- 并行路径（OpenMP，可选）：先并行计算 assign/bestd2，再串行计数与累加 ----
    // 重置各簇的累加器与权重（累加在后续串行阶段进行）
    for (int k = 0; k < K; ++k)
    {
      clusters[k].weight = 0.0;
      sr[k] = sg[k] = sb[k] = 0.0;
    }

    int changed_any = 0;
// 并行计算：最近中心与 bestd2
#pragma omp parallel for schedule(static) reduction(| : changed_any)
    for (int i = 0; i < n; ++i)
    {
      double best = 1e300;
      int bi = 0;
#if defined(__wasm_simd128__)
      const float sr0 = samples[i].r;
      const float sg0 = samples[i].g;
      const float sb0v = samples[i].b;
      v128_t vr = wasm_f32x4_splat(sr0);
      v128_t vg = wasm_f32x4_splat(sg0);
      v128_t vbv = wasm_f32x4_splat(sb0v);
      int k = 0;
      for (; k + 4 <= K; k += 4)
      {
        v128_t crv = wasm_v128_load(&cr[k]);
        v128_t cgv = wasm_v128_load(&cg[k]);
        v128_t cbv4 = wasm_v128_load(&cb[k]);
        v128_t dr = wasm_f32x4_sub(vr, crv);
        v128_t dg = wasm_f32x4_sub(vg, cgv);
        v128_t dbv = wasm_f32x4_sub(vbv, cbv4);
        v128_t d2v = wasm_f32x4_add(
            wasm_f32x4_mul(dr, dr),
            wasm_f32x4_add(wasm_f32x4_mul(dg, dg), wasm_f32x4_mul(dbv, dbv)));
        float d0 = wasm_f32x4_extract_lane(d2v, 0);
        if ((double)d0 < best)
        {
          best = (double)d0;
          bi = k + 0;
        }
        float d1 = wasm_f32x4_extract_lane(d2v, 1);
        if ((double)d1 < best)
        {
          best = (double)d1;
          bi = k + 1;
        }
        float d2 = wasm_f32x4_extract_lane(d2v, 2);
        if ((double)d2 < best)
        {
          best = (double)d2;
          bi = k + 2;
        }
        float d3 = wasm_f32x4_extract_lane(d2v, 3);
        if ((double)d3 < best)
        {
          best = (double)d3;
          bi = k + 3;
        }
      }
      for (; k < K; ++k)
      {
        double d2s = rgb_dist2_raw(samples[i], clusters[k].color);
        if (d2s < best)
        {
          best = d2s;
          bi = k;
        }
      }
#else
      for (int k = 0; k < K; ++k)
      {
        double d2s = rgb_dist2_raw(samples[i], clusters[k].color);
        if (d2s < best)
        {
          best = d2s;
          bi = k;
        }
      }
#endif
      bestd2[i] = best;
      if (assign[i] != bi)
      {
        assign[i] = bi;
        changed_any = 1;
      }
      else
      {
        // 仍然写回（保持一致性）
        assign[i] = bi;
      }
    }
    changed = changed_any;

    // 计数每个簇的样本数
    int *counts = (int *)calloc((size_t)K, sizeof(int));
    if (!counts)
    {
      // 回退：若内存不足，认为未变化并提前退出
      break;
    }
    for (int i = 0; i < n; ++i)
    {
      int k = assign[i];
      if ((unsigned)k < (unsigned)K)
        counts[k]++;
    }

    // 稳定性微调：将空簇重置到“当前最远”样本
    for (int k = 0; k < K; ++k)
    {
      if (counts[k] > 0)
        continue;
      int farIdx = -1;
      double farD2 = -1.0;
      for (int i = 0; i < n; ++i)
      {
        if (bestd2[i] > farD2)
        {
          farD2 = bestd2[i];
          farIdx = i;
        }
      }
      if (farIdx >= 0)
      {
        int old = assign[farIdx];
        if ((unsigned)old < (unsigned)K && counts[old] > 0)
          counts[old]--;
        assign[farIdx] = k;
        counts[k]++;
        changed = 1;
      }
    }

    // 串行累加：按最终 assign 计算簇累加与权重
    for (int i = 0; i < n; ++i)
    {
      int k = assign[i];
      if ((unsigned)k < (unsigned)K)
      {
        sr[k] += (double)samples[i].r;
        sg[k] += (double)samples[i].g;
        sb[k] += (double)samples[i].b;
      }
    }
    for (int k = 0; k < K; ++k)
      clusters[k].weight = (double)counts[k];

    free(counts);

    // 更新阶段：用簇内均值更新中心，并刷新 SIMD 通道缓冲
    for (int k = 0; k < K; ++k)
    {
      if (clusters[k].weight > 0.0)
      {
        clusters[k].color.r = (float)(sr[k] / clusters[k].weight);
        clusters[k].color.g = (float)(sg[k] / clusters[k].weight);
        clusters[k].color.b = (float)(sb[k] / clusters[k].weight);
      }
      cr[k] = clusters[k].color.r;
      cg[k] = clusters[k].color.g;
      cb[k] = clusters[k].color.b;
    }
#endif // ENABLE_OMP

    if (!changed)
      break;
  }
  free(sr);
  free(sg);
  free(sb);
  free(cr);
  free(cg);
  free(cb);
  free(bestd2);
  free(assign);
}

static int cmp_cluster_weight_desc(const void *a, const void *b)
{
  const Cluster *A = (const Cluster *)a;
  const Cluster *B = (const Cluster *)b;
  if (A->weight < B->weight)
    return 1;
  if (A->weight > B->weight)
    return -1;
  return 0;
}

static void merge_colors(const Cluster *clusters, int K, double totalWeight, const Options *opt,
                         ColorAgg **outArr, int *outN)
{
  // 按权重（面积）从大到小排序
  Cluster *sorted = (Cluster *)malloc((size_t)K * sizeof(Cluster));
  memcpy(sorted, clusters, (size_t)K * sizeof(Cluster));
  qsort(sorted, (size_t)K, sizeof(Cluster), cmp_cluster_weight_desc);

  ColorAgg *acc = (ColorAgg *)malloc((size_t)K * sizeof(ColorAgg));
  int m = 0;
  const double rgb_thresh2 = opt->distance * opt->distance * 3.0; // 归一化阈值换算为平方距离

  for (int i = 0; i < K; ++i)
  {
    if (sorted[i].weight <= 0.0)
      continue;
    RGBf c = sorted[i].color;
    double w = sorted[i].weight;
    // 为待合并颜色预计算 HSL
    double hi, si, li;
    rgb_to_hsl((double)c.r, (double)c.g, (double)c.b, &hi, &si, &li);

    // 尝试合并到已有的颜色桶
    int merged = 0;
    for (int j = 0; j < m; ++j)
    {
      // RGB 空间距离（比较平方距离，阈值也换算为平方尺度）
      // 归一化阈值 distance 定义在 e_norm = e / sqrt(3)，比较 e_norm <= distance
      // 等价于比较 e^2 <= distance^2 * 3
      double hd = hue_arc_dist(hi, acc[j].h);
      double sd = fabs(si - acc[j].s);
      double ld = fabs(li - acc[j].l);

      if (rgb_dist2_raw(c, acc[j].color) <= rgb_thresh2 ||
          (sd < opt->satDist && ld < opt->lightDist && hd < opt->hueDist))
      {
        // 加权合并到 acc[j]
        double tw = acc[j].weight + w;
        if (tw > 0.0)
        {
          acc[j].color.r = (float)(((double)acc[j].color.r * acc[j].weight + (double)c.r * w) / tw);
          acc[j].color.g = (float)(((double)acc[j].color.g * acc[j].weight + (double)c.g * w) / tw);
          acc[j].color.b = (float)(((double)acc[j].color.b * acc[j].weight + (double)c.b * w) / tw);
        }
        acc[j].weight = tw;
        // 更新缓存的 HSL
        rgb_to_hsl((double)acc[j].color.r, (double)acc[j].color.g, (double)acc[j].color.b, &acc[j].h, &acc[j].s, &acc[j].l);
        merged = 1;
        break;
      }
    }
    if (!merged)
    {
      acc[m].color = c;
      acc[m].weight = w;
      acc[m].h = hi;
      acc[m].s = si;
      acc[m].l = li;
      m++;
    }
  }

  free(sorted);

  // 将权重归一化为面积占比（0..1）
  for (int i = 0; i < m; ++i)
  {
    acc[i].weight = (totalWeight > 0.0) ? (acc[i].weight / totalWeight) : 0.0;
  }

  *outArr = acc;
  *outN = m;
}

static void print_hex_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
  static const char *hex = "0123456789abcdef";
  char buf[7];
  buf[0] = hex[(r >> 4) & 0xF];
  buf[1] = hex[r & 0xF];
  buf[2] = hex[(g >> 4) & 0xF];
  buf[3] = hex[g & 0xF];
  buf[4] = hex[(b >> 4) & 0xF];
  buf[5] = hex[b & 0xF];
  buf[6] = '\0';
  printf("\"#%s\"", buf);
}

// 从原始 RGBA 像素缓冲与尺寸进行取色（核心逻辑）
static int extract_colors_core(const uint8_t *rgba, int w, int h, const Options *opt,
                               ColorAgg **outAgg, int *outM)
{
  const uint8_t *px = rgba;
  if (w <= 0 || h <= 0 || !px || !outAgg || !outM)
    return 0;

  // 子采样步长：使采样点数量约等于 opt->pixels
  long long total = (long long)w * (long long)h;
  int step = 1;
  if (total > opt->pixels && opt->pixels > 0)
  {
    double ratio = sqrt((double)total / (double)opt->pixels);
    step = (int)ceil(ratio);
    if (step < 1)
      step = 1;
  }

  // 收集采样点
  int cap = (w / step + 1) * (h / step + 1);
  RGBf *samples = (RGBf *)malloc((size_t)cap * sizeof(RGBf));
  int n = 0;
  for (int y = 0; y < h; y += step)
  {
    const uint8_t *row = px + (size_t)y * (size_t)w * 4;
    for (int x = 0; x < w; x += step)
    {
      const uint8_t *p = row + (size_t)x * 4;
      uint8_t r = p[0], g = p[1], b = p[2], a = p[3];
      if (UNLIKELY(a <= opt->alphaThreshold))
        continue; // 默认像素过滤：alpha > 250
      samples[n].r = (float)r * (1.0f / 255.0f);
      samples[n].g = (float)g * (1.0f / 255.0f);
      samples[n].b = (float)b * (1.0f / 255.0f);
      n++;
    }
  }
  if (n == 0)
  {
    free(samples);
    *outAgg = NULL;
    *outM = 0;
    return 1;
  }

  int K = opt->maxColors;
  if (K > n)
    K = n;
  if (K <= 0)
    K = 1;
  Cluster *clusters = (Cluster *)malloc((size_t)K * sizeof(Cluster));
  if (!clusters)
  {
    free(samples);
    return 0;
  }

  srand((unsigned int)time(NULL));
  kmeans_pp_init(samples, n, clusters, K);
  kmeans_run(samples, n, clusters, K, 12);

  double totalW = 0.0;
  for (int k = 0; k < K; ++k)
    totalW += clusters[k].weight;
  ColorAgg *agg = NULL;
  int m = 0;
  merge_colors(clusters, K, totalW, opt, &agg, &m);

  free(samples);
  free(clusters);
  *outAgg = agg;
  *outM = m;
  return 1;
}

#ifndef __EMSCRIPTEN__
// 从 Image 取色并输出 JSON（原生 CLI 用）
static int extract_colors_from_image(const Image *im, const Options *opt)
{
  ColorAgg *agg = NULL;
  int m = 0;
  if (!extract_colors_core(im->rgba, im->width, im->height, opt, &agg, &m))
    return 0;
  // 输出 JSON 数组
  printf("[\n");
  for (int i = 0; i < m; ++i)
  {
    RGBf c = agg[i].color;
    // 直接使用缓存的 HSL，避免重复计算
    double h_ = agg[i].h, s_ = agg[i].s, l_ = agg[i].l;
    int R = (int)lround(clampd((double)c.r, 0.0, 1.0) * 255.0);
    int G = (int)lround(clampd((double)c.g, 0.0, 1.0) * 255.0);
    int B = (int)lround(clampd((double)c.b, 0.0, 1.0) * 255.0);
    double intensity = ((double)c.r + (double)c.g + (double)c.b) / 3.0; // 0..1

    printf("  { ");
    printf("\"hex\": ");
    print_hex_from_rgb((uint8_t)R, (uint8_t)G, (uint8_t)B);
    printf(", ");
    printf("\"red\": %d, \"green\": %d, \"blue\": %d, ", R, G, B);
    printf("\"hue\": %.10g, ", h_);
    printf("\"intensity\": %.10g, ", intensity);
    printf("\"lightness\": %.10g, ", l_);
    printf("\"saturation\": %.10g, ", s_);
    printf("\"area\": %.10g ", agg[i].weight);
    printf("}%s\n", (i + 1 < m) ? "," : "");
  }
  printf("]\n");
  free(agg);
  return 1;
}
#endif

#ifdef __EMSCRIPTEN__
// Emscripten 运行时：确保导出符号不会被裁剪
#include <emscripten/emscripten.h>
// ---- Wasm 导出：像素缓冲与结果缓冲 ----
static uint8_t *g_pixels_buf = NULL;
static size_t g_pixels_cap = 0;

// 返回一段至少 size 字节的 RGBA 写入缓冲指针（线性内存地址）
EMSCRIPTEN_KEEPALIVE __attribute__((export_name("get_pixels_buffer")))
uint32_t
get_pixels_buffer(uint32_t size)
{
  if (size > g_pixels_cap)
  {
    // 重新分配
    uint8_t *nbuf = (uint8_t *)realloc(g_pixels_buf, size);
    if (!nbuf)
      return 0;
    g_pixels_buf = nbuf;
    g_pixels_cap = size;
  }
  return (uint32_t)(uintptr_t)g_pixels_buf;
}

// 结果按 double 打包：
// out[0] = 颜色数量 M（double）
// 紧随其后每个颜色 8 个 double：
//   [R(0..255), G(0..255), B(0..255), hue(0..1), intensity(0..1), lightness(0..1), saturation(0..1), area(0..1)]
// 固定最大颜色数上限（避免 JS 端管理内存）：
#define EXTRACT_MAX_OUT_COLORS 64
static double g_out_buf[1 + EXTRACT_MAX_OUT_COLORS * 8];

static void pack_results_to_out(const ColorAgg *agg, int m)
{
  if (m > EXTRACT_MAX_OUT_COLORS)
    m = EXTRACT_MAX_OUT_COLORS;
  g_out_buf[0] = (double)m;
  for (int i = 0; i < m; ++i)
  {
    RGBf c = agg[i].color;
    // 直接使用缓存的 HSL，避免重复计算
    double h_ = agg[i].h, s_ = agg[i].s, l_ = agg[i].l;
    double R = clampd((double)c.r, 0.0, 1.0) * 255.0;
    double G = clampd((double)c.g, 0.0, 1.0) * 255.0;
    double B = clampd((double)c.b, 0.0, 1.0) * 255.0;
    double intensity = ((double)c.r + (double)c.g + (double)c.b) / 3.0;
    size_t base = 1 + (size_t)i * 8;
    g_out_buf[base + 0] = R;
    g_out_buf[base + 1] = G;
    g_out_buf[base + 2] = B;
    g_out_buf[base + 3] = h_;
    g_out_buf[base + 4] = intensity;
    g_out_buf[base + 5] = l_;
    g_out_buf[base + 6] = s_;
    g_out_buf[base + 7] = agg[i].weight; // area
  }
}

// 从 RGBA 指针取色，返回结果缓冲的线性内存地址
EMSCRIPTEN_KEEPALIVE __attribute__((export_name("extract_colors_from_rgba_js")))
uint32_t
extract_colors_from_rgba_js(uint32_t rgba_ptr, int width, int height,
                            int pixels, double distance, double satDist,
                            double lightDist, double hueDist,
                            int alphaThreshold, int maxColors)
{
  Options opt;
  opt.pixels = pixels > 0 ? pixels : 64000;
  opt.distance = distance;
  opt.satDist = satDist;
  opt.lightDist = lightDist;
  opt.hueDist = hueDist;
  opt.alphaThreshold = alphaThreshold;
  opt.maxColors = (maxColors > 0 && maxColors <= EXTRACT_MAX_OUT_COLORS) ? maxColors : 16;

  ColorAgg *agg = NULL;
  int m = 0;
  const uint8_t *rgba = (const uint8_t *)(uintptr_t)rgba_ptr;
  if (!extract_colors_core(rgba, width, height, &opt, &agg, &m))
    return 0;
  pack_results_to_out(agg, m);
  free(agg);
  return (uint32_t)(uintptr_t)g_out_buf;
}
#endif // __EMSCRIPTEN__

static void print_usage(const char *prog)
{
  fprintf(stderr,
          "Usage:\n"
          "  %s <image_path> [--pixels N] [--distance D] [--saturationDistance S]\n"
          "                 [--lightnessDistance L] [--hueDistance H] [--alphaThreshold A]\n"
          "                 [--maxColors K]\n\n"
          "Defaults: pixels=64000, distance=0.22, saturationDistance=0.2, lightnessDistance=0.2,\n"
          "          hueDistance=0.083333333 (~30deg), alphaThreshold=250, maxColors=16\n",
          prog);
}

#ifndef __EMSCRIPTEN__
int main(int argc, char **argv)
{
  if (argc < 2)
  {
    print_usage(argv[0]);
    return 1;
  }
  const char *imagePath = NULL;
  Options opt;
  opt.pixels = 64000;
  opt.distance = 0.22;
  opt.satDist = 0.2;
  opt.lightDist = 0.2;
  opt.hueDist = 0.083333333; // ~30 degrees
  opt.alphaThreshold = 250;
  opt.maxColors = 16;

  // parse args
  for (int i = 1; i < argc; ++i)
  {
    const char *a = argv[i];
    if (a[0] == '-')
    {
      if (strcmp(a, "--pixels") == 0 && i + 1 < argc)
      {
        opt.pixels = atoi(argv[++i]);
        continue;
      }
      if (strcmp(a, "--distance") == 0 && i + 1 < argc)
      {
        opt.distance = atof(argv[++i]);
        continue;
      }
      if (strcmp(a, "--saturationDistance") == 0 && i + 1 < argc)
      {
        opt.satDist = atof(argv[++i]);
        continue;
      }
      if (strcmp(a, "--lightnessDistance") == 0 && i + 1 < argc)
      {
        opt.lightDist = atof(argv[++i]);
        continue;
      }
      if (strcmp(a, "--hueDistance") == 0 && i + 1 < argc)
      {
        opt.hueDist = atof(argv[++i]);
        continue;
      }
      if (strcmp(a, "--alphaThreshold") == 0 && i + 1 < argc)
      {
        opt.alphaThreshold = atoi(argv[++i]);
        continue;
      }
      if (strcmp(a, "--maxColors") == 0 && i + 1 < argc)
      {
        opt.maxColors = atoi(argv[++i]);
        continue;
      }
      fprintf(stderr, "Unknown or incomplete option: %s\n", a);
      print_usage(argv[0]);
      return 1;
    }
    else
    {
      if (!imagePath)
        imagePath = a;
      else
      {
        fprintf(stderr, "Unexpected positional argument: %s\n", a);
        print_usage(argv[0]);
        return 1;
      }
    }
  }

  if (!imagePath)
  {
    print_usage(argv[0]);
    return 1;
  }

  Image im;
  if (!load_image_rgba8(imagePath, &im))
  {
    fprintf(stderr, "Failed to load image: %s\n", imagePath);
    return 1;
  }

  int ok = extract_colors_from_image(&im, &opt);
  free_image(&im);
  return ok ? 0 : 2;
}
#endif
