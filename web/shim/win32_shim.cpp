// win32_shim.cpp -- implementations of the Win32 calls declared in Windows.h, over POSIX/pthreads.
// Keeps the decompiled engine's calling patterns working unchanged (CreateFileA/ReadFileEx/SleepEx
// overlapped-read dance used by the fastfile loader, critical sections, events, threads, TLS...).
#include <Windows.h>
#include <io.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <errno.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <string>
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <emscripten.h>
#include "kvfs.h"
#undef fopen   // kisak_compat.h routes fopen/stat through this file; we need the real ones here
#undef stat

// ------------------------------------------------------------------ error state
static thread_local DWORD t_lastError = 0;
extern "C" DWORD GetLastError(void) { return t_lastError; }
extern "C" void SetLastError(DWORD e) { t_lastError = e; }

static bool traceFs() { static int v = -1; if (v < 0) v = getenv("KISAK_TRACE_FS") ? 1 : 0; return v == 1; }
// ------------------------------------------------------------------ handles
enum HType { H_FILE = 1, H_EVENT, H_THREAD, H_FIND };
struct W32Handle { int type; };
struct FileH : W32Handle { int fd; };
struct EventH : W32Handle { pthread_mutex_t m; pthread_cond_t c; bool signaled; bool manual; };
struct ThreadH : W32Handle { pthread_t th; volatile bool done; DWORD exitCode; LPTHREAD_START_ROUTINE fn; LPVOID arg; DWORD id; bool suspended; pthread_mutex_t m; pthread_cond_t c; };
struct FindH : W32Handle { std::vector<kvfs::Entry> entries; size_t next; std::string pattern; };

// ------------------------------------------------------------------ path handling
// The engine builds Windows-style paths ("C:\foo\bar", "zone\english\x.ff"). Normalise separators and fall back to a
// case-insensitive walk (game data is authored for a case-insensitive FS; the web FS is case-sensitive).
static const char *kroot() { const char *r = getenv("KISAK_INSTALL_PATH"); return (r && *r) ? r : "/cod4"; }
static std::string normPath(const char *in) {
    std::string p(in ? in : "");
    for (char &ch : p) if (ch == '\\') ch = '/';
    if (p.size() > 2 && p[1] == ':' && p[2] == '/') p = p.substr(2); // drop drive letter
    else if (p.find(':') != std::string::npos) return p;               // pseudo-paths such as "update:/x"
    if (p.empty() || p[0] != '/') p = std::string(kroot()) + "/" + p;  // the process cwd is the install root
    // collapse ".", ".." and duplicate separators
    std::vector<std::string> parts; size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i); if (j == std::string::npos) j = p.size();
        std::string c = p.substr(i, j - i);
        if (c == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!c.empty() && c != ".") parts.push_back(c);
        i = j + 1;
    }
    std::string out;
    for (auto &c : parts) out += "/" + c;
    if (out.empty()) out = "/";
    if (p.size() > 1 && p.back() == '/' && out.size() > 1) out += "/";
    return out;
}
static void ensureInit() { kvfs::init(kroot()); }
static bool resolveCI(const std::string &path, std::string &out) {
    ensureInit();
    return kvfs::statPath(path, nullptr, nullptr, &out);
}

extern "C" {

// ------------------------------------------------------------------ files
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)flags; (void)tmpl;
    ensureInit();
    std::string p = normPath(name);
    if (traceFs()) fprintf(stderr, "[fs] CreateFile(%s) access=%lx disp=%lu\n", name, access, disp);
    if (p.rfind("update:", 0) == 0) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    int of = 0;
    bool wr = (access & GENERIC_WRITE) != 0, rd = (access & GENERIC_READ) != 0;
    of = (wr && rd) ? O_RDWR : (wr ? O_WRONLY : O_RDONLY);
    switch (disp) {
        case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: of |= O_CREAT; break;
        case TRUNCATE_EXISTING: of |= O_TRUNC; break;
        default: break;
    }
    int fd = kvfs::open(p, of);
    if (traceFs()) fprintf(stderr, "[fs]   open('%s', flags=%x) -> fd=%d errno=%d\n", p.c_str(), of, fd, fd < 0 ? errno : 0);
    if (fd < 0) { SetLastError(errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    FileH *h = new FileH; h->type = H_FILE; h->fd = fd; return h;
}
static int fdOf(HANDLE hh) { W32Handle *h = (W32Handle *)hh; return (h && h != INVALID_HANDLE_VALUE && h->type == H_FILE) ? ((FileH *)h)->fd : -1; }

BOOL ReadFile(HANDLE f, LPVOID buf, DWORD n, LPDWORD out, LPOVERLAPPED ov) {
    int fd = fdOf(f); if (fd < 0) { SetLastError(6); return FALSE; }
    int64_t r = ov ? kvfs::pread(fd, buf, n, ov->Offset | ((uint64_t)ov->OffsetHigh << 32)) : kvfs::read(fd, buf, n);
    if (r < 0) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (out) *out = (DWORD)r;
    if (r == 0 && n != 0) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
    return TRUE;
}
// "Overlapped" read: performed synchronously; the completion routine is queued and delivered by the next alertable
// SleepEx on this thread (that is exactly how the fastfile loader waits for it).
struct PendingApc { LPOVERLAPPED_COMPLETION_ROUTINE cb; LPOVERLAPPED ov; DWORD bytes; DWORD err; };
static thread_local PendingApc t_apc[8]; static thread_local int t_apcCount = 0;
BOOL ReadFileEx(HANDLE f, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE cb) {
    int fd = fdOf(f); if (fd < 0 || !ov) { SetLastError(6); return FALSE; }
    int64_t r = kvfs::pread(fd, buf, n, ov->Offset | ((uint64_t)ov->OffsetHigh << 32));
    if (r < 0) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (r == 0 && n != 0) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
    if (t_apcCount < 8) t_apc[t_apcCount++] = { cb, ov, (DWORD)r, 0 };
    return TRUE;
}
BOOL WriteFile(HANDLE f, LPCVOID buf, DWORD n, LPDWORD out, LPOVERLAPPED ov) {
    (void)ov; int fd = fdOf(f); if (fd < 0) { SetLastError(6); return FALSE; }
    int64_t r = kvfs::write(fd, buf, n); if (r < 0) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (out) *out = (DWORD)r; return TRUE;
}
DWORD GetFileSize(HANDLE f, LPDWORD high) {
    int fd = fdOf(f); if (fd < 0) return INVALID_FILE_SIZE;
    int64_t sz; if (!kvfs::fileSize(fd, &sz)) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)((uint64_t)sz >> 32);
    return (DWORD)sz;
}
DWORD SetFilePointer(HANDLE f, LONG dist, PLONG distHigh, DWORD method) {
    int fd = fdOf(f); if (fd < 0) return INVALID_SET_FILE_POINTER;
    int64_t off = dist; if (distHigh) off |= (int64_t)*distHigh << 32;
    int64_t r = kvfs::lseek(fd, off, method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return INVALID_SET_FILE_POINTER; if (distHigh) *distHigh = (LONG)((uint64_t)r >> 32); return (DWORD)r;
}
BOOL FlushFileBuffers(HANDLE) { return TRUE; }
BOOL CloseHandle(HANDLE hh) {
    W32Handle *h = (W32Handle *)hh; if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    switch (h->type) {
        case H_FILE: kvfs::close(((FileH *)h)->fd); delete (FileH *)h; break;
        case H_EVENT: pthread_mutex_destroy(&((EventH *)h)->m); pthread_cond_destroy(&((EventH *)h)->c); delete (EventH *)h; break;
        case H_THREAD: pthread_detach(((ThreadH *)h)->th); /* leak the record: thread may still touch it */ break;
        default: break;
    }
    return TRUE;
}
DWORD GetFileAttributesA(LPCSTR name) {
    ensureInit();
    bool isDir; int64_t sz;
    if (!kvfs::statPath(normPath(name), &isDir, &sz)) return 0xFFFFFFFFu;
    return isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL SetFileAttributesA(LPCSTR, DWORD) { return TRUE; }
BOOL DeleteFileA(LPCSTR name) { ensureInit(); return kvfs::unlink(normPath(name)); }
BOOL RemoveDirectoryA(LPCSTR name) { ensureInit(); return kvfs::rmdir(normPath(name)); }
BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES) { ensureInit(); return kvfs::mkdir(normPath(name)); }
DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf) { const char *r = kroot(); strncpy(buf, r, n); if (n) buf[n - 1] = 0; return (DWORD)strlen(buf); }
DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD n) {
    // The engine derives its install path (where zone/, main/ and localization.txt live) from the exe path.
    std::string s = std::string(kroot()) + "/iw3mp.exe"; strncpy(buf, s.c_str(), n); buf[n ? n - 1 : 0] = 0; return (DWORD)strlen(buf);
}

// ---- FindFirstFile / _findfirst ----
static bool splitPattern(const char *spec, std::string &dir, std::string &pat) {
    std::string p = normPath(spec); size_t sl = p.rfind('/');
    if (sl == std::string::npos) { dir = "."; pat = p; } else { dir = p.substr(0, sl); pat = p.substr(sl + 1); if (dir.empty()) dir = "/"; }
    return true;
}
static bool fillNext(FindH *h, WIN32_FIND_DATAA *data) {
    while (h->next < h->entries.size()) {
        const kvfs::Entry &e = h->entries[h->next++];
        if (fnmatch(h->pattern.c_str(), e.name.c_str(), FNM_CASEFOLD) != 0) continue;
        memset(data, 0, sizeof(*data)); strncpy(data->cFileName, e.name.c_str(), MAX_PATH - 1);
        data->dwFileAttributes = e.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL; data->nFileSizeLow = (DWORD)e.size; data->nFileSizeHigh = (DWORD)(e.size >> 32);
        return true;
    }
    return false;
}
HANDLE FindFirstFileA(LPCSTR spec, LPWIN32_FIND_DATAA data) {
    ensureInit();
    FindH *h = new FindH; h->type = H_FIND; h->next = 0;
    if (traceFs()) fprintf(stderr, "[fs] FindFirstFile(%s)\n", spec);
    std::string dir; splitPattern(spec, dir, h->pattern);
    if (!kvfs::listdir(dir, &h->entries)) { delete h; SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    if (!fillNext(h, data)) { delete h; SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return h;
}
BOOL FindNextFileA(HANDLE hh, LPWIN32_FIND_DATAA data) {
    FindH *h = (FindH *)hh;
    if (!fillNext(h, data)) { SetLastError(18); return FALSE; }
    return TRUE;
}
BOOL FindClose(HANDLE hh) { FindH *h = (FindH *)hh; if (h && h != INVALID_HANDLE_VALUE) delete h; return TRUE; }
intptr_t _findfirst64i32(const char *spec, struct _finddata64i32_t *fi) {
    WIN32_FIND_DATAA d; HANDLE h = FindFirstFileA(spec, &d); if (h == INVALID_HANDLE_VALUE) return -1;
    memset(fi, 0, sizeof(*fi)); strncpy(fi->name, d.cFileName, sizeof(fi->name) - 1); fi->attrib = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _A_SUBDIR : 0; fi->size = d.nFileSizeLow; return (intptr_t)h;
}
int _findnext64i32(intptr_t h, struct _finddata64i32_t *fi) {
    WIN32_FIND_DATAA d; if (!FindNextFileA((HANDLE)h, &d)) return -1;
    memset(fi, 0, sizeof(*fi)); strncpy(fi->name, d.cFileName, sizeof(fi->name) - 1); fi->attrib = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _A_SUBDIR : 0; fi->size = d.nFileSizeLow; return 0;
}
int _findclose(intptr_t h) { FindClose((HANDLE)h); return 0; }

// ------------------------------------------------------------------ time
static uint64_t nowNs() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
DWORD GetTickCount(void) { return (DWORD)(nowNs() / 1000000ull); }
DWORD timeGetTime(void) { return GetTickCount(); }
BOOL QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = (LONGLONG)nowNs(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
void GetLocalTime(LPSYSTEMTIME t) {
    time_t now = time(nullptr); struct tm tmv; localtime_r(&now, &tmv);
    t->wYear = tmv.tm_year + 1900; t->wMonth = tmv.tm_mon + 1; t->wDayOfWeek = tmv.tm_wday; t->wDay = tmv.tm_mday; t->wHour = tmv.tm_hour; t->wMinute = tmv.tm_min; t->wSecond = tmv.tm_sec; t->wMilliseconds = 0;
}
void GetSystemTime(LPSYSTEMTIME t) { GetLocalTime(t); }
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft) {
    struct tm tmv = {}; tmv.tm_year = st->wYear - 1900; tmv.tm_mon = st->wMonth - 1; tmv.tm_mday = st->wDay; tmv.tm_hour = st->wHour; tmv.tm_min = st->wMinute; tmv.tm_sec = st->wSecond;
    uint64_t t = ((uint64_t)timegm(&tmv) + 11644473600ull) * 10000000ull; ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32); return TRUE;
}
LONG CompareFileTime(const FILETIME *a, const FILETIME *b) {
    uint64_t x = ((uint64_t)a->dwHighDateTime << 32) | a->dwLowDateTime, y = ((uint64_t)b->dwHighDateTime << 32) | b->dwLowDateTime; return x < y ? -1 : x > y ? 1 : 0;
}

// ------------------------------------------------------------------ sleep / APCs
DWORD SleepEx(DWORD ms, BOOL alertable) {
    if (alertable && t_apcCount > 0) {
        int n = t_apcCount; t_apcCount = 0;
        for (int i = 0; i < n; ++i) { PendingApc a = t_apc[i]; if (a.cb) a.cb(a.err, a.bytes, a.ov); }
        return WAIT_IO_COMPLETION;
    }
    if (ms == INFINITE) { for (;;) usleep(1000000); }
    if (ms == 0) { sched_yield(); return 0; }
    usleep((useconds_t)ms * 1000u); return 0;
}
void Sleep(DWORD ms) { SleepEx(ms, FALSE); }

// ------------------------------------------------------------------ critical sections (lazy recursive pthread mutex)
static pthread_mutex_t *csMutex(LPCRITICAL_SECTION cs) {
    void *m = __atomic_load_n(&cs->Reserved, __ATOMIC_ACQUIRE);
    if (!m) {
        pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_t *nm = new pthread_mutex_t; pthread_mutex_init(nm, &at);
        void *expected = nullptr;
        if (!__atomic_compare_exchange_n(&cs->Reserved, &expected, (void *)nm, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { pthread_mutex_destroy(nm); delete nm; m = expected; } else m = nm;
    }
    return (pthread_mutex_t *)m;
}
void InitializeCriticalSection(LPCRITICAL_SECTION cs) { cs->Reserved = nullptr; csMutex(cs); }
void DeleteCriticalSection(LPCRITICAL_SECTION cs) { if (cs->Reserved) { pthread_mutex_destroy((pthread_mutex_t *)cs->Reserved); delete (pthread_mutex_t *)cs->Reserved; cs->Reserved = nullptr; } }
void EnterCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_lock(csMutex(cs)); }
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) { return pthread_mutex_trylock(csMutex(cs)) == 0; }
void LeaveCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_unlock(csMutex(cs)); }

// ------------------------------------------------------------------ events / waits / threads
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCSTR) {
    EventH *e = new EventH; e->type = H_EVENT; pthread_mutex_init(&e->m, nullptr); pthread_cond_init(&e->c, nullptr); e->signaled = initial; e->manual = manual; return e;
}
BOOL SetEvent(HANDLE h) { EventH *e = (EventH *)h; if (traceFs()) fprintf(stderr, "[ev] Set %p\n", h); pthread_mutex_lock(&e->m); e->signaled = true; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m); return TRUE; }
BOOL ResetEvent(HANDLE h) { EventH *e = (EventH *)h; if (traceFs()) fprintf(stderr, "[ev] Reset %p\n", h); pthread_mutex_lock(&e->m); e->signaled = false; pthread_mutex_unlock(&e->m); return TRUE; }
static void abstime(DWORD ms, struct timespec *ts) { clock_gettime(CLOCK_REALTIME, ts); ts->tv_sec += ms / 1000; ts->tv_nsec += (long)(ms % 1000) * 1000000L; if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; } }
DWORD WaitForSingleObject(HANDLE hh, DWORD ms) {
    W32Handle *h = (W32Handle *)hh;
    if (!h || h == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    if (h->type == H_EVENT) {
        EventH *e = (EventH *)h; if (traceFs()) fprintf(stderr, "[ev] Wait %p ms=%lu signaled=%d\n", h, ms, (int)e->signaled); pthread_mutex_lock(&e->m); struct timespec ts; if (ms != INFINITE) abstime(ms, &ts);
        while (!e->signaled) { int r = (ms == INFINITE) ? pthread_cond_wait(&e->c, &e->m) : pthread_cond_timedwait(&e->c, &e->m, &ts); if (r == ETIMEDOUT) { pthread_mutex_unlock(&e->m); return WAIT_TIMEOUT; } }
        if (!e->manual) e->signaled = false; pthread_mutex_unlock(&e->m); return WAIT_OBJECT_0;
    }
    if (h->type == H_THREAD) {
        ThreadH *t = (ThreadH *)h; DWORD waited = 0;
        while (!t->done) { if (ms != INFINITE && waited >= ms) return WAIT_TIMEOUT; usleep(1000); ++waited; }
        return WAIT_OBJECT_0;
    }
    return WAIT_FAILED;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    DWORD waited = 0;
    for (;;) {
        for (DWORD i = 0; i < n; ++i) { if (WaitForSingleObject(hs[i], 0) == WAIT_OBJECT_0) { if (!all) return WAIT_OBJECT_0 + i; } }
        if (ms != INFINITE && waited >= ms) return WAIT_TIMEOUT; usleep(1000); ++waited;
    }
}
// Thread ids: the engine compares GetCurrentThreadId() against the id CreateThread() reported (Sys_IsDatabaseThread etc.),
// so both must come from the same counter. Threads we did not create (the main thread) get an id lazily.
static DWORD g_nextThreadId = 1;
static thread_local DWORD t_tid = 0;
static void *threadTramp(void *p) {
    ThreadH *t = (ThreadH *)p; t_tid = t->id;
    pthread_mutex_lock(&t->m); while (t->suspended) pthread_cond_wait(&t->c, &t->m); pthread_mutex_unlock(&t->m);
    t->exitCode = t->fn(t->arg); t->done = true; return nullptr;
}
HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD id) {
    ThreadH *t = new ThreadH; t->type = H_THREAD; t->done = false; t->exitCode = 0; t->fn = fn; t->arg = arg;
    t->id = __atomic_add_fetch(&g_nextThreadId, 1, __ATOMIC_SEQ_CST); t->suspended = (flags & CREATE_SUSPENDED) != 0;
    pthread_mutex_init(&t->m, nullptr); pthread_cond_init(&t->c, nullptr);
    if (id) *id = t->id; // publish before the thread can run: the new thread reads this value
    pthread_attr_t at; pthread_attr_init(&at); if (stack) pthread_attr_setstacksize(&at, stack < 1048576 ? 1048576 : stack); else pthread_attr_setstacksize(&at, 4u << 20);
    if (pthread_create(&t->th, &at, threadTramp, t) != 0) { delete t; return nullptr; }
    return t;
}
DWORD GetCurrentThreadId(void) { if (!t_tid) t_tid = __atomic_add_fetch(&g_nextThreadId, 1, __ATOMIC_SEQ_CST); return t_tid; }
HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
DWORD GetCurrentProcessId(void) { return 1; }
BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, HANDLE *dst, DWORD, BOOL, DWORD) { if (dst) *dst = src; return TRUE; }
BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
int GetThreadPriority(HANDLE) { return 0; }
DWORD SuspendThread(HANDLE) { return 0; } // only CREATE_SUSPENDED start is supported
DWORD ResumeThread(HANDLE h) {
    W32Handle *w = (W32Handle *)h; if (!w || w == INVALID_HANDLE_VALUE || w->type != H_THREAD) return 0;
    ThreadH *t = (ThreadH *)w; pthread_mutex_lock(&t->m); bool was = t->suspended; t->suspended = false; pthread_cond_broadcast(&t->c); pthread_mutex_unlock(&t->m); return was ? 1 : 0;
}
DWORD SetThreadAffinityMask(HANDLE, DWORD) { return 1; }
BOOL SetProcessAffinityMask(HANDLE, DWORD_PTR) { return TRUE; }
BOOL GetProcessAffinityMask(HANDLE, DWORD_PTR *p, DWORD_PTR *s) { if (p) *p = 1; if (s) *s = 1; return TRUE; }
BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
BOOL GetExitCodeThread(HANDLE h, LPDWORD code) { ThreadH *t = (ThreadH *)h; if (code) *code = t->done ? t->exitCode : STILL_ACTIVE; return TRUE; }
void RaiseException(DWORD, DWORD, DWORD, const ULONG_PTR *) {}

// TLS
DWORD TlsAlloc(void) { pthread_key_t k; pthread_key_create(&k, nullptr); return (DWORD)k; }
BOOL TlsSetValue(DWORD i, LPVOID v) { return pthread_setspecific((pthread_key_t)i, v) == 0; }
LPVOID TlsGetValue(DWORD i) { return pthread_getspecific((pthread_key_t)i); }

// ------------------------------------------------------------------ system info / misc
void GetSystemInfo(LPSYSTEM_INFO si) { memset(si, 0, sizeof(*si)); si->dwPageSize = 4096; si->dwNumberOfProcessors = 4; si->dwAllocationGranularity = 65536; }
void GlobalMemoryStatus(LPMEMORYSTATUS ms) { memset(ms, 0, sizeof(*ms)); ms->dwLength = sizeof(*ms); ms->dwTotalPhys = ms->dwAvailPhys = 2000u << 20; ms->dwTotalVirtual = ms->dwAvailVirtual = 2000u << 20; ms->dwTotalPageFile = ms->dwAvailPageFile = 2000u << 20; }
void OutputDebugStringA(LPCSTR s) { fputs(s, stderr); }
void ExitProcess(UINT code) { exit((int)code); }
void DebugBreak(void) { __builtin_trap(); }
BOOL IsDebuggerPresent(void) { return FALSE; }
BOOL GetVersionExA(OSVERSIONINFOA *v) { memset(v->szCSDVersion, 0, sizeof(v->szCSDVersion)); v->dwMajorVersion = 6; v->dwMinorVersion = 1; v->dwBuildNumber = 7601; v->dwPlatformId = 2; return TRUE; }
BOOL GetComputerNameA(LPSTR b, LPDWORD n) { strncpy(b, "WEBCLIENT", *n); return TRUE; }
BOOL GetUserNameA(LPSTR b, LPDWORD n) { strncpy(b, "player", *n); return TRUE; }
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD) { (void)type; if (addr) return addr; void *p = nullptr; if (posix_memalign(&p, 65536, size)) return nullptr; memset(p, 0, size); return p; }
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    if (type & MEM_RELEASE) { free(addr); return TRUE; }   // release the whole reservation
    if (type & 0x4000) { if (addr && size) memset(addr, 0, size); return TRUE; }   // MEM_DECOMMIT: keep the pages, hand back zeroes on recommit
    return TRUE;
}
int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT) { fprintf(stderr, "[MessageBox] %s: %s\n", caption ? caption : "", text ? text : ""); return IDOK; }
SHORT GetAsyncKeyState(int) { return 0; }
HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
HWND GetActiveWindow(void) { return nullptr; }
BOOL SetForegroundWindow(HWND) { return TRUE; }
HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR url, LPCSTR, LPCSTR, INT) { fprintf(stderr, "[ShellExecute] %s\n", url ? url : ""); return (HINSTANCE)(intptr_t)33; }
BOOL SetEnvironmentVariableA(LPCSTR n, LPCSTR v) { return setenv(n, v ? v : "", 1) == 0; }
BOOL SystemParametersInfoA(UINT, UINT, PVOID, UINT) { return FALSE; }
HGLOBAL GlobalAlloc(UINT, SIZE_T n) { return calloc(1, n); }
LPVOID GlobalLock(HGLOBAL h) { return h; }
BOOL GlobalUnlock(HGLOBAL) { return TRUE; }
HGLOBAL GlobalFree(HGLOBAL h) { free(h); return nullptr; }

} // extern "C"

extern "C" char *kisak_getcwd(char *buf, size_t size) {
    const char *r = kroot();
    if (!buf) { buf = (char *)malloc(strlen(r) + 1); size = strlen(r) + 1; }
    strncpy(buf, r, size); if (size) buf[size - 1] = 0; return buf;
}

// ------------------------------------------------------------------ CRT extras
extern "C" char *_itoa(int v, char *buf, int radix) {
    if (radix == 10) { sprintf(buf, "%d", v); return buf; }
    char tmp[40]; int i = 0; unsigned u = v < 0 && radix == 10 ? -v : (unsigned)v; if (u == 0) tmp[i++] = '0';
    while (u) { int d = u % radix; tmp[i++] = d < 10 ? '0' + d : 'a' + d - 10; u /= radix; }
    int j = 0; while (i) buf[j++] = tmp[--i]; buf[j] = 0; return buf;
}
extern "C" char *strlwr(char *s) { for (char *p = s; *p; ++p) *p = (char)tolower((unsigned char)*p); return s; }
extern "C" char *strupr(char *s) { for (char *p = s; *p; ++p) *p = (char)toupper((unsigned char)*p); return s; }

// ------------------------------------------------------------------ stdio / stat with Windows-style path handling
// FILE* streams over kvfs handles (works for the remote HTTP backend as well as in-memory files) via fopencookie.
struct KCookie { int h; };
static ssize_t kcRead(void *c, char *buf, size_t n) { int64_t r = kvfs::read(((KCookie *)c)->h, buf, n); return (ssize_t)r; }
static ssize_t kcWrite(void *c, const char *buf, size_t n) { int64_t r = kvfs::write(((KCookie *)c)->h, buf, n); return (ssize_t)r; }
static int kcSeek(void *c, off_t *off, int whence) { int64_t r = kvfs::lseek(((KCookie *)c)->h, *off, whence); if (r < 0) return -1; *off = (off_t)r; return 0; }
static int kcClose(void *c) { kvfs::close(((KCookie *)c)->h); delete (KCookie *)c; return 0; }

extern "C" FILE *kisak_fopen(const char *path, const char *mode) {
    ensureInit();
    std::string p = normPath(path), r = p;
    bool writing = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    int of = O_RDONLY;
    if (strchr(mode, 'w')) of = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strchr(mode, 'a')) of = O_WRONLY | O_CREAT | O_APPEND;
    if (strchr(mode, '+')) of = (of & ~O_ACCMODE) | O_RDWR;
    if (!writing) { std::string c; if (kvfs::statPath(p, nullptr, nullptr, &c)) r = c; }
    int h = kvfs::open(r, of);
    if (traceFs()) fprintf(stderr, "[fs] fopen(%s, %s) -> %s\n", path, mode, h >= 0 ? "ok" : "FAIL");
    if (h < 0) return nullptr;
    // MSVC's default (and "t") mode translates CRLF -> LF on read; the retail data (localization.txt, .cfg) is CRLF.
    // Emulate that for read-only text opens by translating into a memory stream (these files are small).
    if (!writing && !strchr(mode, 'b')) {
        int64_t n = 0; kvfs::fileSize(h, &n);
        std::vector<char> raw((size_t)(n > 0 ? n : 1));
        size_t got = 0;
        while ((int64_t)got < n) { int64_t k = kvfs::read(h, raw.data() + got, (size_t)(n - (int64_t)got)); if (k <= 0) break; got += (size_t)k; }
        kvfs::close(h);
        char *buf = (char *)malloc(got ? got : 1);
        size_t o = 0; for (size_t i = 0; i < got; ++i) { if (raw[i] == '\r' && i + 1 < got && raw[i + 1] == '\n') continue; buf[o++] = raw[i]; }
        FILE *m = fmemopen(buf, o ? o : 1, "r"); // buffer intentionally outlives the stream
        if (!o && m) fseek(m, 0, SEEK_END);
        return m;
    }
    KCookie *c = new KCookie{h};
    cookie_io_functions_t fn = { kcRead, kcWrite, kcSeek, kcClose };
    FILE *f = fopencookie(c, mode, fn);
    if (!f) { kvfs::close(h); delete c; }
    return f;
}
extern "C" int kisak_stat(const char *path, struct stat *st) {
    ensureInit();
    bool isDir; int64_t sz;
    if (!kvfs::statPath(normPath(path), &isDir, &sz)) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_mode = isDir ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_size = (off_t)sz;
    st->st_mtime = 1000000000;
    return 0;
}
