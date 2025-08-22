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