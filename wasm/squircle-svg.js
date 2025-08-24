// squircle/capsule SVG path generator (browser ESM)
// Exports:
//   getPath(shape, width, height, radius) => Promise<string>
//   getSquircle(width, height, radius) => Promise<string>
//   getCapsule(width, height, radius) => Promise<string>

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
    args_sizes_get: (pArgc, pArgvBufSize) => { try { const v = new DataView(memory.buffer); v.setUint32(pArgc >>> 0, 0, true); v.setUint32(pArgvBufSize >>> 0, 0, true); } catch { } return 0; },
    environ_get: ret0,
    environ_sizes_get: (pCount, pBufSize) => { try { const v = new DataView(memory.buffer); v.setUint32(pCount >>> 0, 0, true); v.setUint32(pBufSize >>> 0, 0, true); } catch { } return 0; },
    fd_write,
    random_get,
    proc_exit,
  };
}

async function instantiateWasmWithFallback(url) {
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
    const resp2 = await fetch(url);
    if (!resp2.ok) throw new Error(`Failed to fetch ${url}: ${resp2.status}`);
    const buf = await resp2.arrayBuffer();
    const memory = new WebAssembly.Memory({ initial: 64, maximum: 16384 });
    const imports = {
      wasi_snapshot_preview1: createWasiStub(memory),
      env: { memory, abort() { }, emscripten_notify_memory_growth() { } },
    };
    const { instance } = await WebAssembly.instantiate(buf, imports);
    return instance;
  }
}

let _inst = null; let _mem = null; let _ready = false; let _initPromise = null;

async function ensureReady(options = {}) {
  if (_ready) return;
  if (_initPromise) return _initPromise;
  const { wasmUrl = 'squircle-svg.wasm' } = options;
  const url = new URL(wasmUrl, import.meta.url).href;
  _initPromise = (async () => {
    const inst = await instantiateWasmWithFallback(url);
    _inst = inst.exports;
    _mem = _inst.memory; // emcc 默认导出 memory
    _ready = true;
  })();
  return _initPromise;
}

function readCString(ptr) {
  ptr = ptr >>> 0;
  const u8 = new Uint8Array(_mem.buffer);
  let end = ptr;
  while (end < u8.length && u8[end] !== 0) end++;
  return new TextDecoder().decode(u8.subarray(ptr, end));
}

export async function getSquircle(width, height, radius, options) {
  await ensureReady(options);
  const p = _inst.squircle_path_js(+width, +height, +radius) >>> 0;
  if (!p) throw new Error('squircle_path_js returned 0');
  return readCString(p);
}

export async function getCapsule(width, height, radius, options) {
  await ensureReady(options);
  const p = _inst.capsule_path_js(+width, +height, +radius) >>> 0;
  if (!p) throw new Error('capsule_path_js returned 0');
  return readCString(p);
}

export async function getPath(shape, width, height, radius, options) {
  const s = String(shape).toLowerCase();
  if (s === 'squircle') return getSquircle(width, height, radius, options);
  if (s === 'capsule') return getCapsule(width, height, radius, options);
  throw new Error('Unknown shape: ' + shape);
}
