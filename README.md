# RGB ↔ OKLCH 数字转换器

两个用 C 实现的命令行小工具，在 sRGB 与 OKLCH 之间互转，均采用“数字入/数字出”的简单接口。

## 编译

```zsh
cd /Users/bytedance/Desktop/rgb2oklch
gcc rgb2oklch.c -o rgb2oklch
gcc oklch2rgb.c -o oklch2rgb
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

## 说明

- 转换基于 OKLab/OKLCH 参考实现（Björn Ottosson）。
- 仅使用 sRGB 色彩空间与标准传输函数。
- 输出为纯数字便于脚本处理；若需固定小数位或 JSON 格式，可在源码中调整打印逻辑。

- python3 -m http.server 8000
- http://localhost:8000/wasm/minimal.html

## WebAssembly 构建与最小示例

- 目录 `wasm/` 下包含可直接在浏览器加载的 `oklch2rgb.wasm` 与 `rgb2oklch.wasm` 以及演示页面 `minimal.html`（无需 Emscripten JS 胶水）。
- 演示页输入框新增了相对色度 `rel`（0..1，可留空）：
  - 当 `rel` 留空时：调用绝对色度接口，使用输入的 `C`。
  - 当填写 `rel`（数字且在 [0,1]）：调用相对色度接口，忽略输入的 `C`。
- 导出函数（供 JS 直接调用）：
  - `oklch2rgb_calc_js(L, C, h)` → 返回指向 `[R,G,B]` 的内存指针（int32，0..255）
  - `oklch2rgb_calc_rel_js(L, h, rel)` → 相对色度版本，返回同上
  - `rgb2oklch_calc_js(R, G, B)` → 返回指向 `[L,C,h]` 的内存指针（float64）

运行本地演示：

1. 在项目根目录起一个静态服务器（例如 Python http.server）。
2. 访问 `http://localhost:8000/wasm/minimal.html`。
3. 在 oklch2rgb 区块中填写 L、h，若要使用相对色度则在 rel 输入 0..1，点击“转换”。

备注：当前仓库的 wasm 二进制已包含 `oklch2rgb_calc_rel_js` 新导出并通过本地验证；若自行重新编译 wasm，请使用 Emscripten 以独立 wasm（`-s STANDALONE_WASM=1 --no-entry`）方式生成，源码中已通过 `__attribute__((export_name(...)))` 指定导出名。
