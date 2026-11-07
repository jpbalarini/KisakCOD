// kvfs.cpp -- see kvfs.h
#include "kvfs.h"

#include <algorithm>
#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <emscripten.h>

#undef stat

namespace kvfs {

static bool g_browser = false;
static bool g_inited = false;
// Files the engine writes live in memory (g_mem) instead of on disk. Always true in the browser; under node it is opt-in (KISAK_MEMWRITE=1,
// set by the dedicated server) so that the game data directory is only ever read: reads then come from the real files unless a file of the
// same name was written in memory.
static bool g_memFs = false;
static std::string g_root;            // install root, no trailing slash, lower-cased
static std::string g_rootExact;

static bool traceFs() { static int v = -1; if (v < 0) v = getenv("KISAK_TRACE_FS") ? 1 : 0; return v == 1; }
static std::string lower(const std::string &s) { std::string r = s; for (char &c : r) c = (char)tolower((unsigned char)c); return r; }

bool isBrowser() { return g_browser; }

// ====================================================================== POSIX backend
static bool posixResolve(const std::string &path, std::string &out)
{
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) { out = path; return true; }
    std::string cur = (!path.empty() && path[0] == '/') ? "/" : "";
    size_t i = cur.size();
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string comp = path.substr(i, j - i);
        if (!comp.empty() && comp != "." && comp != "..") {
            DIR *d = opendir(cur.empty() ? "." : cur.c_str());
            if (!d) return false;
            std::string found;
            struct dirent *e;
            while ((e = readdir(d))) if (strcasecmp(e->d_name, comp.c_str()) == 0) { found = e->d_name; break; }
            closedir(d);
            if (found.empty()) return false;
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += found;
        } else if (!comp.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += comp;
        }
        i = j + 1;
    }
    out = cur;
    return true;
}

// ====================================================================== HTTP backend
struct RemoteFile { std::string rel; uint64_t size; uint32_t id; };
struct MemFile { std::vector<uint8_t> data; std::mutex m; };

static std::map<std::string, RemoteFile> g_remote;                 // lower(rel path) -> info
static std::map<std::string, std::vector<Entry>> g_dirs;           // lower(rel dir) -> entries
static std::mutex g_mutex;
static std::map<std::string, std::shared_ptr<MemFile>> g_mem;      // lower(full path) -> file
static std::set<std::string> g_memDirs;                            // lower(full path)
static std::map<std::string, std::string> g_memOrig;              // lower(full path) -> the path as the engine spelled it (listings keep the capitals, as NTFS does)
static std::string leafOf(const std::string &p) { size_t sl = p.rfind('/'); return sl == std::string::npos ? p : p.substr(sl + 1); }
static std::string displayLeaf(const std::string &lowerPath) { auto it = g_memOrig.find(lowerPath); return leafOf(it != g_memOrig.end() ? it->second : lowerPath); }

struct Handle {
    bool used = false;
    std::shared_ptr<MemFile> mem;
    std::string rel; uint64_t size = 0;      // remote
    int64_t pos = 0;
    bool writable = false, append = false;
    bool persist = false; std::string persistRel;   // a file under players/: saved to the browser's storage when closed (see "profile persistence")
    // remote read state (see remoteRead): sequential-stream detection + a private buffer for streaming reads
    uint32_t fileId = 0;
    int streak = 0;                  // consecutive fetches that continued right where the previous one ended
    int64_t fetchEnd = -1;           // end offset of the previous fetch
    int64_t sOff = -1; std::vector<uint8_t> sbuf;   // stream buffer: [sOff, sOff + sbuf.size())
};
static std::vector<Handle> g_handles;
static const int kHandleBase = 1000;
// Handles below kHandleBase are real file descriptors (node); memory / remote handles start at kHandleBase.
static inline bool posixHandle(int h) { return !g_memFs || h < kHandleBase; }

EM_JS(char *, kvfs_fetch_manifest, (), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open('GET', '/data-manifest.json', false);
        xhr.send(null);
        if (xhr.status !== 200) return 0;
        return stringToNewUTF8(xhr.responseText);
    } catch (e) { return 0; }
});

EM_JS(int, kvfs_fetch_range, (const char *relPath, double off, int len, void *dst), {
    const path = UTF8ToString(relPath).split('/').map(encodeURIComponent).join('/');
    const xhr = new XMLHttpRequest();
    xhr.open('GET', '/data/' + path, false);
    xhr.responseType = 'arraybuffer';
    xhr.setRequestHeader('Range', 'bytes=' + off + '-' + (off + len - 1));
    try { xhr.send(null); } catch (e) { return -1; }
    if (xhr.status !== 206 && xhr.status !== 200) return -2;
    const buf = new Uint8Array(xhr.response);
    if (xhr.status === 200) {
        const sl = buf.subarray(off, off + len);
        HEAPU8.set(sl, dst);
        return sl.length;
    }
    const n = Math.min(buf.length, len);
    HEAPU8.set(buf.subarray(0, n), dst);
    return n;
});

// ---- bridge to web/app/io-worker.js ------------------------------------------------------------------------------------------------
// All remote range reads go through the I/O worker (persistent cache + parallel fetches). Each calling thread owns one request slot in a
// control block in shared memory, posts its request and blocks on it. If the worker cannot start, the reads fall back to plain sync XHR.
static const int kIoSlots = 16, kIoSlotWords = 8, kIoSlot0 = 8;
alignas(64) static volatile int32_t g_ioCtl[kIoSlot0 + kIoSlots * kIoSlotWords];
static int g_ioState = 0;                                  // 0 not started, 1 ready, 2 unavailable
static std::atomic<int> g_ioNextSlot{0};

// Asks the page (main thread) to start the worker, then waits for its ready flag. 1 = persistent cache, 3 = network only, 2/0 = unavailable.
static int ioStart()
{
    MAIN_THREAD_ASYNC_EM_ASM({
        if (typeof kisakIoAttach === 'function') kisakIoAttach(wasmMemory, $0);
        else { const I = new Int32Array(wasmMemory.buffer); Atomics.store(I, ($0 >> 2) + 1, 2); Atomics.notify(I, ($0 >> 2) + 1); }
    }, (uintptr_t)g_ioCtl);
    return EM_ASM_INT({
        const I = HEAP32;
        const w = ($0 >> 2) + 1;
        const t0 = Date.now();
        while (Atomics.load(I, w) === 0 && Date.now() - t0 < 20000) Atomics.wait(I, w, 0, 100);
        return Atomics.load(I, w);
    }, (uintptr_t)g_ioCtl);
}

// Blocking range read through the worker. Returns bytes read, or < 0 (-3: this thread has no slot).
EM_JS(int, kvfs_io_call, (int32_t *ctl, int slot, const char *path, double off, int len, void *dst), {
    const I = HEAP32, b = (ctl >> 2) + 8 + slot * 8;
    Atomics.store(I, b + 1, path);
    Atomics.store(I, b + 2, off % 4294967296 | 0);
    Atomics.store(I, b + 3, Math.floor(off / 4294967296));
    Atomics.store(I, b + 4, len);
    Atomics.store(I, b + 5, dst);
    Atomics.store(I, b + 7, 0);                     // op 0: range read
    Atomics.store(I, b, 1);
    Atomics.add(I, ctl >> 2, 1);
    Atomics.notify(I, ctl >> 2);
    while (Atomics.load(I, b) !== 2) Atomics.wait(I, b, Atomics.load(I, b), 1000);
    const r = Atomics.load(I, b + 6);
    Atomics.store(I, b, 0);
    return r;
});

// This thread's request slot, or -1 if all are taken.
static int ioSlot()
{
    static thread_local int slot = -2;
    if (slot == -2) { int s = g_ioNextSlot.fetch_add(1); slot = s < kIoSlots ? s : -1; }
    return slot;
}

static int ioFetch(const std::string &rel, int64_t off, int len, void *dst)
{
    int slot = ioSlot();
    if (slot < 0) return -3;
    return kvfs_io_call((int32_t *)g_ioCtl, slot, rel.c_str(), (double)off, len, dst);
}

// ---- profile persistence -----------------------------------------------------------------------------------------------------------
// The engine's player profiles (config, stats/unlocks/custom classes, profile list, active profile) are files under <root>/players/ that live in
// memory (g_mem). They are mirrored to the I/O worker, which keeps them in IndexedDB (see "profile persistence" in app/io-worker.js): loaded back
// into g_mem by kvfs::init, and every close of a written file / unlink / mkdir / rmdir below players/ is sent as it happens.
enum { kProfSize = 1, kProfData = 2, kProfPut = 3, kProfDel = 4, kProfMkdir = 5 };
static bool g_profPersist = false;

EM_JS(int, kvfs_prof_call, (int32_t *ctl, int slot, int op, const char *path, const void *data, int len), {
    const I = HEAP32, b = (ctl >> 2) + 8 + slot * 8;
    Atomics.store(I, b + 1, path);
    Atomics.store(I, b + 4, len);
    Atomics.store(I, b + 5, data);
    Atomics.store(I, b + 7, op);
    Atomics.store(I, b, 1);
    Atomics.add(I, ctl >> 2, 1);
    Atomics.notify(I, ctl >> 2);
    while (Atomics.load(I, b) !== 2) Atomics.wait(I, b, Atomics.load(I, b), 1000);
    const r = Atomics.load(I, b + 6);
    Atomics.store(I, b, 0);
    return r;
});

static int profCall(int op, const std::string &rel, const void *data, int len)
{
    if (!g_profPersist) return -3;
    int slot = ioSlot();
    if (slot < 0) return -3;
    return kvfs_prof_call((int32_t *)g_ioCtl, slot, op, rel.c_str(), data, len);
}

// Is `path` (any case) <root>/players or below it? Then *rel = the path relative to the root, as spelled.
static bool isPersistPath(const std::string &path, std::string *rel)
{
    const std::string lp = lower(path), pre = g_root + "/players";
    if (lp.size() < pre.size() || lp.compare(0, pre.size(), pre) != 0) return false;
    if (lp.size() > pre.size() && lp[pre.size()] != '/') return false;
    *rel = path.substr(g_root.size() + 1);
    return true;
}

// Bring back what an earlier session saved (before the engine looks for profiles).
static void profileLoad()
{
    int n = profCall(kProfSize, "", nullptr, 0);
    if (n <= 0) return;
    std::vector<uint8_t> blob((size_t)n);
    int got = profCall(kProfData, "", blob.data(), n);
    if (got != n) { fprintf(stderr, "[kvfs] the saved profile could not be read (%d)\n", got); return; }
    auto rd32 = [&](size_t p) { uint32_t v; memcpy(&v, &blob[p], 4); return v; };
    size_t p = 0, files = 0, dirs = 0;
    if ((size_t)n < 4) return;
    uint32_t count = rd32(p); p += 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 5 > (size_t)n) break;
        bool isDir = blob[p++] != 0;
        uint32_t pl = rd32(p); p += 4;
        if (p + pl + 4 > (size_t)n) break;
        std::string rel((const char *)&blob[p], pl); p += pl;
        uint32_t dl = rd32(p); p += 4;
        if (p + dl > (size_t)n) break;
        std::string full = g_rootExact + "/" + rel, key = lower(full), chk;
        if (!isPersistPath(full, &chk)) { p += dl; continue; }          // only ever restore below players/
        g_memOrig[key] = full;
        if (isDir) { g_memDirs.insert(key); ++dirs; }
        else {
            auto mf = std::make_shared<MemFile>();
            mf->data.assign(blob.begin() + p, blob.begin() + p + dl);
            g_mem[key] = mf; ++files;
        }
        p += dl;
    }
    fprintf(stderr, "[kvfs] saved profile restored: %zu files, %zu directories\n", files, dirs);
}

// ---- request statistics (KISAK_FSSTATS=1 / `--fsstats`): what the engine reads and what it costs ----------------------------------
struct FsStat { uint64_t opens = 0, reqs = 0, fetched = 0, wanted = 0; double ms = 0; };
static std::map<std::string, FsStat> g_stats;
static std::mutex g_statMutex;
static double g_statLast = 0, g_statStart = 0;
static bool statsOn() { return getenv("KISAK_FSSTATS") != nullptr; }   // not cached: files are opened before main() parses --fsstats

static void dumpStats()
{
    FsStat tot; std::vector<std::pair<std::string, FsStat>> rows;
    for (auto &kv : g_stats) { rows.push_back(kv); tot.opens += kv.second.opens; tot.reqs += kv.second.reqs; tot.fetched += kv.second.fetched; tot.wanted += kv.second.wanted; tot.ms += kv.second.ms; }
    std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.second.fetched > b.second.fetched; });
    fprintf(stderr, "[fsstats] t=%.1fs  %llu requests, %.1f MB fetched (%.1f MB wanted by the engine), %.1fs blocked in fetches (all threads), %llu opens\n",
            (emscripten_get_now() - g_statStart) / 1000.0, (unsigned long long)tot.reqs, tot.fetched / 1048576.0, tot.wanted / 1048576.0, tot.ms / 1000.0, (unsigned long long)tot.opens);
    if (g_ioState == 1)
        fprintf(stderr, "[fsstats] I/O worker: %d network requests (%.1f MB), %d requests served from the persistent cache (%.1f MB), %d read-aheads\n",
                g_ioCtl[2], g_ioCtl[3] / 1024.0, g_ioCtl[4], g_ioCtl[5] / 1024.0, g_ioCtl[6]);
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
        const FsStat &f = rows[i].second;
        fprintf(stderr, "[fsstats]   %-40s %6llu req %8.1f MB fetched %8.1f MB wanted  %6.1fs  %5llu opens\n", rows[i].first.c_str(), (unsigned long long)f.reqs,
                f.fetched / 1048576.0, f.wanted / 1048576.0, f.ms / 1000.0, (unsigned long long)f.opens);
    }
}

static int rawFetch(const std::string &rel, int64_t off, int len, void *dst)
{
    if (g_ioState == 1) {
        int r = ioFetch(rel, off, len, dst);
        if (r >= 0) return r;                              // (-3 = no slot for this thread, < -3 / -1 / -2 = worker failed: use XHR)
    }
    return kvfs_fetch_range(rel.c_str(), (double)off, len, dst);
}

static int fetchRange(const std::string &rel, int64_t off, int len, void *dst)
{
    if (!statsOn()) return rawFetch(rel, off, len, dst);
    double t0 = emscripten_get_now();
    int r = rawFetch(rel, off, len, dst);
    double t1 = emscripten_get_now();
    std::lock_guard<std::mutex> g(g_statMutex);
    if (g_statStart == 0) g_statStart = t0;
    FsStat &st = g_stats[rel];
    st.reqs++; st.ms += t1 - t0; if (r > 0) st.fetched += (uint64_t)r;
    if (t1 - g_statLast > 5000) { g_statLast = t1; dumpStats(); }
    return r;
}
static void noteWanted(const std::string &rel, size_t n, int64_t off, uint64_t fileSize)
{
    if (!statsOn()) return;
    std::lock_guard<std::mutex> g(g_statMutex);
    g_stats[rel].wanted += n;
    const char *tr = getenv("KISAK_FSSTATS");
    static int shown = 0;
    // trace mode: log the first reads of .iwd files outside their central directory (the last ~400 KB)
    if (tr && tr[0] == '2' && shown < 500 && rel.find(".iwd") != std::string::npos && (uint64_t)off + 400000 < fileSize) { ++shown; fprintf(stderr, "[fsread] %s off=%lld n=%zu\n", rel.c_str(), (long long)off, n); }
}
static void noteOpen(const std::string &rel) { if (statsOn()) { std::lock_guard<std::mutex> g(g_statMutex); g_stats[rel].opens++; } }

static void parseManifest(const char *json)
{
    // {"files": {"main/a.iwd": 123, ...}}   (written by web/tools/serve.py; paths contain no escapes beyond \\ and \")
    const char *p = strstr(json, "\"files\"");
    if (!p) return;
    p = strchr(p, '{');
    if (!p) return;
    ++p;
    while (*p) {
        while (*p && *p != '"' && *p != '}') ++p;
        if (*p != '"') break;
        ++p;
        std::string name;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) ++p;
            name += *p++;
        }
        if (*p == '"') ++p;
        while (*p && *p != ':') ++p;
        if (*p == ':') ++p;
        uint64_t size = strtoull(p, (char **)&p, 10);
        g_remote[lower(name)] = RemoteFile{name, size, (uint32_t)g_remote.size()};
        // directory index
        std::string dir;
        size_t start = 0;
        for (;;) {
            size_t sl = name.find('/', start);
            if (sl == std::string::npos) {
                g_dirs[lower(dir)].push_back(Entry{name.substr(start), false, size});
                break;
            }
            std::string sub = name.substr(start, sl - start);
            std::string ld = lower(dir);
            auto &v = g_dirs[ld];
            bool have = false;
            for (auto &e : v) if (e.isDir && strcasecmp(e.name.c_str(), sub.c_str()) == 0) { have = true; break; }
            if (!have) v.push_back(Entry{sub, true, 0});
            dir = dir.empty() ? sub : dir + "/" + sub;
            start = sl + 1;
        }
    }
}

void init(const char *installRoot)
{
    if (g_inited) return;
    g_inited = true;
    g_browser = EM_ASM_INT({ return (typeof process !== 'undefined' && process.versions && process.versions.node) ? 0 : 1; }) != 0;
    g_memFs = g_browser || getenv("KISAK_MEMWRITE") != nullptr;
    g_rootExact = installRoot ? installRoot : "";
    while (g_rootExact.size() > 1 && g_rootExact.back() == '/') g_rootExact.pop_back();
    g_root = lower(g_rootExact);
    g_handles.resize(64);
    if (!g_browser) return;
    char *json = kvfs_fetch_manifest();
    if (!json) { fprintf(stderr, "[kvfs] could not fetch /data-manifest.json (is web/tools/serve.py running with --data?)\n"); return; }
    parseManifest(json);
    free(json);
    fprintf(stderr, "[kvfs] browser backend: %zu remote files under %s\n", g_remote.size(), g_rootExact.c_str());
    int io = ioStart();
    g_ioState = (io == 1 || io == 3) ? 1 : 2;
    fprintf(stderr, "[kvfs] data I/O: %s\n", io == 1 ? "worker with persistent cache" : io == 3 ? "worker, network only (cache unavailable or in use by another tab)" : "plain XHR (worker unavailable)");
    g_profPersist = g_ioState == 1;
    if (g_profPersist) profileLoad();
    else fprintf(stderr, "[kvfs] profiles are not saved (the I/O worker is unavailable)\n");
}

// returns true if `path` is below the install root; sets rel (relative path, original case as given)
static bool underRoot(const std::string &path, std::string *rel)
{
    std::string lp = lower(path);
    if (lp.size() > g_root.size() && lp.compare(0, g_root.size(), g_root) == 0 && lp[g_root.size()] == '/') { *rel = path.substr(g_root.size() + 1); return true; }
    if (lp == g_root) { *rel = ""; return true; }
    return false;
}

static const RemoteFile *findRemote(const std::string &path)
{
    std::string rel;
    if (!underRoot(path, &rel)) return nullptr;
    auto it = g_remote.find(lower(rel));
    return it == g_remote.end() ? nullptr : &it->second;
}

static int allocHandle()
{
    for (size_t i = 0; i < g_handles.size(); ++i) if (!g_handles[i].used) { g_handles[i] = Handle(); g_handles[i].used = true; return (int)i + kHandleBase; }
    g_handles.emplace_back();
    g_handles.back().used = true;
    return (int)g_handles.size() - 1 + kHandleBase;
}
static Handle *handleOf(int h) { int i = h - kHandleBase; return (i >= 0 && i < (int)g_handles.size() && g_handles[i].used) ? &g_handles[i] : nullptr; }

// ====================================================================== public API
int open(const std::string &path, int of)
{
    if (!g_memFs) {
        std::string r = path;
        if (!(of & O_CREAT)) { if (!posixResolve(path, r)) { errno = ENOENT; return -1; } }
        return ::open(r.c_str(), of, 0666);
    }
    std::lock_guard<std::mutex> g(g_mutex);
    std::string lp = lower(path);
    bool write = (of & (O_WRONLY | O_RDWR)) != 0;
    auto mit = g_mem.find(lp);
    if (!write) {
        if (mit != g_mem.end()) {
            int h = allocHandle(); Handle *hd = handleOf(h); hd->mem = mit->second; return h;
        }
        if (!g_browser) {   // node, writes in memory: everything not written by the engine is read from the real files
            std::string r;
            if (!posixResolve(path, r)) { errno = ENOENT; return -1; }
            return ::open(r.c_str(), of & ~O_CREAT, 0666);
        }
        const RemoteFile *rf = findRemote(path);
        if (!rf) { errno = ENOENT; return -1; }
        int h = allocHandle(); Handle *hd = handleOf(h); hd->rel = rf->rel; hd->size = rf->size; hd->fileId = rf->id;
        if (traceFs()) fprintf(stderr, "[kvfs] open remote %s (%llu bytes)\n", rf->rel.c_str(), (unsigned long long)rf->size);
        noteOpen(rf->rel);
        return h;
    }
    std::shared_ptr<MemFile> mf;
    if (mit != g_mem.end() && !(of & O_TRUNC)) mf = mit->second;
    else {
        if ((of & O_EXCL) && mit != g_mem.end()) { errno = EEXIST; return -1; }
        mf = std::make_shared<MemFile>();
        if (!g_browser && !(of & O_TRUNC)) {   // node: opening a file that exists on disk for update starts from its content (copy-up; the disk file is untouched)
            std::string r; struct stat st;
            if (posixResolve(path, r) && ::stat(r.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (64 << 20)) {
                int fd = ::open(r.c_str(), O_RDONLY);
                if (fd >= 0) {
                    mf->data.resize((size_t)st.st_size);
                    ssize_t got = ::read(fd, mf->data.data(), mf->data.size());
                    mf->data.resize(got > 0 ? (size_t)got : 0);
                    ::close(fd);
                }
            }
        }
        g_mem[lp] = mf;
        g_memOrig[lp] = path;
    }
    int h = allocHandle(); Handle *hd = handleOf(h); hd->mem = mf; hd->writable = true; hd->append = (of & O_APPEND) != 0;
    hd->persist = g_profPersist && isPersistPath(path, &hd->persistRel);
    return h;
}

int close(int h)
{
    if (posixHandle(h)) return ::close(h);
    std::string saveRel; std::vector<uint8_t> snapshot; bool save = false;
    {
        std::lock_guard<std::mutex> g(g_mutex);
        Handle *hd = handleOf(h);
        if (!hd) return -1;
        if (hd->writable && hd->persist && hd->mem) {      // a profile file was written: remember what it contains now
            save = true; saveRel = hd->persistRel;
            std::lock_guard<std::mutex> gm(hd->mem->m);
            snapshot = hd->mem->data;
        }
        *hd = Handle();
    }
    if (save) {                                            // (outside the locks: this waits for the I/O worker)
        int r = profCall(kProfPut, saveRel, snapshot.data(), (int)snapshot.size());
        if (r < 0) fprintf(stderr, "[kvfs] could not save %s (%d)\n", saveRel.c_str(), r);
        else if (r == 0) fprintf(stderr, "[kvfs] saved %s (%zu bytes)\n", saveRel.c_str(), snapshot.size());
    }
    return 0;
}

// ---- remote reads: shared block cache + sequential read-ahead --------------------------------------------------------------------
// The engine reads the .iwd archives (167 MB each) with tiny reads (3 B, 1 KB, a few KB) at scattered offsets, and the .ff fastfiles
// with big sequential reads. Both go through synchronous XHR range requests, so cost = requests x round-trip + bytes / bandwidth.
//  * Random reads: fetch only the 64 KB blocks that cover the read (about one round trip, ~1.5x the bytes actually needed) and keep them
//    in a global LRU cache shared by all handles, so neighbouring reads, re-opened files and other threads hit the cache. (The previous
//    1 MB per-handle blocks fetched ~6x more data than the engine used, up to 17x for some archives.)
//  * Sequential reads: when a read misses right where the previous fetch ended, the fetch window doubles (128 KB ... 4 MB). Large windows
//    go to a private per-handle stream buffer instead of the LRU, so streaming a 57 MB fastfile does not flush the archive blocks.
static const int64_t kGran = 64 * 1024;                    // cache block size
static const int kMaxStreak = 6;                           // window = kGran << streak: 128 KB ... 4 MB
static const int kStreamStreak = 3;                        // windows of 512 KB and up use the stream buffer
static const size_t kCacheBudget = 96u << 20;              // bytes of cached blocks

struct CBlock { std::vector<uint8_t> data; std::list<uint64_t>::iterator lru; };
static std::unordered_map<uint64_t, CBlock> g_blocks;      // key = fileId << 32 | block index
static std::list<uint64_t> g_lru;                          // most recently used first
static size_t g_cacheBytes = 0;
static std::mutex g_cacheMutex;
static inline uint64_t blockKey(uint32_t file, int64_t blk) { return ((uint64_t)file << 32) | (uint64_t)blk; }

static void cacheInsert(uint32_t file, int64_t blk, const uint8_t *src, size_t len)   // g_cacheMutex held
{
    uint64_t k = blockKey(file, blk);
    if (g_blocks.count(k)) return;
    g_lru.push_front(k);
    CBlock &b = g_blocks[k];
    b.data.assign(src, src + len);
    b.lru = g_lru.begin();
    g_cacheBytes += len;
    while (g_cacheBytes > kCacheBudget && g_lru.size() > 1) {
        uint64_t victim = g_lru.back();
        g_lru.pop_back();
        auto it = g_blocks.find(victim);
        g_cacheBytes -= it->second.data.size();
        g_blocks.erase(it);
    }
}

static int64_t remoteRead(Handle *hd, void *vbuf, size_t n, int64_t off)
{
    uint8_t *buf = (uint8_t *)vbuf;
    if (off >= (int64_t)hd->size) return 0;
    if ((uint64_t)off + n > hd->size) n = (size_t)(hd->size - off);
    if (!n) return 0;
    noteWanted(hd->rel, n, off, hd->size);
    const int64_t size = (int64_t)hd->size, end = off + (int64_t)n;
    if (n >= (4u << 20)) {   // huge reads go straight into the destination (no staging copy)
        int r = fetchRange(hd->rel, off, (int)n, buf);
        return r < 0 ? -1 : r;
    }
    int64_t pos = off;
    while (pos < end) {
        // 1. the handle's stream buffer
        if (hd->sOff >= 0 && pos >= hd->sOff && pos < hd->sOff + (int64_t)hd->sbuf.size()) {
            size_t c = (size_t)std::min<int64_t>(end - pos, hd->sOff + (int64_t)hd->sbuf.size() - pos);
            memcpy(buf + (pos - off), hd->sbuf.data() + (pos - hd->sOff), c);
            pos += (int64_t)c;
            continue;
        }
        // 2. the shared block cache
        int64_t blk = pos / kGran;
        {
            std::lock_guard<std::mutex> g(g_cacheMutex);
            auto it = g_blocks.find(blockKey(hd->fileId, blk));
            if (it != g_blocks.end()) {
                g_lru.splice(g_lru.begin(), g_lru, it->second.lru);
                const std::vector<uint8_t> &d = it->second.data;
                int64_t inBlk = pos - blk * kGran;
                size_t c = (size_t)std::min<int64_t>(end - pos, (int64_t)d.size() - inBlk);
                memcpy(buf + (pos - off), d.data() + inBlk, c);
                pos += (int64_t)c;
                continue;
            }
        }
        // 3. miss: one range request. A miss right where the previous fetch ended continues a sequential stream: grow the window.
        bool sequential = hd->fetchEnd >= 0 && pos >= hd->fetchEnd && pos - hd->fetchEnd <= kGran;
        hd->streak = sequential ? std::min(hd->streak + 1, kMaxStreak) : 0;
        int64_t window = hd->streak ? (kGran << hd->streak) : 0;
        bool stream = hd->streak >= kStreamStreak;
        int64_t from = stream ? pos : blk * kGran;
        int64_t to = std::max(end, pos + window);
        to = std::min(size, (to + kGran - 1) / kGran * kGran);
        if (stream) to = std::min(size, std::max(to, end));
        size_t len = (size_t)(to - from);
        std::vector<uint8_t> tmp(len);
        int r = fetchRange(hd->rel, from, (int)len, tmp.data());
        if (r <= 0) return pos > off ? pos - off : -1;
        hd->fetchEnd = from + r;
        if (stream) {
            hd->sbuf.swap(tmp);
            hd->sbuf.resize((size_t)r);
            hd->sOff = from;
        } else {
            std::lock_guard<std::mutex> g(g_cacheMutex);
            for (int64_t b = from / kGran; b * kGran < from + r; ++b) {
                int64_t bs = b * kGran;
                size_t bl = (size_t)std::min<int64_t>(kGran, from + r - bs);
                if (bl == (size_t)std::min<int64_t>(kGran, size - bs)) cacheInsert(hd->fileId, b, tmp.data() + (bs - from), bl);   // whole blocks only
            }
            // serve this read straight from the fetched bytes (the blocks may be evicted by another thread before the next loop)
            int64_t avail = std::min<int64_t>(end, from + r);
            if (avail > pos) { memcpy(buf + (pos - off), tmp.data() + (pos - from), (size_t)(avail - pos)); pos = avail; }
        }
        if (stream) continue;   // next iteration copies from the stream buffer
    }
    return (int64_t)n;
}

int64_t pread(int h, void *buf, size_t n, int64_t off)
{
    if (posixHandle(h)) return ::pread(h, buf, n, off);
    Handle *hd;
    { std::lock_guard<std::mutex> g(g_mutex); hd = handleOf(h); if (!hd) { errno = EBADF; return -1; } }
    if (hd->mem) {
        std::lock_guard<std::mutex> g(hd->mem->m);
        if (off >= (int64_t)hd->mem->data.size()) return 0;
        size_t c = std::min<size_t>(n, hd->mem->data.size() - (size_t)off);
        memcpy(buf, hd->mem->data.data() + off, c);
        return (int64_t)c;
    }
    return remoteRead(hd, buf, n, off);
}

int64_t read(int h, void *buf, size_t n)
{
    if (posixHandle(h)) return ::read(h, buf, n);
    Handle *hd;
    { std::lock_guard<std::mutex> g(g_mutex); hd = handleOf(h); if (!hd) { errno = EBADF; return -1; } }
    int64_t r = pread(h, buf, n, hd->pos);
    if (r > 0) hd->pos += r;
    return r;
}

int64_t write(int h, const void *buf, size_t n)
{
    if (posixHandle(h)) return ::write(h, buf, n);
    Handle *hd;
    { std::lock_guard<std::mutex> g(g_mutex); hd = handleOf(h); if (!hd || !hd->mem || !hd->writable) { errno = EBADF; return -1; } }
    std::lock_guard<std::mutex> g(hd->mem->m);
    if (hd->append) hd->pos = (int64_t)hd->mem->data.size();
    if ((size_t)hd->pos + n > hd->mem->data.size()) hd->mem->data.resize((size_t)hd->pos + n);
    memcpy(hd->mem->data.data() + hd->pos, buf, n);
    hd->pos += (int64_t)n;
    return (int64_t)n;
}

int64_t lseek(int h, int64_t off, int whence)
{
    if (posixHandle(h)) return ::lseek(h, off, whence);
    std::lock_guard<std::mutex> g(g_mutex);
    Handle *hd = handleOf(h);
    if (!hd) { errno = EBADF; return -1; }
    int64_t size = hd->mem ? (int64_t)hd->mem->data.size() : (int64_t)hd->size;
    int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? hd->pos + off : size + off;
    if (np < 0) { errno = EINVAL; return -1; }
    hd->pos = np;
    return np;
}

bool fileSize(int h, int64_t *size)
{
    if (posixHandle(h)) { struct stat st; if (::fstat(h, &st) != 0) return false; *size = st.st_size; return true; }
    std::lock_guard<std::mutex> g(g_mutex);
    Handle *hd = handleOf(h);
    if (!hd) return false;
    *size = hd->mem ? (int64_t)hd->mem->data.size() : (int64_t)hd->size;
    return true;
}

static bool posixStat(const std::string &path, bool *isDir, int64_t *size, std::string *canonical)
{
    std::string r;
    if (!posixResolve(path, r)) return false;
    struct stat st;
    if (::stat(r.c_str(), &st) != 0) return false;
    if (isDir) *isDir = S_ISDIR(st.st_mode);
    if (size) *size = st.st_size;
    if (canonical) *canonical = r;
    return true;
}

bool statPath(const std::string &path, bool *isDir, int64_t *size, std::string *canonical)
{
    if (!g_memFs) return posixStat(path, isDir, size, canonical);
    std::lock_guard<std::mutex> g(g_mutex);
    std::string lp = lower(path);
    auto mit = g_mem.find(lp);
    if (mit != g_mem.end()) { if (isDir) *isDir = false; if (size) *size = (int64_t)mit->second->data.size(); if (canonical) *canonical = path; return true; }
    if (g_memDirs.count(lp)) { if (isDir) *isDir = true; if (size) *size = 0; if (canonical) *canonical = path; return true; }
    if (!g_browser) return posixStat(path, isDir, size, canonical);   // node, writes in memory: the rest is the real data directory
    std::string rel;
    if (!underRoot(path, &rel)) return false;
    if (rel.empty() || g_dirs.count(lower(rel)) || false) {
        if (isDir) *isDir = true; if (size) *size = 0; if (canonical) *canonical = path; return true;
    }
    auto it = g_remote.find(lower(rel));
    if (it == g_remote.end()) return false;
    if (isDir) *isDir = false;
    if (size) *size = (int64_t)it->second.size;
    if (canonical) *canonical = g_rootExact + "/" + it->second.rel;
    return true;
}

bool listdir(const std::string &dir, std::vector<Entry> *out)
{
    out->clear();
    bool posixListed = false;   // node with writes in memory: the real directory's entries come first, then what the engine wrote
    if (!g_browser) {
        std::string r;
        bool ok = false;
        if (posixResolve(dir, r)) {
            if (DIR *d = opendir(r.c_str())) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    Entry en; en.name = e->d_name;
                    struct stat st; std::string full = r + "/" + en.name;
                    en.isDir = ::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
                    en.size = ::stat(full.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
                    out->push_back(en);
                }
                closedir(d);
                ok = true;
            }
        }
        if (!g_memFs) return ok;
        posixListed = ok;
    }
    std::lock_guard<std::mutex> g(g_mutex);
    std::string rel;
    bool found = posixListed;
    if (g_browser && underRoot(dir, &rel)) {
        auto it = g_dirs.find(lower(rel));
        if (it != g_dirs.end()) { *out = it->second; found = true; }
        else if (rel.empty()) found = true;
    }
    std::string ld = lower(dir);
    if (!ld.empty() && ld.back() == '/') ld.pop_back();
    for (auto &kv : g_mem) {
        size_t sl = kv.first.rfind('/');
        if (sl == std::string::npos) continue;
        if (kv.first.substr(0, sl) == ld) {
            std::string nm = displayLeaf(kv.first);
            bool dup = false;
            for (auto &e : *out) if (strcasecmp(e.name.c_str(), nm.c_str()) == 0) { dup = true; break; }
            if (!dup) out->push_back(Entry{nm, false, kv.second->data.size()});
            found = true;
        }
    }
    for (const std::string &d : g_memDirs) {          // in-memory subdirectories
        size_t sl = d.rfind('/');
        if (sl == std::string::npos || d.substr(0, sl) != ld) continue;
        std::string nm = displayLeaf(d);
        bool dup = false;
        for (auto &e : *out) if (strcasecmp(e.name.c_str(), nm.c_str()) == 0) { dup = true; break; }
        if (!dup) out->push_back(Entry{nm, true, 0});
        found = true;
    }
    if (g_memDirs.count(ld)) found = true;
    if (!found) return false;
    if (!posixListed) {
        out->push_back(Entry{".", true, 0});
        out->push_back(Entry{"..", true, 0});
    }
    return true;
}

bool unlink(const std::string &path)
{
    if (!g_memFs) return ::unlink(path.c_str()) == 0;
    std::string lp = lower(path), rel;
    bool ok;
    { std::lock_guard<std::mutex> g(g_mutex); ok = g_mem.erase(lp) > 0; if (ok) g_memOrig.erase(lp); }
    if (ok && g_profPersist && isPersistPath(path, &rel)) profCall(kProfDel, rel, nullptr, 0);
    return ok;
}
// The engine names directories both ways ("...\\profiles\\zed" when creating, "...\\profiles\\zed\\" when deleting a tree): a directory is recorded without a trailing slash.
static std::string noTrailingSlash(std::string p) { while (p.size() > 1 && p.back() == '/') p.pop_back(); return p; }

bool mkdir(const std::string &path_)
{
    if (!g_memFs) return ::mkdir(path_.c_str(), 0777) == 0;
    const std::string path = noTrailingSlash(path_);
    std::string lp = lower(path), rel;
    bool isNew;
    { std::lock_guard<std::mutex> g(g_mutex); isNew = g_memDirs.insert(lp).second; if (isNew) g_memOrig[lp] = path; }
    if (isNew && g_profPersist && isPersistPath(path, &rel)) profCall(kProfMkdir, rel, nullptr, 0);
    return true;
}
bool rmdir(const std::string &path_)
{
    if (!g_memFs) return ::rmdir(path_.c_str()) == 0;
    const std::string path = noTrailingSlash(path_);
    std::string lp = lower(path), rel;
    bool ok;
    { std::lock_guard<std::mutex> g(g_mutex); ok = g_memDirs.erase(lp) > 0; if (ok) g_memOrig.erase(lp); }
    if (ok && g_profPersist && isPersistPath(path, &rel)) profCall(kProfDel, rel, nullptr, 0);
    return ok;
}

} // namespace kvfs
