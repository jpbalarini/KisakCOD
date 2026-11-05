// io-worker.js -- game-data I/O for the web build: persistent range cache (OPFS), parallel fetches and read-ahead.
//
// The engine reads the game data (7 GB of .iwd archives and fastfiles that live on the dev server) with *synchronous* reads from its own worker
// threads (web/shim/kvfs.cpp). Those threads cannot await anything, so they hand each range read to this worker through a small control block in
// the shared wasm memory and block on it (Atomics.wait):
//
//   control block (int32 words):  [0] doorbell (bumped per request)   [1] ready: 0 starting, 1 ok (persistent cache), 3 ok (network only), 2 failed
//                                 [2..6] statistics: network requests, network KB, cache-hit requests, cache-hit KB, read-ahead requests
//                                 [8 + 8*slot ..] request slot: state (0 free, 1 pending, 3 busy, 2 done), path ptr, off lo, off hi, len, dst ptr, result
//
// For each request the worker serves every byte it already has from the cache, fetches only the missing gaps with HTTP range requests (several
// at a time), appends them to the cache and copies the result straight into the engine's destination buffer. The cache is a log file in the
// Origin Private File System: `data.bin` holds the fetched bytes back to back, `index.bin` is an append-only list of
// (file, start, end, position in data.bin) records and `meta.json` maps game-data paths to file numbers. A later page load therefore reads
// what an earlier one downloaded straight from local disk. Entries are keyed by (size, mtime) from /data-manifest.json, so a changed
// game-data file is ignored automatically.
//
// A second tab of the same origin cannot open the same OPFS files (exclusive locks): it runs network-only (still with parallel read-ahead
// disabled, since there is nowhere to keep the bytes).
// Page URL switches: ?args=--nocache (do not use the persistent cache)  ?args=--clearcache (empty it first)
'use strict';

const SLOT0 = 8, SLOT_WORDS = 8, SLOTS = 16;
const CACHE_DIR = 'kisak-data-cache-v1';
const CACHE_LIMIT = 6 * 1024 * 1024 * 1024;       // no new data is cached past this; the next page load starts over with an empty cache
const WIPE_AT = CACHE_LIMIT - 16 * 1024 * 1024;   // (a chunk is at most ~8 MB, so a cache that stopped growing is always above this)
const MAX_PARALLEL = 6;                            // concurrent network requests (browsers allow 6 HTTP/1.1 connections per origin)
const PREFETCH_PAR = 5;                            // of which at most this many run trace prefetches: one is always free for the engine's own reads
const PREFETCH_CHUNK = 1024 * 1024;                // a prefetch request is at most this big (a read of the engine that overlaps it waits for it)
const PREFETCH_MAX = 8 * 1024 * 1024;
const TRACE_WINDOW_MS = 90 * 1000;                 // a phase's reads are recorded for this long (the load itself is over long before)
const TRACE_MAX_BYTES = 1.5e9;
const GAP_MERGE = 128 * 1024;                      // prefetch merges reads of one file that are closer than this into one request
const REC_MERGE = 32 * 1024;

const ioLog = (t) => { console.log(t); try { postMessage({ type: 'log', text: t }); } catch (_) {} };   // also shown in the page's log

let mem = null, base = '', ctl = 0, learnAll = false;
let files = new Map();                             // path -> {path, size, no, ext: [{s,e,o}] sorted, inflight: [{s,e,p}], lastEnd, seq}
let store = null;                                  // {data, index, meta, logEnd, idxEnd, dirty}
let netBusy = 0; const netQ = [[], []];            // [0] engine reads and sequential read-ahead, [1] trace prefetch

const view = () => new Int32Array(mem.buffer);

function stat(word, add) { const I = view(); Atomics.add(I, (ctl >> 2) + word, add); }

// ---------------------------------------------------------------------------------------------------------------------------------------
// persistent store
// ---------------------------------------------------------------------------------------------------------------------------------------
async function openStore(manifest, clear) {
  if (!self.navigator || !navigator.storage || !navigator.storage.getDirectory) return null;
  const root = await navigator.storage.getDirectory();
  const dir = await root.getDirectoryHandle(CACHE_DIR, { create: true });
  const fh = {};
  for (const n of ['data.bin', 'index.bin', 'meta.json']) fh[n] = await dir.getFileHandle(n, { create: true });
  const open = [];
  try {
    for (const n of ['data.bin', 'index.bin', 'meta.json']) open.push(await fh[n].createSyncAccessHandle());
  } catch (e) {                                   // another tab of this origin owns the cache
    for (const h of open) try { h.close(); } catch (_) {}
    ioLog('[io] persistent cache is in use by another tab; running network-only');
    return null;
  }
  const [data, index, meta] = open;
  const dec = new TextDecoder();
  let m = { v: 1, next: 1, files: {} };
  if (!clear) {
    try {
      const sz = meta.getSize();
      if (sz) { const b = new Uint8Array(sz); meta.read(b, { at: 0 }); m = JSON.parse(dec.decode(b)); }
    } catch (e) { m = { v: 1, next: 1, files: {} }; }
  }
  if (clear || m.v !== 1 || data.getSize() > WIPE_AT) {
    if (!clear && data.getSize() > WIPE_AT) ioLog('[io] persistent cache was full (' + (data.getSize() / 1073741824).toFixed(1) + ' GB): starting over');
    data.truncate(0); index.truncate(0);
    m = { v: 1, next: 1, files: {} };
  }
  // file numbers: reuse the stored one when (size, stamp) still match, otherwise allocate a fresh number (old extents become garbage)
  let metaChanged = false;
  for (const [path, info] of manifest) {
    const old = m.files[path];
    if (old && old.size === info.size && old.stamp === info.stamp) { info.no = old.no; continue; }
    info.no = m.next++;
    m.files[path] = { no: info.no, size: info.size, stamp: info.stamp };
    metaChanged = true;
  }
  const byNo = new Map();
  for (const f of files.values()) byNo.set(f.no, f);
  // replay the index
  const logEnd = data.getSize();
  const isz = index.getSize() - (index.getSize() % 32);
  let kept = 0, dropped = 0;
  if (isz) {
    const raw = new Uint8Array(isz); index.read(raw, { at: 0 });
    const rec = new Float64Array(raw.buffer);
    for (let i = 0; i < rec.length; i += 4) {
      const f = byNo.get(rec[i]);
      const s = rec[i + 1], e = rec[i + 2], o = rec[i + 3];
      if (!f || e <= s || o + (e - s) > logEnd || e > f.size) { dropped++; continue; }
      insertExtent(f, s, e, o); kept++;
    }
  }
  if (isz !== index.getSize()) index.truncate(isz);
  const st = { data, index, meta, logEnd, idxEnd: isz, dirty: false };
  if (metaChanged) writeMeta(st, m);
  st.m = m;
  ioLog(`[io] cache: ${(logEnd / 1048576).toFixed(0)} MB in ${kept} extents${dropped ? ` (${dropped} stale records ignored)` : ''}`);
  return st;
}

function writeMeta(st, m) {
  const b = new TextEncoder().encode(JSON.stringify(m));
  st.meta.truncate(0); st.meta.write(b, { at: 0 }); st.meta.flush();
}

function insertExtent(f, s, e, o) {                // keeps f.ext sorted by start; callers guarantee no overlap
  const a = f.ext;
  let lo = 0, hi = a.length;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (a[mid].s < s) lo = mid + 1; else hi = mid; }
  a.splice(lo, 0, { s, e, o });
}

// What part of [s, e) is cached? -> {hits: [{s, e, o}], gaps: [[a, b]]}
function coverage(f, s, e) {
  const a = f.ext, hits = [], gaps = [];
  let lo = 0, hi = a.length;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (a[mid].e <= s) lo = mid + 1; else hi = mid; }
  let pos = s;
  for (let i = lo; i < a.length && a[i].s < e; i++) {
    const x = a[i];
    if (x.s > pos) gaps.push([pos, x.s]);
    const hs = Math.max(x.s, pos), he = Math.min(x.e, e);
    hits.push({ s: hs, e: he, o: x.o + (hs - x.s) });
    pos = he;
  }
  if (pos < e) gaps.push([pos, e]);
  return { hits, gaps };
}

// Append fetched bytes (the file range starting at `start`) to the log, skipping parts that are already cached.
function storeBytes(f, start, bytes) {
  if (!store || store.full) return;
  try {
    const end = start + bytes.length;
    for (const [a, b] of coverage(f, start, end).gaps) {
      if (store.logEnd + (b - a) > CACHE_LIMIT) {
        store.full = true;
        ioLog('[io] persistent cache reached its size limit: nothing new is cached this session, the next page load starts over');
        return;
      }
      const chunk = bytes.subarray(a - start, b - start);
      store.data.write(chunk, { at: store.logEnd });
      const rec = new Float64Array([f.no, a, b, store.logEnd]);
      store.index.write(new Uint8Array(rec.buffer), { at: store.idxEnd });
      insertExtent(f, a, b, store.logEnd);
      store.logEnd += chunk.length; store.idxEnd += 32; store.dirty = true;
    }
  } catch (e) {                                    // quota exceeded...: stop adding data, but keep serving what is already cached
    console.warn('[io] cache write failed, no new data will be cached this session:', e);
    store.full = true;
  }
}

setInterval(() => {
  if (store && store.dirty) { try { store.data.flush(); store.index.flush(); store.dirty = false; } catch (_) {} }
}, 3000);

// ---------------------------------------------------------------------------------------------------------------------------------------
// network
// ---------------------------------------------------------------------------------------------------------------------------------------
// Run fn() with at most MAX_PARALLEL in flight. prio 0 (the engine is waiting) always goes before prio 1 (prefetch), and prefetch never takes
// the last connection, so an engine read is never stuck behind a queue of prefetches.
function netPump() {
  for (;;) {
    let job = null;
    if (netBusy < MAX_PARALLEL && netQ[0].length) job = netQ[0].shift();
    else if (netBusy < MAX_PARALLEL - 1 && netQ[1].length) job = netQ[1].shift();
    if (!job) return;
    netBusy++;
    job.fn().then(job.res, job.rej).finally(() => { netBusy--; netPump(); });
  }
}
function net(fn, prio = 0) {
  return new Promise((res, rej) => { netQ[prio].push({ fn, res, rej }); netPump(); });
}

const encodePath = (p) => p.split('/').map(encodeURIComponent).join('/');

async function fetchBytes(f, s, e, prio = 0) {
  return net(async () => {
    // cache: 'no-store' skips Chrome's HTTP cache, which would otherwise serialise parallel range requests to the same URL (it takes a
    // per-URL lock: 5 parallel requests ran at less than half the speed of the same 5 without it) and never keeps 206 responses anyway
    const r = await fetch(`${base}/data/${encodePath(f.path)}`, { headers: { Range: `bytes=${s}-${e - 1}` }, cache: 'no-store' });
    if (r.status !== 206 && r.status !== 200) throw new Error(`HTTP ${r.status} for ${f.path}`);
    let b = new Uint8Array(await r.arrayBuffer());
    if (r.status === 200) b = b.subarray(s, e);   // server ignored the Range header
    stat(2, 1); stat(3, Math.round(b.length / 1024));
    return b;
  }, prio);
}

// Fetch [a, b) of f over the network and append it to the cache; registered as in-flight so overlapping reads wait for it instead of
// fetching it again.
function fetchGap(f, a, b, prio) {
  const entry = { s: a, e: b, p: null };
  entry.p = fetchBytes(f, a, b, prio).then((bytes) => storeBytes(f, a, bytes));
  entry.p.catch(() => {}).finally(() => { const i = f.inflight.indexOf(entry); if (i >= 0) f.inflight.splice(i, 1); });
  f.inflight.push(entry);
  return entry.p;
}

// Make [s, e) of f available and return it as one Uint8Array.
async function getRange(f, s, e) {
  for (let tries = 0; tries < 4; tries++) {
    const waits = f.inflight.filter((x) => x.e > s && x.s < e).map((x) => x.p.catch(() => {}));
    if (!waits.length) break;
    await Promise.all(waits);                       // someone (read-ahead or another thread) is already fetching part of it
  }
  const out = new Uint8Array(e - s);
  const { hits, gaps } = coverage(f, s, e);
  const jobs = gaps.map(([a, b]) => {
    const entry = { s: a, e: b, p: null };
    entry.p = fetchBytes(f, a, b).then((bytes) => { storeBytes(f, a, bytes); out.set(bytes.subarray(0, b - a), a - s); });
    entry.p.catch(() => {}).finally(() => { const i = f.inflight.indexOf(entry); if (i >= 0) f.inflight.splice(i, 1); });
    f.inflight.push(entry);
    return entry.p;
  });
  let hitBytes = 0;
  if (hits.length && store) {
    for (const h of hits) {
      const dst = out.subarray(h.s - s, h.e - s);
      store.data.read(dst, { at: h.o });
      hitBytes += dst.length;
    }
  }
  await Promise.all(jobs);
  if (!gaps.length) { stat(4, 1); }
  if (hitBytes) stat(5, Math.round(hitBytes / 1024));
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// load traces: what a map (or the start-up, or the menu) reads is the same every time, so the server remembers it and a cold client fetches it
// all in parallel *ahead* of the engine, whose own reads are synchronous one-at-a-time round trips (most of a cold load is waiting for those).
//
// The engine's data reads are split into "phases": `startup` (until the first fastfile is read) and then one phase per zone fastfile
// (`zone/<lang>/<name>.ff`, read from offset 0), e.g. `localized_common_mp`, `common_mp`, `ui_mp`, `mp_crash`. When a phase starts the worker asks the
// server for /traces/<name>.json. Found: prefetch it. Not found: record this phase's reads (the first TRACE_WINDOW_MS of them) and upload them,
// so the next client to load it benefits. The first client to load a map is therefore as slow as before; everybody after it is not.
// ---------------------------------------------------------------------------------------------------------------------------------------
let phase = null;                                  // {name, learn, answered, rec: [{f, s, e}] | null, kept, timer}
let pf = null;                                     // running prefetch: {name, queue, idx, total, done}

function startPhase(name) {
  closePhase();
  const ph = { name, learn: false, answered: false, rec: [], kept: null, timer: 0 };
  phase = ph;
  ph.timer = setTimeout(() => { if (phase === ph) closePhase(); }, TRACE_WINDOW_MS);
  loadTrace(ph);
}

function closePhase() {                            // stop recording; upload what was recorded if the server did not know this phase
  const ph = phase;
  if (!ph || !ph.rec) return;
  clearTimeout(ph.timer);
  const rec = ph.rec; ph.rec = null;
  if (!ph.answered) ph.kept = rec;                 // the server has not said yet whether it knows this phase: loadTrace decides
  else if (ph.learn) uploadTrace(ph, rec);
}

function record(f, off, n) {
  const ph = phase;
  if (!ph || !ph.rec) return;
  const a = ph.rec, end = off + n;
  for (let i = a.length - 1, k = 0; i >= 0 && k < 24; i--, k++) {              // extend a recent read of the same file instead of listing both
    const x = a[i];
    if (x.f === f && off <= x.e + REC_MERGE && end >= x.s - REC_MERGE) { x.s = Math.min(x.s, off); x.e = Math.max(x.e, end); return; }
  }
  a.push({ f, s: off, e: end });
}

async function uploadTrace(ph, rec) {
  rec = rec || [];
  const paths = [], idx = new Map(), ext = [];
  let bytes = 0;
  for (const x of rec) {
    let i = idx.get(x.f.path);
    if (i === undefined) { i = paths.length; idx.set(x.f.path, i); paths.push(x.f.path); }
    ext.push([i, x.s, x.e]); bytes += x.e - x.s;
  }
  if (!ext.length || bytes > TRACE_MAX_BYTES) return;
  try {
    const r = await fetch(`${base}/traces/${encodeURIComponent(ph.name)}.json${learnAll ? '?replace=1' : ''}`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ v: 1, files: paths, ext })
    });
    ioLog(`[io] load trace '${ph.name}': ${ext.length} reads, ${(bytes / 1048576).toFixed(0)} MB, uploaded (HTTP ${r.status})`);
  } catch (e) { console.warn('[io] could not upload the load trace:', e); }
}

async function loadTrace(ph) {
  let tr = null;
  try {
    const r = await fetch(`${base}/traces/${encodeURIComponent(ph.name)}.json`, { cache: 'no-store' });
    if (r.status === 200) tr = await r.json();
    else if (r.status === 404) ph.learn = true;
  } catch (_) {}
  if (learnAll) ph.learn = true;
  ph.answered = true;
  if (ph.kept) { if (ph.learn) uploadTrace(ph, ph.kept); ph.kept = null; }     // the phase ended before the server answered
  if (tr && phase === ph && store && !store.full) startPrefetch(ph, tr);
}

// The trace (reads in the order the engine made them) -> requests: per file, merge reads that are close together; then order the merged
// requests by the time of their first read, so the data the engine needs soonest arrives first; cut them into PREFETCH_CHUNK pieces.
function planFrom(tr) {
  const per = new Map();
  tr.ext.forEach(([fi, s, e], t) => {
    const f = files.get(tr.files[fi]);
    if (!f || !(e > s) || e > f.size) return;
    let a = per.get(f); if (!a) per.set(f, a = []);
    a.push({ s, e, t });
  });
  const merged = [];
  for (const [f, a] of per) {
    a.sort((x, y) => x.s - y.s);
    let cur = null;
    for (const x of a) {
      if (cur && x.s <= cur.e + GAP_MERGE) { cur.e = Math.max(cur.e, x.e); cur.t = Math.min(cur.t, x.t); }
      else { if (cur) merged.push(cur); cur = { f, s: x.s, e: x.e, t: x.t }; }
    }
    if (cur) merged.push(cur);
  }
  merged.sort((x, y) => x.t - y.t);
  const chunks = [];
  for (const x of merged) for (let s = x.s; s < x.e; s += PREFETCH_CHUNK) chunks.push({ f: x.f, s, e: Math.min(x.e, s + PREFETCH_CHUNK) });
  return chunks;
}

function startPrefetch(ph, tr) {
  const queue = planFrom(tr);
  let total = 0;
  for (const c of queue) for (const [a, b] of coverage(c.f, c.s, c.e).gaps) total += b - a;
  if (!total) { ioLog(`[io] load trace '${ph.name}': everything is already cached`); return; }
  const mine = { name: ph.name, queue, idx: 0, total, done: 0, t0: performance.now() };
  pf = mine;
  ioLog(`[io] load trace '${ph.name}': prefetching ${(total / 1048576).toFixed(0)} MB`);
  let alive = PREFETCH_PAR;
  const finish = () => {
    if (--alive) return;
    const s = (performance.now() - mine.t0) / 1000;
    ioLog(`[io] load trace '${mine.name}': prefetched ${(mine.done / 1048576).toFixed(0)} MB in ${s.toFixed(1)} s (${(mine.done / 1048576 / Math.max(s, 0.1)).toFixed(1)} MB/s)`);
    if (pf === mine) pf = null;
  };
  for (let i = 0; i < PREFETCH_PAR; i++) prefetchLoop(mine).finally(finish);
}

async function prefetchLoop(p) {
  while (pf === p && p.idx < p.queue.length) {
    const c = p.queue[p.idx++];
    try {
      const { f, s, e } = c;
      const waits = f.inflight.filter((x) => x.e > s && x.s < e).map((x) => x.p.catch(() => {}));
      if (waits.length) await Promise.all(waits);                              // somebody is already fetching part of it
      if (!store || store.full) return;
      // (no await between computing the gaps and registering their fetches, so nobody else can start the same one)
      const jobs = coverage(f, s, e).gaps.map(([a, b]) => { p.done += b - a; return fetchGap(f, a, b, 1); });
      await Promise.all(jobs);
    } catch (e) { /* a failed chunk is simply fetched by the engine's own read later */ }
  }
}

function readCString(ptr) {
  const u8 = new Uint8Array(mem.buffer, ptr, 1024).slice();       // copy out of the shared buffer (TextDecoder rejects shared views)
  let n = 0; while (n < u8.length && u8[n]) n++;
  return new TextDecoder().decode(u8.subarray(0, n));
}

// Sequential streams (fastfile loading): keep the next stretch coming while the engine decompresses what it just got.
function noteAccess(f, off, n) {
  const sequential = f.lastEnd >= 0 && off >= f.lastEnd - 4096 && off <= f.lastEnd + 65536;
  f.seq = sequential ? Math.min(f.seq + 1, 8) : 0;
  f.lastEnd = off + n;
  if (!store || f.seq < 2) return;
  const a = off + n, b = Math.min(f.size, a + Math.min(PREFETCH_MAX, Math.max(n * 2, 1 << 20)));
  if (b - a < 65536) return;
  if (f.inflight.some((x) => x.e > a)) return;                    // already running ahead
  const { gaps } = coverage(f, a, b);
  if (!gaps.length) return;
  stat(6, 1);
  getRange(f, gaps[0][0], Math.min(b, gaps[0][1])).catch(() => {});
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// profile persistence. The engine writes its player profiles (config, stats, unlocks, custom classes, the list of profiles and the active one)
// as files under <install root>/players/, which kvfs.cpp keeps in memory. kvfs mirrors every change to them here with request types 3-5, and asks for
// everything saved with request types 1-2 at start-up, so a profile survives a page reload. Stored in IndexedDB (not the OPFS cache: those handles are
// exclusive to one tab, IndexedDB is not): database 'kisak-profile', store 'files', records {path, dir, data, mtime} with paths relative to the install
// root: `path` is the lower-case key ('players/profiles/alice/config_mp.cfg'), `orig` the same path with the capitals the engine used. app/shell.html
// reads and writes the same store for Export / Import.
//   request word 7 (op): 0 = range read (above)   1 = size of the saved profile blob   2 = fetch it   3 = save a file   4 = delete   5 = make a directory
// The blob: u32 count, then per record: u8 isDir, u32 pathLen, path (utf-8), u32 dataLen, data (all little-endian).
// ---------------------------------------------------------------------------------------------------------------------------------------
const PROF_DB = 'kisak-profile', PROF_STORE = 'files';
let profDb = null, profChain = Promise.resolve(), profBlob = null, nopersist = false;

function profOpen() {
  if (!profDb) profDb = new Promise((res, rej) => {
    const r = indexedDB.open(PROF_DB, 1);
    r.onupgradeneeded = () => r.result.createObjectStore(PROF_STORE, { keyPath: 'path' });
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
  return profDb;
}
function profTx(mode, fn) {
  return profOpen().then((db) => new Promise((res, rej) => {
    const tx = db.transaction(PROF_STORE, mode);
    const rq = fn(tx.objectStore(PROF_STORE));
    tx.oncomplete = () => res(rq && rq.result);
    tx.onerror = tx.onabort = () => rej(tx.error);
  }));
}
function profQueue(fn) {                           // writes happen in the order the engine made them
  profChain = profChain.then(fn).catch((e) => console.warn('[io] saving the profile failed:', e));
}

async function profBuildBlob() {
  const recs = await profTx('readonly', (s) => s.getAll());
  const enc = new TextEncoder();
  const items = recs.map((r) => ({ dir: !!r.dir, path: enc.encode(r.orig || r.path), data: r.dir ? new Uint8Array(0) : new Uint8Array(r.data) }));
  let total = 4;
  for (const it of items) total += 1 + 4 + it.path.length + 4 + it.data.length;
  const out = new Uint8Array(total), dv = new DataView(out.buffer);
  let p = 0;
  dv.setUint32(p, items.length, true); p += 4;
  for (const it of items) {
    out[p++] = it.dir ? 1 : 0;
    dv.setUint32(p, it.path.length, true); p += 4; out.set(it.path, p); p += it.path.length;
    dv.setUint32(p, it.data.length, true); p += 4; out.set(it.data, p); p += it.data.length;
  }
  return out;
}

async function serveProfile(slot, op) {
  const b = (ctl >> 2) + SLOT0 + slot * SLOT_WORDS;
  let I = view();
  const ptr = I[b + 1], len = I[b + 4], dst = I[b + 5] >>> 0;
  let result = 0;
  try {
    if (nopersist) result = op >= 3 ? 1 : 0;       // switched off (?args=--nopersist): requests are accepted and ignored (1 = "not stored")
    else {
      if (op === 1) { profBlob = await profBuildBlob(); result = profBlob.length > 4 ? profBlob.length : 0; if (!result) profBlob = null; }
      else if (op === 2) { if (profBlob) { new Uint8Array(mem.buffer, dst, profBlob.length).set(profBlob); result = profBlob.length; profBlob = null; } }
      else {
        const orig = readCString(ptr), path = orig.toLowerCase();               // key: lower case; `orig` keeps the capitals ('players/profiles/Alice/...')
        if (op === 3) {
          const data = new Uint8Array(mem.buffer, dst, len).slice().buffer;     // copy now: the engine's buffer is only valid until this request completes
          profQueue(() => profTx('readwrite', (s) => s.put({ path, orig, dir: false, data, mtime: Date.now() })));
        } else if (op === 4) profQueue(() => profTx('readwrite', (s) => s.delete(path)));
        else if (op === 5) profQueue(() => profTx('readwrite', (s) => s.put({ path, orig, dir: true, mtime: Date.now() })));
      }
    }
  } catch (e) {
    console.warn('[io] profile request failed:', e);
    result = -2;
  }
  I = view();
  Atomics.store(I, b + 6, result);
  Atomics.store(I, b, 2);
  Atomics.notify(I, b);
}

async function serve(slot) {
  const b = (ctl >> 2) + SLOT0 + slot * SLOT_WORDS;
  let I = view();
  const op = I[b + 7];
  if (op) return serveProfile(slot, op);
  const ptr = I[b + 1], off = (I[b + 2] >>> 0) + (I[b + 3] >>> 0) * 4294967296, len = I[b + 4], dst = I[b + 5] >>> 0;
  let result = -1;
  try {
    const f = files.get(readCString(ptr));
    if (f) {
      if (off >= f.size) result = 0;
      else {
        const n = Math.min(len, f.size - off);
        if (off < 4096) { const m = /^zone\/[^/]+\/([^/]+)\.ff$/.exec(f.path); if (m && (!phase || phase.name !== m[1])) startPhase(m[1]); }
        const bytes = await getRange(f, off, off + n);
        new Uint8Array(mem.buffer, dst, n).set(bytes);
        result = n;
        noteAccess(f, off, n);
        record(f, off, n);
      }
    }
  } catch (e) {
    console.warn('[io] read failed:', e);
    result = -2;
  }
  I = view();
  Atomics.store(I, b + 6, result);
  Atomics.store(I, b, 2);
  Atomics.notify(I, b);
}

async function loop() {
  let seen = Atomics.load(view(), ctl >> 2);
  for (;;) {
    const I = view();
    for (let s = 0; s < SLOTS; s++) {
      const w = (ctl >> 2) + SLOT0 + s * SLOT_WORDS;
      if (Atomics.compareExchange(I, w, 1, 3) === 1) serve(s);
    }
    const now = Atomics.load(I, ctl >> 2);
    if (now !== seen) { seen = now; continue; }                    // more work arrived while scanning
    if (Atomics.waitAsync) await Atomics.waitAsync(I, ctl >> 2, seen).value;
    else await new Promise((r) => setTimeout(r, 2));
  }
}

onmessage = async (ev) => {
  const m = ev.data;
  mem = m.memory || { get buffer() { return m.buffer; } };
  ctl = m.ctl;
  base = m.base;
  learnAll = !!m.learn;
  nopersist = !!m.nopersist;
  const setReady = (v) => { const I = view(); Atomics.store(I, (ctl >> 2) + 1, v); Atomics.notify(I, (ctl >> 2) + 1); };
  try {
    const man = await (await fetch(`${base}/data-manifest.json`)).json();
    const manifest = new Map();
    for (const [path, size] of Object.entries(man.files)) {
      const info = { path, size, stamp: String((man.stamps || {})[path] || 0), no: 0, ext: [], inflight: [], lastEnd: -1, seq: 0 };
      manifest.set(path, info);
      files.set(path, info);
    }
    if (!m.nocache) {
      try { store = await openStore(manifest, m.clear); } catch (e) { console.warn('[io] persistent cache unavailable:', e); store = null; }
    }
    setReady(store ? 1 : 3);
    startPhase('startup');
    loop();
  } catch (e) {
    console.warn('[io] init failed:', e);
    setReady(2);
  }
};
