// M1 harness: the real engine (Com_Init / Com_Frame) running in the browser. The engine's frame loop is driven by the
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

// Files the engine writes live in memory (kvfs), so a player profile would have to be created in the UI on every page load.
// Pre-seed one (players/profiles/active.txt + its directory) so the game starts straight into the main menu.
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

static void FrameCb(void *)
{
    Com_Frame();
    ++g_frames;
    double now = emscripten_get_now();
    if (now - g_lastReport > 5000.0) {
        if (g_lastReport > 0)
            fprintf(stderr, "[m1] %.1f fps (%d frames)\n", (g_frames - g_framesAtReport) * 1000.0 / (now - g_lastReport), g_frames);
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
    char cmd[1024] = "+set r_fullscreen 0 +set developer 1 +set sv_pure 0 +set com_introPlayed 1";
    bool autoProfile = true;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-profile")) { autoProfile = false; continue; }
        if (!strcmp(argv[i], "--trace-fs")) { setenv("KISAK_TRACE_FS", "1", 1); continue; }
        if (!strcmp(argv[i], "--nosound")) { setenv("KISAK_NOAUDIO", "1", 1); continue; }
        if (!strcmp(argv[i], "--gldebug")) { setenv("KISAK_GLDEBUG", "1", 1); continue; }
        strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
    }
    I_strncpyz(sys_cmdline, cmd, 1024);
    Sys_Milliseconds();
    Profile_Init();
    Profile_InitContext(0);
    if (autoProfile) SeedDefaultProfile();
    fprintf(stderr, "[m1] Com_Init...\n");
    Com_Init(sys_cmdline);
    fprintf(stderr, "[m1] Com_Init done, entering frame loop\n");
    emscripten_set_main_loop_arg(FrameCb, nullptr, 0, 0);
    return 0;
}
