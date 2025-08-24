# RGB ↔ OKLCH 数字转换器

两个用 C 实现的命令行小工具，在 sRGB 与 OKLCH 之间互转，均采用“数字入/数字出”的简单接口。

## 编译

```zsh
gcc -O2 rgb2oklch.c -o rgb2oklch
gcc -O2 oklch2rgb.c -o oklch2rgb
```

## 工具 1：rgb2oklch

- 输入：三参数 R G B（0–255 数字，按 sRGB 8 位通道解释）
- 输出：三数字 `L C h`
  - L ∈ [0,1]
  - C ≥ 0
  - h 为角度数值（度）；当 C≈0 时强制 h=0

示例：

```zsh
./rgb2oklch 255 255 255
# 1 0 0

./rgb2oklch 128 100 231
# 0.597263 0.190026 289.184618
```

## 工具 2：oklch2rgb

- 输入：三参数 L C h（L∈[0,1]、C≥0、h 为度数 0–360）
- 输出：三数字 `R G B`（0–255 的整数，sRGB 编码并夹紧）

示例：

```zsh
./oklch2rgb 0.5964 0.1899 289.06
# 128 100 231
```

相对色度（Relative chroma，可选第四参 `rel`，范围 [0,1]）：

- 当提供 `rel` 时，会忽略输入的 `C`，转而计算 `C = rel * Cmax(L, h)`，其中 `Cmax(L,h)` 为在 sRGB 色域内可达的最大色度；随后仍会进行一次色域安全收敛以保证线性 sRGB ∈ [0,1]。
- 行为检查示例：

```zsh
# rel=0 得到同亮度的灰色
./oklch2rgb 0.7 0.2 40 0   # -> 158 158 158（示例数值）

# rel=1 得到该 L/h 下的最大可显示色度
./oklch2rgb 0.7 0.2 40 1   # -> 255 104 44（示例数值）

# rel 介于 0 和 1 之间得到渐进的去饱和
./oklch2rgb 0.7 999 40 0.5 # -> 211 137 111（示例数值）
```

## 往返验证（可选）

```zsh
./rgb2oklch 128 100 231 | awk '{print $1, $2, $3}' | xargs ./oklch2rgb
# 128 100 231
```

## 工具 3：extract-colors（图片主色/调色板提取）

- 输入：图片路径（CLI），或在浏览器端传入 URL/HTMLImageElement/ImageData。
- 输出：JSON 数组，每个条目包含：
  - `hex`, `red`, `green`, `blue`, `hue`, `intensity`, `lightness`, `saturation`, `area`

macOS 下编译 CLI（使用 CoreGraphics/ImageIO 读取图片）：

```zsh
clang -O2 extract-colors.c -o extract-colors \
  -framework ImageIO -framework CoreGraphics -framework CoreFoundation
```

命令行用法（默认与 Namide/extract-colors 接近）：

```zsh
./extract-colors <image_path> \
  [--pixels N] [--distance D] \
  [--saturationDistance S] [--lightnessDistance L] [--hueDistance H] \
  [--alphaThreshold A] [--maxColors K]

# 默认：pixels=64000, distance=0.22, saturationDistance=0.2,
#       lightnessDistance=0.2, hueDistance≈1/12(30°), alphaThreshold=250, maxColors=16
```

示例（输出为 JSON 数组）：

```zsh
./extract-colors m.png | jq .[0]
```

## 说明

- 转换基于 OKLab/OKLCH 参考实现（Björn Ottosson）。
- 仅使用 sRGB 色彩空间与标准传输函数。
- 输出为纯数字便于脚本处理；若需固定小数位或 JSON 格式，可在源码中调整打印逻辑。

以上三个工具均先用 C 实现核心算法，再编译为 WebAssembly 用于网页端最小实践（纯原生 JS 加载，无 Emscripten 胶水脚本）。

## 使用 Makefile 一键构建与测试

本仓库已提供顶层 `Makefile`，常用命令：

```zsh
# 构建本地可执行文件 + 构建三份 WASM + 运行最小烟测
make all

# 仅构建本地可执行文件（macOS）
make native

# 仅构建 WASM（需要 emcc 在 PATH 中）
make wasm

# 运行最小烟测（依赖已构建好的本地可执行文件）
make test

# 清理产物（本地二进制与 wasm 文件）
make clean
```

说明：

- 本地构建使用 `clang -O3 -ffast-math -std=c11`（`oklch2rgb/rgb2oklch` 还带 `-march=native`）。
- `extract-colors` 本地构建依赖 macOS Frameworks：ImageIO、CoreGraphics、CoreFoundation。
- WASM 构建采用独立 `.wasm`（`-s STANDALONE_WASM=1 --no-entry`），导出：
  - `oklch2rgb.wasm`: `oklch2rgb_calc_js`, `oklch2rgb_calc_rel_js`
  - `rgb2oklch.wasm`: `rgb2oklch_calc_js`
  - `extract-colors.wasm`: `get_pixels_buffer`, `extract_colors_from_rgba_js`

若尚未安装 Emscripten，请先安装并配置 emcc 到 PATH。

## WebAssembly 构建与最小示例

- 目录 `wasm/` 下包含可直接在浏览器加载的 `oklch2rgb.wasm`、`rgb2oklch.wasm`、`extract-colors.wasm` 以及演示页面 `minimal.html`（无需 Emscripten JS 胶水）。
- 演示页输入框新增了相对色度 `rel`（0..1，可留空）：
  - 当 `rel` 留空时：调用绝对色度接口，使用输入的 `C`。
  - 当填写 `rel`（数字且在 [0,1]）：调用相对色度接口，忽略输入的 `C`。
- 导出函数（供 JS 直接调用）：

  - `oklch2rgb_calc_js(L, C, h)` → 返回指向 `[R,G,B]` 的内存指针（int32，0..255）
  - `oklch2rgb_calc_rel_js(L, h, rel)` → 相对色度版本，返回同上
  - `rgb2oklch_calc_js(R, G, B)` → 返回指向 `[L,C,h]` 的内存指针（float64）

- 浏览器端取色封装（API 对齐 Namide/extract-colors）：

```js
import extractColors from "./wasm/extract-colors.js";

// 输入可为：URL 字符串、HTMLImageElement、ImageData（或 {data,width,height}）
const colors = await extractColors(imgOrUrlOrImageData, {
  pixels: 64000,
  distance: 0.22,
  saturationDistance: 0.2,
  lightnessDistance: 0.2,
  hueDistance: 1 / 12,
  // colorValidator?: (r,g,b,a) => boolean
});
```

运行本地演示：

1. 在项目根目录起一个静态服务器（例如 Python http.server） python3 -m http.server 8000。
2. 访问 `http://localhost:8000/wasm/minimal.html`。
3. 在 oklch2rgb 区块中填写 L、h，若要使用相对色度则在 rel 输入 0..1，点击“转换”。
4. 在 extract-colors 区块选择一张图片，点击“提取颜色”，可看到色卡与 JSON 输出（结果顶部显示本次耗时）。

备注：当前仓库的 wasm 二进制已包含 `oklch2rgb_calc_rel_js` 新导出并通过本地验证；若自行重新编译 wasm，请使用 Emscripten 以独立 wasm（`-s STANDALONE_WASM=1 --no-entry`）方式生成，源码中已通过 `__attribute__((export_name(...)))` 指定导出名。
