// Browser entry point: the real engine (Com_Init / Com_Frame) running in the browser. The engine's frame loop is driven by the
// browser (one Com_Frame per animation frame, so WebGL frames are presented between calls).
#include <universal/q_shared.h>
#include <Windows.h>
#include <client/client.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <qcommon/mem_track.h>
#include <universal/com_memory.h>
#include <universal/com_files.h>
#include <universal/q_parse.h>
#include <universal/timing.h>
#include <universal/profile.h>
#include <win32/win_local.h>
#include <win32/win_steam.h>
#include <win32/win_storage.h>
#include <win32/win_localize.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <string>
#include <emscripten.h>
#include <emscripten/html5.h>
#include "../shim/kvfs.h"

extern char sys_cmdline[1024];
void Sys_FindInfo();

static int g_frames = 0;

// Files the engine writes live in memory (kvfs); the ones under players/ (the profiles) are also saved in the browser and restored at start-up.
// On a first run there is no profile yet: seed one (players/profiles/active.txt + its directory) so the game starts straight into the main menu
// instead of the "create a profile" screen. (These writes are saved like any other.)
static void SeedDefaultProfile()
{
    const char *name = "Player";
    kvfs::mkdir("/cod4/players");
    kvfs::mkdir("/cod4/players/profiles");
    kvfs::mkdir(std::string("/cod4/players/profiles/") + name);
    int h = kvfs::open("/cod4/players/profiles/active.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (h >= 0) { kvfs::write(h, name, strlen(name)); kvfs::close(h); }
}

static double g_lastReport = 0;
static int g_framesAtReport = 0;

static double g_lastFrameMs = 0;

extern bool g_webQuitting;   // platform/sys_web.cpp: the engine has quit (Quit in the menu / the quit command)

static void FrameCb(void *)
{
    if (g_webQuitting) return;
    Com_Frame();
    ++g_frames;
    double now = emscripten_get_now();
    g_lastFrameMs = now;
    if (now - g_lastReport > 5000.0) {
        if (g_lastReport > 0)
            fprintf(stderr, "[web] %.1f fps (%d frames)\n", (g_frames - g_framesAtReport) * 1000.0 / (now - g_lastReport), g_frames);
        g_lastReport = now;
        g_framesAtReport = g_frames;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setenv("KISAK_INSTALL_PATH", "/cod4", 0);
    kvfs::init("/cod4");

    (void)0;
    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    track_init();
    Win_InitLocalization();
    Com_InitParse();
    Dvar_Init();
    InitTiming();
    Sys_FindInfo();
    // Defaults for the web build (a page's ?args=... come after and can override them):
    //  - sv_pure 0: web servers do not verify clients' .iwd checksums, so they advertise "mod 1" and no "pure" key in their infoResponse;
    //  - ui_netSource 0 (Local): there is no master server, only the relay's virtual LAN;
    //  - ui_browserShowPure 0 / ui_browserMod -1: the server browser's stock filters ("pure servers only", "no mods") would hide every
    //    web server, i.e. the in-game LAN browser would always be empty.
    //  - sv_connectTimeout / cl_connectTimeout: a client that joins a game loads the map's fastfiles before it answers the server, and in the
    //    browser that read goes over the network (a cold cache on another machine takes minutes), during which the engine thread sends no
    //    packets. The stock 45 s (server) / 200 s (client) limits would drop the joining player with "Server disconnected" (EXE_TIMEDOUT).
    //    These are the dvars' maximum values (1800 s / 3600 s).
    char cmd[1024] = "+set r_fullscreen 0 +set developer 1 +set sv_pure 0 +set com_introPlayed 1"
                     " +set ui_netSource 0 +set ui_browserShowPure 0 +set ui_browserMod -1"
                     " +set sv_connectTimeout 1800 +set cl_connectTimeout 3600";
    bool autoProfile = true;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-profile")) { autoProfile = false; continue; }
        if (!strcmp(argv[i], "--trace-fs")) { setenv("KISAK_TRACE_FS", "1", 1); continue; }
        if (!strcmp(argv[i], "--nosound")) { setenv("KISAK_NOAUDIO", "1", 1); continue; }
        if (!strcmp(argv[i], "--gldebug")) { setenv("KISAK_GLDEBUG", "1", 1); continue; }
        if (!strcmp(argv[i], "--fsstats")) { setenv("KISAK_FSSTATS", "1", 1); continue; }
        if (!strcmp(argv[i], "--fstrace")) { setenv("KISAK_FSSTATS", "2", 1); continue; }   // log game-data request statistics every 5 s
        strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
    }
    I_strncpyz(sys_cmdline, cmd, 1024);
    Sys_Milliseconds();
    Profile_Init();
    Profile_InitContext(0);
    // A profile saved by an earlier session was restored by kvfs::init (active.txt names the profile in use); only a first run creates "Player".
    bool savedProfile = false;
    { bool isDir = false; int64_t size = 0; savedProfile = kvfs::statPath("/cod4/players/profiles/active.txt", &isDir, &size) && !isDir && size > 0; }
    if (autoProfile && !savedProfile) SeedDefaultProfile();
    Steam_Init();   // browser identity (platform/steam_web.cpp): the connect handshake sends a "Steam" ticket
    fprintf(stderr, "[web] Com_Init...\n");
    Com_Init(sys_cmdline);
    // The profile is seeded, not selected through the menus, so the engine never reads/initialises its stats (LiveStorage_ReadStats runs after
    // a profile switch). All-zero stats make the game scripts kick any remote player ("check for stat integrity": getstat(205+10*i) == 0).
    // No mpdata file exists, so this takes the "no stats found" path: default class slots, unlocks, stat version.
    if (autoProfile) LiveStorage_ReadStats();
    fprintf(stderr, "[web] Com_Init done, entering frame loop\n");
    emscripten_set_main_loop_arg(FrameCb, nullptr, 0, 0);
    // requestAnimationFrame stops in a hidden tab, which would freeze a game server hosted there (its players time out). Worker timers are not
    // throttled by tab visibility, so a watchdog runs a frame (~20 Hz) whenever the rAF loop has not produced one recently.
    emscripten_set_interval([](void *) { if (emscripten_get_now() - g_lastFrameMs > 100.0) FrameCb(nullptr); }, 50.0, nullptr);
    return 0;
}
