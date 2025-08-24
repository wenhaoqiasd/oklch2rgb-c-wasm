// 颜色转换合并模块（浏览器端 ESM）
// - 统一初始化：一次并行加载 oklch2rgb.wasm 与 rgb2oklch.wasm
// - 导出 API：
//   init({ oklch2rgbUrl?, rgb2oklchUrl? })  —— 幂等初始化，重复调用不会重复加载
//   oklch2rgb_abs(L, C, h)                 —— 绝对色度：OKLCH -> sRGB(0..255)
//   oklch2rgb_rel(L, h, rel)               —— 相对色度：OKLCH(L,h,相对色度0..1) -> sRGB(0..255)
//   rgb2oklch(r, g, b)                     —— sRGB(0..255) -> OKLCH

// ---- 最小化 WASM 实例化辅助（内联自 wasm-util，按需精简） ----
function createWasiStub(memory) {
  function ret0() { return 0; }
  function fd_write(_fd, _iov, _iovcnt, pOut) {
    try { new DataView(memory.buffer).setUint32(pOut >>> 0, 0, true); } catch { }
    return 0;
  }
  function random_get(ptr, len) {
    try {
      const dst = new Uint8Array(memory.buffer, ptr >>> 0, len >>> 0);
      crypto.getRandomValues(dst);
    } catch { }
    return 0;
  }
  function proc_exit(code) { throw new Error("WASI proc_exit: " + code); }
  return {
    args_get: ret0,
    args_sizes_get: (pArgc, pArgvBufSize) => {
      try {
        const view = new DataView(memory.buffer);
        view.setUint32(pArgc >>> 0, 0, true);
        view.setUint32(pArgvBufSize >>> 0, 0, true);
      } catch { }
      return 0;
    },
    environ_get: ret0,
    environ_sizes_get: (pCount, pBufSize) => {
      try {
        const view = new DataView(memory.buffer);
        view.setUint32(pCount >>> 0, 0, true);
        view.setUint32(pBufSize >>> 0, 0, true);
      } catch { }
      return 0;
    },
    fd_write,
    random_get,
    proc_exit,
  };
}

/**
 * 尝试以最优方式实例化 WASM：
 * 1) 优先使用 instantiateStreaming（服务器返回 application/wasm）
 * 2) 回退为 ArrayBuffer 实例化
 * 3) 如果缺少导入（wasi/env），再使用最小化的导入桩并提供独立内存
 */
async function instantiateWasmWithFallback(url) {
  // 首选路径：单次请求，能流式就流式，否则走 arrayBuffer
  try {
    const resp = await fetch(url);
    if (!resp.ok) throw new Error(`Failed to fetch ${url}: ${resp.status}`);

    const ct = resp.headers.get("content-type") || "";
    if (WebAssembly.instantiateStreaming && ct.includes("application/wasm")) {
      const { instance } = await WebAssembly.instantiateStreaming(resp, {});
      return instance;
    }
    const { instance } = await WebAssembly.instantiate(await resp.arrayBuffer(), {});
    return instance;
  } catch {
    // 回退路径：提供最小 WASI/env 导入和独立内存（避免导入缺失带来的实例化失败）
    const resp2 = await fetch(url);
    if (!resp2.ok) throw new Error(`Failed to fetch ${url}: ${resp2.status}`);
    const buf = await resp2.arrayBuffer();

    const memory = new WebAssembly.Memory({ initial: 256, maximum: 16384 });
    const imports = {
      wasi_snapshot_preview1: createWasiStub(memory),
      env: { memory, abort() { }, emscripten_notify_memory_growth() { } },
    };
    const { instance } = await WebAssembly.instantiate(buf, imports);
    return instance;
  }
}

// ---- 两个 WASM 模块的共享状态 ----
let okExports = null; // oklch2rgb wasm exports
let okMem = null;     // oklch2rgb wasm memory
let rgbExports = null;// rgb2oklch wasm exports
let rgbMem = null;    // rgb2oklch wasm memory
let _ready = false;   // 初始化是否完成
let _initPromise = null; // 初始化中的 Promise，避免重复开销

/**
 * 初始化（幂等）：并行加载两个 WASM，并保存导出与内存引用。
 * 可选参数允许自定义 wasm 路径；相对路径相对于本模块文件。
 */
export async function init(options = {}) {
  if (_ready) return;                // 已完成，直接返回
  if (_initPromise) return _initPromise; // 进行中，复用同一个 Promise
  const {
    oklch2rgbUrl = 'oklch2rgb.wasm',
    rgb2oklchUrl = 'rgb2oklch.wasm',
  } = options;

  // 将相对路径解析为相对于当前模块的绝对 URL，避免页面目录差异导致的加载失败
  const okUrl = new URL(oklch2rgbUrl, import.meta.url).href;
  const rgbUrl = new URL(rgb2oklchUrl, import.meta.url).href;

  _initPromise = (async () => {
    const [okInst, rgbInst] = await Promise.all([
      instantiateWasmWithFallback(okUrl),
      instantiateWasmWithFallback(rgbUrl),
    ]);

    okExports = okInst.exports;
    okMem = okExports.memory;
    rgbExports = rgbInst.exports;
    rgbMem = rgbExports.memory;
    _ready = true;
  })();

  return _initPromise;
}

function ensureOk() {
  if (!okExports || !okMem) throw new Error('oklch2rgb 模块尚未初始化，请先调用 init()');
}
function ensureRgb() {
  if (!rgbExports || !rgbMem) throw new Error('rgb2oklch 模块尚未初始化，请先调用 init()');
}

// ---- 转换函数 ----
/**
 * OKLCH 绝对色度 -> sRGB 整数分量
 * 入参：L, C, h
 * 返回：{ R, G, B }，范围 0..255
 */
export function oklch2rgb_abs(L, C, h) {
  ensureOk();
  const ptr = okExports.oklch2rgb_calc_js(L, C, h) >>> 0;
  const i32 = new Int32Array(okMem.buffer, ptr, 3);
  return { R: i32[0] | 0, G: i32[1] | 0, B: i32[2] | 0 };
}

/**
 * OKLCH 相对色度 -> sRGB 整数分量
 * 入参：L, h, rel（相对色度 0..1）
 * 返回：{ R, G, B }，范围 0..255
 */
export function oklch2rgb_rel(L, h, rel) {
  ensureOk();
  const r = Math.max(0, Math.min(1, Number(rel)));
  const ptr = okExports.oklch2rgb_calc_rel_js(L, h, r) >>> 0;
  const i32 = new Int32Array(okMem.buffer, ptr, 3);
  return { R: i32[0] | 0, G: i32[1] | 0, B: i32[2] | 0 };
}

/**
 * sRGB 整数分量 -> OKLCH 浮点分量
 * 入参：r, g, b（0..255）
 * 返回：{ L, C, h }
 */
export function rgb2oklch(r, g, b) {
  ensureRgb();
  const ptr = rgbExports.rgb2oklch_calc_js(r | 0, g | 0, b | 0) >>> 0;
  const f64 = new Float64Array(rgbMem.buffer, ptr, 3);
  return { L: f64[0], C: f64[1], h: f64[2] };
}
