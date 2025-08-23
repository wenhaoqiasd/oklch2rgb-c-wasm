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
  double r, g, b; // 0..1 归一化
} RGBf;

typedef struct
{
  RGBf color;
  double weight; // 聚类内样本数量（权重）
} Cluster;

static double clampd(double x, double lo, double hi)
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
static void rgb_to_hsl(double r, double g, double b, double *h, double *s, double *l)
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

static double hue_arc_dist(double h1, double h2)
{
  double d = fabs(h1 - h2);
  if (d > 0.5)
    d = 1.0 - d; // 沿色相环取最短弧长
  return d;
}

// 归一化 RGB 距离，范围 [0,1]，黑白约为 1。
static double rgb_norm_dist(RGBf a, RGBf b)
{
  double dr = a.r - b.r;
  double dg = a.g - b.g;
  double db = a.b - b.b;
  double e = sqrt(dr * dr + dg * dg + db * db);
  return e / 1.7320508075688772; // sqrt(3)
}

// KMeans++ 初始化：先随机一个中心，再按距离平方加权挑选其余中心
static void kmeans_pp_init(const RGBf *samples, int n, Cluster *clusters, int K)
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

  for (int i = 0; i < n; ++i)
    dist2[i] = rgb_norm_dist(samples[i], clusters[0].color);
  for (int i = 0; i < n; ++i)
    dist2[i] = dist2[i] * dist2[i];

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
      double d = rgb_norm_dist(samples[i], clusters[k].color);
      d = d * d;
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

  for (int it = 0; it < iters; ++it)
  {
    int changed = 0;
    // 重置各簇的累加器
    for (int k = 0; k < K; ++k)
    {
      clusters[k].weight = 0.0;
      clusters[k].color.r = clusters[k].color.r;
    }
    double *sr = (double *)calloc((size_t)K, sizeof(double));
    double *sg = (double *)calloc((size_t)K, sizeof(double));
    double *sb = (double *)calloc((size_t)K, sizeof(double));
    if (!sr || !sg || !sb)
    {
      if (sr)
        free(sr);
      if (sg)
        free(sg);
      if (sb)
        free(sb);
      break;
    }

    // 赋值阶段：将样本分配到最近的中心
    for (int i = 0; i < n; ++i)
    {
      double best = 1e9;
      int bi = 0;
      for (int k = 0; k < K; ++k)
      {
        double d = rgb_norm_dist(samples[i], clusters[k].color);
        if (d < best)
        {
          best = d;
          bi = k;
        }
      }
      if (assign[i] != bi)
      {
        assign[i] = bi;
        changed = 1;
      }
      sr[bi] += samples[i].r;
      sg[bi] += samples[i].g;
      sb[bi] += samples[i].b;
      clusters[bi].weight += 1.0;
    }

    // 更新阶段：用簇内均值更新中心
    for (int k = 0; k < K; ++k)
    {
      if (clusters[k].weight > 0.0)
      {
        clusters[k].color.r = sr[k] / clusters[k].weight;
        clusters[k].color.g = sg[k] / clusters[k].weight;
        clusters[k].color.b = sb[k] / clusters[k].weight;
      }
    }

    free(sr);
    free(sg);
    free(sb);
    if (!changed)
      break;
  }
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

typedef struct
{
  RGBf color;
  double weight;
} ColorAgg;

static void merge_colors(const Cluster *clusters, int K, double totalWeight, const Options *opt,
                         ColorAgg **outArr, int *outN)
{
  // 按权重（面积）从大到小排序
  Cluster *sorted = (Cluster *)malloc((size_t)K * sizeof(Cluster));
  memcpy(sorted, clusters, (size_t)K * sizeof(Cluster));
  qsort(sorted, (size_t)K, sizeof(Cluster), cmp_cluster_weight_desc);

  ColorAgg *acc = (ColorAgg *)malloc((size_t)K * sizeof(ColorAgg));
  int m = 0;

  for (int i = 0; i < K; ++i)
  {
    if (sorted[i].weight <= 0.0)
      continue;
    RGBf c = sorted[i].color;
    double w = sorted[i].weight;

    // 尝试合并到已有的颜色桶
    int merged = 0;
    for (int j = 0; j < m; ++j)
    {
      // RGB 空间距离
      double d = rgb_norm_dist(c, acc[j].color);
      // HSL 各维度的差异
      double h1, s1, l1, h2, s2, l2;
      rgb_to_hsl(c.r, c.g, c.b, &h1, &s1, &l1);
      rgb_to_hsl(acc[j].color.r, acc[j].color.g, acc[j].color.b, &h2, &s2, &l2);
      double hd = hue_arc_dist(h1, h2);
      double sd = fabs(s1 - s2);
      double ld = fabs(l1 - l2);

      if (d <= opt->distance || (sd < opt->satDist && ld < opt->lightDist && hd < opt->hueDist))
      {
        // 加权合并到 acc[j]
        double tw = acc[j].weight + w;
        if (tw > 0.0)
        {
          acc[j].color.r = (acc[j].color.r * acc[j].weight + c.r * w) / tw;
          acc[j].color.g = (acc[j].color.g * acc[j].weight + c.g * w) / tw;
          acc[j].color.b = (acc[j].color.b * acc[j].weight + c.b * w) / tw;
        }
        acc[j].weight = tw;
        merged = 1;
        break;
      }
    }
    if (!merged)
    {
      acc[m].color = c;
      acc[m].weight = w;
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
      if (a <= opt->alphaThreshold)
        continue; // 默认像素过滤：alpha > 250
      samples[n].r = r / 255.0;
      samples[n].g = g / 255.0;
      samples[n].b = b / 255.0;
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
    double h_, s_, l_;
    rgb_to_hsl(c.r, c.g, c.b, &h_, &s_, &l_);
    int R = (int)lround(clampd(c.r, 0.0, 1.0) * 255.0);
    int G = (int)lround(clampd(c.g, 0.0, 1.0) * 255.0);
    int B = (int)lround(clampd(c.b, 0.0, 1.0) * 255.0);
    double intensity = (c.r + c.g + c.b) / 3.0; // 0..1

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
    double h_, s_, l_;
    rgb_to_hsl(c.r, c.g, c.b, &h_, &s_, &l_);
    double R = clampd(c.r, 0.0, 1.0) * 255.0;
    double G = clampd(c.g, 0.0, 1.0) * 255.0;
    double B = clampd(c.b, 0.0, 1.0) * 255.0;
    double intensity = (c.r + c.g + c.b) / 3.0;
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
