// web_prof.cpp -- implementation of the zone profiler (see shim/web_prof.h).
#include <web_prof.h>
#include <emscripten.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <vector>

namespace webprof {

bool g_on = false;

namespace {
struct Entry {
    const char *name = nullptr;
    // current frame
    double fIncl = 0, fExcl = 0; uint32_t fCalls = 0;
    // current report window
    double wIncl = 0, wExcl = 0, wMaxExcl = 0; uint64_t wCalls = 0;
};
constexpr int kSlots = 2048;           // power of two
Entry g_zones[kSlots];
Entry g_counters[256];                 // frame sum in fExcl, window sum in wExcl, window max of the frame sum in wMaxExcl
double g_stackChild[128];
int g_depth = 0;
pthread_t g_mainThread;
double g_hitchMs = 60.0;
double g_winStart = 0, g_winFrames = 0, g_winFrameMs = 0, g_winMaxFrameMs = 0, g_winIntervalMs = 0, g_winMaxIntervalMs = 0;
int g_slow = 0;                        // frames over the hitch threshold in this window

inline int SlotFor(const char *name, Entry *tab, int n)
{
    uintptr_t h = (uintptr_t)name; h ^= h >> 11; h *= 2654435761u;
    int i = (int)(h & (n - 1));
    for (int probe = 0; probe < n; ++probe, i = (i + 1) & (n - 1)) {
        if (!tab[i].name) { tab[i].name = name; return i; }
        if (tab[i].name == name) return i;
    }
    return -1;
}
inline double Now() { return emscripten_get_now(); }

struct Row { const char *name; double excl, incl, maxExcl; double calls; };
}

static void (*g_extra)() = nullptr;
void SetExtraReport(void (*fn)()) { g_extra = fn; }

void Init(double hitchMs) { g_on = true; g_mainThread = pthread_self(); g_hitchMs = hitchMs; g_winStart = Now(); }

void Zone::begin()
{
    if (!pthread_equal(pthread_self(), g_mainThread) || g_depth >= 127) return;
    slot = SlotFor(name, g_zones, kSlots);
    if (slot < 0) return;
    live = true;
    g_stackChild[++g_depth] = 0;
    t0 = Now();
}

void Zone::end()
{
    double dur = Now() - t0;
    Entry &e = g_zones[slot];
    e.fIncl += dur; e.fExcl += dur - g_stackChild[g_depth]; ++e.fCalls;
    --g_depth;
    g_stackChild[g_depth] += dur;
}

void Count(const char *name, double v)
{
    if (!pthread_equal(pthread_self(), g_mainThread)) return;
    int s = SlotFor(name, g_counters, 256);
    if (s >= 0) g_counters[s].fExcl += v;
}

// merge entries that have the same text (the same literal in different translation units has different addresses)
static void Collect(std::vector<Row> &rows, bool frame)
{
    for (int i = 0; i < kSlots; ++i) {
        Entry &e = g_zones[i];
        if (!e.name) continue;
        double excl = frame ? e.fExcl : e.wExcl, incl = frame ? e.fIncl : e.wIncl, mx = frame ? e.fExcl : e.wMaxExcl;
        double calls = frame ? e.fCalls : (double)e.wCalls;
        if (calls == 0) continue;
        bool merged = false;
        for (Row &r : rows) if (!strcmp(r.name, e.name)) { r.excl += excl; r.incl += incl; r.calls += calls; r.maxExcl = std::max(r.maxExcl, mx); merged = true; break; }
        if (!merged) rows.push_back({e.name, excl, incl, mx, calls});
    }
}

void FrameEnd(double frameMs, double intervalMs)
{
    g_depth = 0;
    if (frameMs > g_hitchMs) {
        ++g_slow;
        std::vector<Row> rows; Collect(rows, true);
        std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.excl > b.excl; });
        fprintf(stderr, "[prof] HITCH %.1f ms (interval %.1f):", frameMs, intervalMs);
        for (size_t i = 0; i < rows.size() && i < 7; ++i) fprintf(stderr, " %s=%.1f(x%.0f)", rows[i].name, rows[i].excl, rows[i].calls);
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < kSlots; ++i) {
        Entry &e = g_zones[i];
        if (!e.name) continue;
        e.wIncl += e.fIncl; e.wExcl += e.fExcl; e.wCalls += e.fCalls;
        e.wMaxExcl = std::max(e.wMaxExcl, e.fExcl);
        e.fIncl = e.fExcl = 0; e.fCalls = 0;
    }
    for (int i = 0; i < 256; ++i) {
        Entry &c = g_counters[i];
        if (!c.name) continue;
        c.wExcl += c.fExcl; c.wMaxExcl = std::max(c.wMaxExcl, c.fExcl); c.fExcl = 0;
    }
    g_winFrames += 1; g_winFrameMs += frameMs; g_winIntervalMs += intervalMs;
    g_winMaxFrameMs = std::max(g_winMaxFrameMs, frameMs); g_winMaxIntervalMs = std::max(g_winMaxIntervalMs, intervalMs);

    double now = Now();
    if (now - g_winStart < 4000.0) return;
    double n = g_winFrames > 0 ? g_winFrames : 1;
    fprintf(stderr, "[prof] ---- %.0f frames in %.1f s: %.1f fps | Com_Frame avg %.1f ms (max %.1f) | frame interval avg %.1f ms (max %.1f) | %d hitch frames\n",
            g_winFrames, (now - g_winStart) / 1000.0, g_winFrames * 1000.0 / (now - g_winStart), g_winFrameMs / n, g_winMaxFrameMs,
            g_winIntervalMs / n, g_winMaxIntervalMs, g_slow);
    std::vector<Row> rows; Collect(rows, false);
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.excl > b.excl; });
    fprintf(stderr, "[prof]   %-34s %8s %8s %8s %8s\n", "zone (by self time)", "self/fr", "incl/fr", "calls/fr", "max self");
    for (size_t i = 0; i < rows.size() && i < 32; ++i)
        fprintf(stderr, "[prof]   %-34s %8.2f %8.2f %8.1f %8.1f\n", rows[i].name, rows[i].excl / n, rows[i].incl / n, rows[i].calls / n, rows[i].maxExcl);
    std::vector<Row> cs;
    for (int i = 0; i < 256; ++i) if (g_counters[i].name) cs.push_back({g_counters[i].name, g_counters[i].wExcl, 0, g_counters[i].wMaxExcl, 0});
    std::sort(cs.begin(), cs.end(), [](const Row &a, const Row &b) { return strcmp(a.name, b.name) < 0; });
    fprintf(stderr, "[prof]   counters (avg/frame, max):");
    for (Row &c : cs) fprintf(stderr, " %s=%.0f(%.0f)", c.name, c.excl / n, c.maxExcl);
    fprintf(stderr, "\n");
    if (g_extra) g_extra();
    // reset the window
    for (int i = 0; i < kSlots; ++i) { Entry &e = g_zones[i]; e.wIncl = e.wExcl = e.wMaxExcl = 0; e.wCalls = 0; }
    for (int i = 0; i < 256; ++i) { g_counters[i].wExcl = 0; g_counters[i].wMaxExcl = 0; }
    g_winStart = now; g_winFrames = g_winFrameMs = g_winMaxFrameMs = g_winIntervalMs = g_winMaxIntervalMs = 0; g_slow = 0;
}

}
