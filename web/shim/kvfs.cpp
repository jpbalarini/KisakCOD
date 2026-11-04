// kvfs.cpp -- see kvfs.h
#include "kvfs.h"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#include <emscripten.h>

#undef stat

namespace kvfs {

static bool g_browser = false;
static bool g_inited = false;
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
struct RemoteFile { std::string rel; uint64_t size; };
struct MemFile { std::vector<uint8_t> data; std::mutex m; };

static std::map<std::string, RemoteFile> g_remote;                 // lower(rel path) -> info
static std::map<std::string, std::vector<Entry>> g_dirs;           // lower(rel dir) -> entries
static std::mutex g_mutex;
static std::map<std::string, std::shared_ptr<MemFile>> g_mem;      // lower(full path) -> file
static std::set<std::string> g_memDirs;                            // lower(full path)

struct Handle {
    bool used = false;
    std::shared_ptr<MemFile> mem;
    std::string rel; uint64_t size = 0;      // remote
    int64_t pos = 0;
    bool writable = false, append = false;
    int64_t cacheOff = -1; std::vector<uint8_t> cache;
};
static std::vector<Handle> g_handles;
static const int kHandleBase = 1000;

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
        g_remote[lower(name)] = RemoteFile{name, size};
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
    if (!g_browser) {
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
        const RemoteFile *rf = findRemote(path);
        if (!rf) { errno = ENOENT; return -1; }
        int h = allocHandle(); Handle *hd = handleOf(h); hd->rel = rf->rel; hd->size = rf->size;
        if (traceFs()) fprintf(stderr, "[kvfs] open remote %s (%llu bytes)\n", rf->rel.c_str(), (unsigned long long)rf->size);
        return h;
    }
    std::shared_ptr<MemFile> mf;
    if (mit != g_mem.end() && !(of & O_TRUNC)) mf = mit->second;
    else {
        if ((of & O_EXCL) && mit != g_mem.end()) { errno = EEXIST; return -1; }
        mf = std::make_shared<MemFile>();
        g_mem[lp] = mf;
    }
    int h = allocHandle(); Handle *hd = handleOf(h); hd->mem = mf; hd->writable = true; hd->append = (of & O_APPEND) != 0;
    return h;
}

int close(int h)
{
    if (!g_browser || h < kHandleBase) return ::close(h);
    std::lock_guard<std::mutex> g(g_mutex);
    Handle *hd = handleOf(h);
    if (!hd) return -1;
    *hd = Handle();
    return 0;
}

static int64_t remoteRead(Handle *hd, void *buf, size_t n, int64_t off)
{
    if (off >= (int64_t)hd->size) return 0;
    if ((uint64_t)off + n > hd->size) n = (size_t)(hd->size - off);
    if (!n) return 0;
    const size_t kBlock = 1 << 20;
    if (n >= kBlock / 4) {   // large reads go straight into the destination
        int r = kvfs_fetch_range(hd->rel.c_str(), (double)off, (int)n, buf);
        return r < 0 ? -1 : r;
    }
    if (!(hd->cacheOff >= 0 && off >= hd->cacheOff && (uint64_t)(off + n) <= (uint64_t)hd->cacheOff + hd->cache.size())) {
        int64_t blockStart = off & ~(int64_t)(kBlock - 1);
        size_t len = (size_t)std::min<uint64_t>(kBlock, hd->size - blockStart);
        hd->cache.resize(len);
        int r = kvfs_fetch_range(hd->rel.c_str(), (double)blockStart, (int)len, hd->cache.data());
        if (r < 0) return -1;
        hd->cache.resize((size_t)r);
        hd->cacheOff = blockStart;
        if (!(off >= hd->cacheOff && (uint64_t)(off + n) <= (uint64_t)hd->cacheOff + hd->cache.size())) n = (size_t)std::max<int64_t>(0, hd->cacheOff + (int64_t)hd->cache.size() - off);
    }
    memcpy(buf, hd->cache.data() + (off - hd->cacheOff), n);
    return (int64_t)n;
}

int64_t pread(int h, void *buf, size_t n, int64_t off)
{
    if (!g_browser || h < kHandleBase) return ::pread(h, buf, n, off);
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
    if (!g_browser || h < kHandleBase) return ::read(h, buf, n);
    Handle *hd;
    { std::lock_guard<std::mutex> g(g_mutex); hd = handleOf(h); if (!hd) { errno = EBADF; return -1; } }
    int64_t r = pread(h, buf, n, hd->pos);
    if (r > 0) hd->pos += r;
    return r;
}

int64_t write(int h, const void *buf, size_t n)
{
    if (!g_browser || h < kHandleBase) return ::write(h, buf, n);
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
    if (!g_browser || h < kHandleBase) return ::lseek(h, off, whence);
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
    if (!g_browser || h < kHandleBase) { struct stat st; if (::fstat(h, &st) != 0) return false; *size = st.st_size; return true; }
    std::lock_guard<std::mutex> g(g_mutex);
    Handle *hd = handleOf(h);
    if (!hd) return false;
    *size = hd->mem ? (int64_t)hd->mem->data.size() : (int64_t)hd->size;
    return true;
}

bool statPath(const std::string &path, bool *isDir, int64_t *size, std::string *canonical)
{
    if (!g_browser) {
        std::string r;
        if (!posixResolve(path, r)) return false;
        struct stat st;
        if (::stat(r.c_str(), &st) != 0) return false;
        if (isDir) *isDir = S_ISDIR(st.st_mode);
        if (size) *size = st.st_size;
        if (canonical) *canonical = r;
        return true;
    }
    std::lock_guard<std::mutex> g(g_mutex);
    std::string lp = lower(path);
    auto mit = g_mem.find(lp);
    if (mit != g_mem.end()) { if (isDir) *isDir = false; if (size) *size = (int64_t)mit->second->data.size(); if (canonical) *canonical = path; return true; }
    if (g_memDirs.count(lp)) { if (isDir) *isDir = true; if (size) *size = 0; if (canonical) *canonical = path; return true; }
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
    if (!g_browser) {
        std::string r;
        if (!posixResolve(dir, r)) return false;
        DIR *d = opendir(r.c_str());
        if (!d) return false;
        struct dirent *e;
        while ((e = readdir(d))) {
            Entry en; en.name = e->d_name;
            struct stat st; std::string full = r + "/" + en.name;
            en.isDir = ::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            en.size = ::stat(full.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
            out->push_back(en);
        }
        closedir(d);
        return true;
    }
    std::lock_guard<std::mutex> g(g_mutex);
    std::string rel;
    bool found = false;
    if (underRoot(dir, &rel)) {
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
            std::string nm = kv.first.substr(sl + 1);
            bool dup = false;
            for (auto &e : *out) if (strcasecmp(e.name.c_str(), nm.c_str()) == 0) { dup = true; break; }
            if (!dup) out->push_back(Entry{nm, false, kv.second->data.size()});
            found = true;
        }
    }
    for (const std::string &d : g_memDirs) {          // in-memory subdirectories
        size_t sl = d.rfind('/');
        if (sl == std::string::npos || d.substr(0, sl) != ld) continue;
        std::string nm = d.substr(sl + 1);
        bool dup = false;
        for (auto &e : *out) if (strcasecmp(e.name.c_str(), nm.c_str()) == 0) { dup = true; break; }
        if (!dup) out->push_back(Entry{nm, true, 0});
        found = true;
    }
    if (g_memDirs.count(ld)) found = true;
    if (!found) return false;
    out->push_back(Entry{".", true, 0});
    out->push_back(Entry{"..", true, 0});
    return true;
}

bool unlink(const std::string &path)
{
    if (!g_browser) return ::unlink(path.c_str()) == 0;
    std::lock_guard<std::mutex> g(g_mutex);
    return g_mem.erase(lower(path)) > 0;
}
bool mkdir(const std::string &path)
{
    if (!g_browser) return ::mkdir(path.c_str(), 0777) == 0;
    std::lock_guard<std::mutex> g(g_mutex);
    g_memDirs.insert(lower(path));
    return true;
}
bool rmdir(const std::string &path)
{
    if (!g_browser) return ::rmdir(path.c_str()) == 0;
    std::lock_guard<std::mutex> g(g_mutex);
    return g_memDirs.erase(lower(path)) > 0;
}

} // namespace kvfs
