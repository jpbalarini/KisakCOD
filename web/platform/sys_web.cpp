// sys_web.cpp -- web replacement for src/win32/win_main.cpp (WinMain, window class, splash window, console loop).
// Keeps the data definitions and the Sys_* entry points the rest of the engine links against; everything that was
// "make a Win32 window / pump Win32 messages" is gone (the browser owns the window; input arrives as queued events).
#include <universal/q_shared.h>
#include "win32/win_configure.h"
#include "win32/win_local.h"
#include "win32/win_localize.h"
#include "win32/win_net.h"
#include "win32/win_net_debug.h"
#include <Windows.h>

#include <client/client.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <qcommon/mem_track.h>
#include <universal/com_memory.h>
#include <universal/q_parse.h>
#include <universal/timing.h>
#include <gfx_d3d/r_init.h>
#include <universal/profile.h>
#include <emscripten.h>

char sys_cmdline[1024];
char sys_exitCmdLine[1024];

HWND g_splashWnd;

sysEvent_t eventQue[0x100];
int eventHead;
int eventTail;

SysInfo sys_info;
int client_state;

cmd_function_s Sys_In_Restart_f_VAR;
cmd_function_s Sys_Net_Restart_f_VAR;
cmd_function_s Sys_Listen_f_VAR;

WinVars_t g_wv;

// ---------------------------------------------------------------- event queue (verbatim logic from win_main.cpp)
void __cdecl Sys_QueEvent(uint32_t time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr)
{
    sysEvent_t *ev;

    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    ev = &eventQue[(unsigned __int8)eventHead];
    if (eventHead - eventTail >= 256)
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "Sys_QueEvent: overflow\n");
        if (ev->evPtr)
            Z_Free((char *)ev->evPtr, 10);
        ++eventTail;
    }
    ++eventHead;
    if (!time)
        time = Sys_Milliseconds();
    ev->evTime = time;
    ev->evType = type;
    ev->evValue = value;
    ev->evValue2 = value2;
    ev->evPtrLength = ptrLength;
    ev->evPtr = ptr;
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

void Sys_ShutdownEvents()
{
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    while (eventHead > eventTail)
    {
        sysEvent_t *ev = &eventQue[(unsigned __int8)eventTail++];
        if (ev->evPtr)
            Z_Free((char *)ev->evPtr, 10);
    }
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
}

sysEvent_t *__cdecl Win_GetEvent(sysEvent_t *result)
{
    sysEvent_t ev;

    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    if (eventHead <= eventTail)
    {
        memset(&ev, 0, sizeof(ev));
        ev.evTime = Sys_Milliseconds();
    }
    else
    {
        ev = eventQue[(unsigned __int8)eventTail++];
    }
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    *result = ev;
    return result;
}

sysEvent_t *__cdecl Sys_GetEvent(sysEvent_t *result)
{
    return Win_GetEvent(result);
}

void __cdecl Sys_LoadingKeepAlive()
{
    // The browser main loop yields between frames; nothing to pump while loading.
}

// ---------------------------------------------------------------- lifecycle / errors
void __cdecl Sys_NormalExit() {}
void __cdecl Sys_QuitAndStartProcess(const char *, const char *) { Cbuf_AddText(0, "quit\n"); }

void __cdecl Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    fprintf(stderr, "Out of memory: filename '%s', line %d\n", filename, line);
    exit(-1);
}

void Sys_NoFreeFilesError()
{
    fprintf(stderr, "Out of free file handles / disk full\n");
    exit(-1);
}

int __cdecl Sys_CheckCrashOrRerun() { return 1; }

void Sys_Error(const char *error, ...)
{
    char string[4100];
    va_list va;

    va_start(va, error);
    vsnprintf(string, 0x1000u, error, va);
    va_end(va);
    fprintf(stderr, "\n*** Sys_Error: %s\n", string);
    fflush(stderr);
    emscripten_log(EM_LOG_CONSOLE | EM_LOG_C_STACK | EM_LOG_NO_PATHS, "Sys_Error call stack:");
    exit(1);
}

void __cdecl Sys_OpenURL(const char *url, int doexit)
{
    fprintf(stderr, "Sys_OpenURL(%s)\n", url);
    if (doexit)
        Cbuf_AddText(0, "quit\n");
}

// Set by Sys_Quit: the browser frame loop (app/main.cpp) must not run another frame. By then Com_Quit_f has torn the engine down (UI, menus,
// filesystem), and the loop is a browser timer that exit() does not stop: it kept calling Com_Frame, which printed "Could not find menu 'main'"
// forever.
bool g_webQuitting = false;

void __cdecl Sys_Quit()
{
    fflush(stdout);
    g_webQuitting = true;
    emscripten_cancel_main_loop();
    // Back to the start screen: the page reloads itself (a fresh page also frees the engine's memory and workers). No such function under node.
    MAIN_THREAD_ASYNC_EM_ASM({ if (typeof kisakGameExited === 'function') kisakGameExited(); });
    exit(0);
}

void __cdecl Sys_Print(const char *msg)
{
    fputs(msg, stdout);
}

char *__cdecl Sys_GetClipboardData() { return 0; }
int __cdecl Sys_SetClipboardData(const char *) { return 0; }

// ---------------------------------------------------------------- init / system info
void Sys_In_Restart_f()
{
    IN_Shutdown();
    IN_Init();
}

void Sys_Net_Restart_f() { NET_Restart(); }

void __cdecl Sys_Init()
{
    Cmd_AddCommandInternal("in_restart", Sys_In_Restart_f, &Sys_In_Restart_f_VAR);
    Cmd_AddCommandInternal("net_restart", Sys_Net_Restart_f, &Sys_Net_Restart_f_VAR);
    Cmd_AddCommandInternal("net_listen", Sys_Listen_f, &Sys_Listen_f_VAR);
    Com_Printf(CON_CHANNEL_SYSTEM, "CPU name is \"%s\"\n", sys_info.cpuName);
    Com_Printf(CON_CHANNEL_SYSTEM, "%i logical CPU(s) reported\n", sys_info.logicalCpuCount);
    Com_Printf(CON_CHANNEL_SYSTEM, "System memory is %i MB (capped at 1 GB)\n", sys_info.sysMB);
    IN_Init();
}

void __cdecl Sys_CreateSplashWindow() {}
void __cdecl Sys_ShowSplashWindow() {}

int __cdecl Sys_SystemMemoryMB() { return 1024; }

void Sys_FindInfo()
{
    sys_info.logicalCpuCount = 1;   // single-threaded renderer: the GL context lives on the engine main thread
    sys_info.physicalCpuCount = 4;
    sys_info.cpuGHz = 3.0;
    sys_info.sysMB = Sys_SystemMemoryMB();
    strcpy(sys_info.gpuDescription, "WebGL2");
    sys_info.SSE = 0;   // wasm has no SSE/MMX skinning kernels: use the portable C skinning path
    strcpy(sys_info.cpuVendor, "WebAssembly");
    strcpy(sys_info.cpuName, "wasm32 simd128");
    Sys_SetAutoConfigureGHz(&sys_info);
}

// ---------------------------------------------------------------- dvar globals that lived in the (excluded) win_input.cpp
#include <gfx_d3d/r_dvars.h>
const dvar_t *vid_xpos;
const dvar_t *vid_ypos;
const dvar_t *r_fullscreen;
