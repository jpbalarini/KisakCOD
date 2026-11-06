// web_prof.h -- tiny scoped-zone profiler for the web build.
//
// The engine already marks ~300 interesting spans with PROF_SCOPED("name") (Tracy zones in the upstream project; they compile to nothing
// without Tracy). In the web build they are routed here: when the page is opened with `?args=--prof` every zone entered on the engine's main
// thread is timed (inclusive + exclusive "self" time), and web/app/main.cpp prints a table of the heaviest zones, per-frame counters and a
// dump of every hitch frame to the page log. When profiling is off a zone costs one load + branch.
//
// Zone names must be string literals (they are keyed by pointer, merged by text when reported).
#pragma once

#ifdef KISAK_WEB

namespace webprof {

extern bool g_on;      // set by `--prof`
void Init(double hitchMs);

struct Zone {
    const char *name; double t0; int slot; bool live;
    inline Zone(const char *n) : name(n), live(false) { if (g_on) begin(); }
    inline ~Zone() { if (live) end(); }
    void begin();
    void end();
};

// Frame counters (draw calls, uploads...): summed per frame, reported as per-frame average / max.
void Count(const char *name, double v);

// Extra lines appended to every report (the GL layer prints its per-buffer upload table with this). The callback runs at the end of a report window.
void SetExtraReport(void (*fn)());

// Called by the frame loop around Com_Frame. `frameMs` = time of the whole frame, intervalMs = time since the previous frame started.
void FrameEnd(double frameMs, double intervalMs);

}

#define WEBPROF_CAT2(a, b) a##b
#define WEBPROF_CAT(a, b) WEBPROF_CAT2(a, b)
#define PROF_SCOPED(name) ::webprof::Zone WEBPROF_CAT(webprofZone_, __LINE__)(name)
#define WEBPROF_COUNT(name, v) do { if (::webprof::g_on) ::webprof::Count(name, (double)(v)); } while (0)

#else
#define WEBPROF_COUNT(name, v) do {} while (0)
#endif
