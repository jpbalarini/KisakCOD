// window_web.cpp -- Win32 window / monitor APIs for the browser: there is exactly one "window" (the canvas).
#include <Windows.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <string.h>

static int g_fakeWindow = 0;

extern "C" {

HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)
{
    g_fakeWindow = 1;
    return (HWND)(intptr_t)0x1001;
}
BOOL DestroyWindow(HWND) { g_fakeWindow = 0; return TRUE; }
BOOL IsWindow(HWND h) { return h != nullptr && g_fakeWindow; }
BOOL ShowWindow(HWND, int) { return TRUE; }
BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }
HMONITOR MonitorFromPoint(POINT, DWORD) { return (HMONITOR)(intptr_t)1; }
BOOL GetMonitorInfoA(HMONITOR, LPMONITORINFO mi)
{
    mi->rcMonitor = RECT{0, 0, 1920, 1080};
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = 1;
    return TRUE;
}
BOOL EnumDisplayMonitors(HDC, const RECT *, MONITORENUMPROC cb, LPARAM data)
{
    RECT r = {0, 0, 1920, 1080};
    cb((HMONITOR)(intptr_t)1, nullptr, &r, data);
    return TRUE;
}
int GetSystemMetrics(int idx)
{
    switch (idx) {
    case 0: return 1920;   // SM_CXSCREEN
    case 1: return 1080;   // SM_CYSCREEN
    default: return 0;
    }
}

} // extern "C"
