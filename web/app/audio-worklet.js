// audio-worklet.js -- plays the engine's mixed audio. The engine's mixer thread (web/audio/al_web.cpp) writes interleaved
// stereo float frames into a ring buffer in the shared wasm memory; this processor (running on the audio rendering thread)
// reads them through the same SharedArrayBuffer. Control block layout (uint32 each): read, write, outRate, consumer, underruns.
class KisakOut extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.i32 = new Int32Array(o.sab);
    this.f32 = new Float32Array(o.sab, o.ringPtr, o.ringFrames * 2);
    this.ctl = o.ctlPtr >> 2;
    this.mask = o.ringFrames - 1;
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1];
    const n = L.length;
    const r = Atomics.load(this.i32, this.ctl) >>> 0;
    const w = Atomics.load(this.i32, this.ctl + 1) >>> 0;
    if (((w - r) >>> 0) < n) {                 // not enough data produced yet: output silence, keep position
      Atomics.add(this.i32, this.ctl + 4, 1);
      return true;
    }
    for (let i = 0; i < n; i++) {
      const idx = ((r + i) & this.mask) * 2;
      L[i] = this.f32[idx];
      if (R) R[i] = this.f32[idx + 1];
    }
    Atomics.store(this.i32, this.ctl, (r + n) >>> 0);
    return true;
  }
}
registerProcessor('kisak-out', KisakOut);
