#!/usr/bin/env node
/*
Compare C capsule path against the original TS logic (ported to JS here)
*/
const { spawnSync } = require('node:child_process');

function mr(i) {
  return Math.round(Number(i) * 1000) / 1000;
}

function getCapsulePath(width, height, radius) {
  const r = radius;
  const r160 = r * 1.6;
  const r103 = r * 1.03995;
  const r075 = r * 0.759921;
  const r010 = r * 0.108993;
  const r054 = r * 0.546009;
  const r020 = r * 0.204867;
  const r035 = r * 0.357847;
  return `M ${mr(width - r160)} 0 H ${mr(r160)} C ${mr(r103)} 0 ${mr(r075)} 0 ${mr(r054)} ${mr(r010)} C ${mr(r035)} ${mr(r020)} ${mr(r020)} ${mr(r035)} ${mr(r010)} ${mr(r054)} C 0 ${mr(r075)} 0 ${mr(radius * 0.96)} 0 ${mr(radius)} C 0 ${mr(height - radius * 0.96)} 0 ${mr(height - r075)} ${mr(r010)} ${mr(height - r054)} C ${mr(r020)} ${mr(height - r035)} ${mr(r035)} ${mr(height - r020)} ${mr(r054)} ${mr(height - r010)} C ${mr(r075)} ${mr(height)} ${mr(r103)} ${mr(height)} ${mr(r160)} ${mr(height)} H ${mr(width - r160)} H ${mr(width - r160)} C ${mr(width - r103)} ${mr(height)} ${mr(width - r075)} ${mr(height)} ${mr(width - r054)} ${mr(height - r010)} C ${mr(width - r035)} ${mr(height - r020)} ${mr(width - r020)} ${mr(height - r035)} ${mr(width - r010)} ${mr(height - r054)} C ${mr(width)} ${mr(height - r075)} ${mr(width)} ${mr(height - radius * 0.96)} ${mr(width)} ${mr(radius)} C ${mr(width)} ${mr(radius * 0.96)} ${mr(width)} ${mr(r075)} ${mr(width - r010)} ${mr(r054)} C ${mr(width - r020)} ${mr(r035)} ${mr(width - r035)} ${mr(r020)} ${mr(width - r054)} ${mr(r010)} C ${mr(width - r075)} 0 ${mr(width - r103)} 0 ${mr(width - r160)} 0 Z`;
}

function runC(width, height, radius) {
  const r = spawnSync('./squircle_svg', ['capsule', String(width), String(height), String(radius)], {
    encoding: 'utf8'
  });
  if (r.error) throw r.error;
  if (r.status !== 0) throw new Error(`C process exited ${r.status}: ${r.stderr}`);
  return r.stdout.trim();
}

function eq(a, b) {
  return a === b;
}

function testOne(w, h, r) {
  const js = getCapsulePath(w, h, r);
  const c = runC(w, h, r);
  if (!eq(js, c)) {
    console.error('Mismatch for', { w, h, r });
    console.error('JS:', js);
    console.error(' C:', c);
    process.exit(1);
  }
}

// A few samples
const samples = [
  [200, 120, 16],
  [200, 120, 0],
  [333.333, 211.111, 17.777],
  [512, 256, 24.5],
  [199.999, 119.999, 15.999],
  [801.234, 601.987, 32.123],
];

for (const [w, h, r] of samples) testOne(w, h, r);
console.log('[OK] C capsule path matches TS logic for', samples.length, 'cases.');
