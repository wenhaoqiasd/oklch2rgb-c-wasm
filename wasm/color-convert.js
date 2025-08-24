// Combined color conversion module: shared init + both wasm modules
// - Loads oklch2rgb.wasm and rgb2oklch.wasm together
// - Exposes: init({ oklch2rgbUrl?, rgb2oklchUrl? }), oklch2rgb_abs, oklch2rgb_rel, rgb2oklch

// ---- minimal WASM helpers (inlined from wasm-util.js, with tiny refinements) ----
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

async function instantiateWasmWithFallback(url) {
  // Try normal instantiation first (prefer streaming when content-type is wasm)
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
    // Fallback: provide minimal WASI/env imports with our own Memory
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

// ---- shared state for both wasm modules ----
let okExports = null; // oklch2rgb wasm exports
let okMem = null;     // oklch2rgb wasm memory
let rgbExports = null;// rgb2oklch wasm exports
let rgbMem = null;    // rgb2oklch wasm memory

export async function init(options = {}) {
  const {
    oklch2rgbUrl = 'oklch2rgb.wasm',
    rgb2oklchUrl = 'rgb2oklch.wasm',
  } = options;

  const [okInst, rgbInst] = await Promise.all([
    instantiateWasmWithFallback(oklch2rgbUrl),
    instantiateWasmWithFallback(rgb2oklchUrl),
  ]);

  okExports = okInst.exports;
  okMem = okExports.memory;
  rgbExports = rgbInst.exports;
  rgbMem = rgbExports.memory;
}

function ensureOk() {
  if (!okExports || !okMem) throw new Error('oklch2rgb wasm not initialized');
}
function ensureRgb() {
  if (!rgbExports || !rgbMem) throw new Error('rgb2oklch wasm not initialized');
}

// ---- converters ----
export function oklch2rgb_abs(L, C, h) {
  ensureOk();
  const ptr = okExports.oklch2rgb_calc_js(L, C, h) >>> 0;
  const i32 = new Int32Array(okMem.buffer, ptr, 3);
  return { R: i32[0] | 0, G: i32[1] | 0, B: i32[2] | 0 };
}

export function oklch2rgb_rel(L, h, rel) {
  ensureOk();
  const r = Math.max(0, Math.min(1, Number(rel)));
  const ptr = okExports.oklch2rgb_calc_rel_js(L, h, r) >>> 0;
  const i32 = new Int32Array(okMem.buffer, ptr, 3);
  return { R: i32[0] | 0, G: i32[1] | 0, B: i32[2] | 0 };
}

export function rgb2oklch(r, g, b) {
  ensureRgb();
  const ptr = rgbExports.rgb2oklch_calc_js(r | 0, g | 0, b | 0) >>> 0;
  const f64 = new Float64Array(rgbMem.buffer, ptr, 3);
  return { L: f64[0], C: f64[1], h: f64[2] };
}
