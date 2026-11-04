// Windows.h -- minimal Win32 API shim for the emscripten (wasm32) build of KisakCOD.
// Only what the engine actually touches. Implementations live in web/shim/win32_shim.cpp;
// anything declared here but not implemented simply fails to link / traps if ever called.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- basic types (LLP32-equivalent: wasm32 `long` is 32-bit) ----
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef short SHORT;
typedef unsigned short USHORT;
typedef int INT;
typedef unsigned int UINT;
typedef float FLOAT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef void VOID;
typedef void *PVOID, *LPVOID;
typedef const void *LPCVOID;
typedef BYTE *PBYTE, *LPBYTE;
typedef WORD *LPWORD;
typedef DWORD *PDWORD, *LPDWORD;
typedef LONG *PLONG, *LPLONG;
typedef BOOL *LPBOOL;
typedef CHAR *LPSTR, *PSTR;
typedef const CHAR *LPCSTR, *PCSTR;
typedef WCHAR *LPWSTR, *PWSTR;
typedef const WCHAR *LPCWSTR, *PCWSTR;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR, SIZE_T;
typedef intptr_t INT_PTR, LONG_PTR, SSIZE_T;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM, LRESULT;
typedef long HRESULT;
typedef void *HANDLE, *HGDIOBJ, *HLOCAL, *HGLOBAL;
// Typed handles use the MS "struct X__ *" form (mss.h and the decompiled code name these tags directly)
#define DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__ *name
DECLARE_HANDLE(HWND); DECLARE_HANDLE(HINSTANCE); DECLARE_HANDLE(HDC); DECLARE_HANDLE(HGLRC); DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HCURSOR_); DECLARE_HANDLE(HBRUSH); DECLARE_HANDLE(HMENU); DECLARE_HANDLE(HBITMAP); DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HKEY); DECLARE_HANDLE(HMONITOR); DECLARE_HANDLE(HHOOK); DECLARE_HANDLE(HRSRC); DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HRGN);
typedef HICON HCURSOR;
typedef HINSTANCE HMODULE;
typedef HANDLE *PHANDLE;
typedef WORD ATOM;
typedef DWORD COLORREF;
typedef LONG_PTR(__stdcall *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef int(__stdcall *FARPROC)();
typedef void(__stdcall *LPTHREAD_START_ROUTINE_T)(void *);
typedef DWORD(__stdcall *LPTHREAD_START_ROUTINE)(LPVOID);

#define WINAPI __stdcall
#define CALLBACK __stdcall
#define APIENTRY __stdcall
#define WINAPIV __cdecl
#define CONST const
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FALSE 0
#define TRUE 1
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define UNREFERENCED_PARAMETER(P) (void)(P)
#define ZeroMemory(p, n) memset((p), 0, (n))
#define CopyMemory(d, s, n) memcpy((d), (s), (n))
#define MoveMemory(d, s, n) memmove((d), (s), (n))
#define FillMemory(d, n, f) memset((d), (f), (n))

// ---- HRESULT ----
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_POINTER ((HRESULT)0x80004003L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)

// ---- errors / constants ----
#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_HANDLE_EOF 38L
#define ERROR_IO_PENDING 997L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258L
#define WAIT_FAILED 0xFFFFFFFFu
#define WAIT_IO_COMPLETION 192L
#define STILL_ACTIVE 259
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define CREATE_SUSPENDED 0x4
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
#define PAGE_NOACCESS 0x01

// ---- structs ----
typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;
typedef struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT, POINTL;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct _SIZE { LONG cx, cy; } SIZE;
typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct { DWORD Offset; DWORD OffsetHigh; };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef void(__stdcall *LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);

// Critical section: opaque storage, implemented over a pthread-free spinlock/recursive counter in win32_shim.cpp
typedef struct _RTL_CRITICAL_SECTION {
    volatile long LockCount;
    volatile long RecursionCount;
    volatile unsigned long OwningThread;
    void *Reserved;
} CRITICAL_SECTION, *LPCRITICAL_SECTION, RTL_CRITICAL_SECTION;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef WIN32_FIND_DATAA WIN32_FIND_DATA;

typedef struct _MEMORYSTATUS {
    DWORD dwLength, dwMemoryLoad;
    SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;
typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *LPMSG;
typedef struct _SYSTEM_INFO { DWORD dwOemId; DWORD dwPageSize; LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress; DWORD_PTR dwActiveProcessorMask; DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity; WORD wProcessorLevel, wProcessorRevision; } SYSTEM_INFO, *LPSYSTEM_INFO;
typedef struct _OSVERSIONINFOA { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; CHAR szCSDVersion[128]; } OSVERSIONINFOA, OSVERSIONINFO;

// ---- file / io ----
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE f, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov);
BOOL ReadFileEx(HANDLE f, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE cb);
BOOL WriteFile(HANDLE f, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov);
BOOL CloseHandle(HANDLE h);
DWORD GetFileSize(HANDLE f, LPDWORD high);
DWORD SetFilePointer(HANDLE f, LONG dist, PLONG distHigh, DWORD method);
BOOL FlushFileBuffers(HANDLE f);
DWORD GetFileAttributesA(LPCSTR name);
BOOL DeleteFileA(LPCSTR name);
BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa);
HANDLE FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data);
BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data);
BOOL FindClose(HANDLE h);
DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf);
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n);

// ---- sync / threads ----
DWORD GetLastError(void);
void SetLastError(DWORD e);
void Sleep(DWORD ms);
DWORD SleepEx(DWORD ms, BOOL alertable);
DWORD GetTickCount(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER *c);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);
DWORD timeGetTime(void);
void GetLocalTime(LPSYSTEMTIME t);
void GetSystemTime(LPSYSTEMTIME t);
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD id);
DWORD GetCurrentThreadId(void);
HANDLE GetCurrentThread(void);
DWORD GetCurrentProcessId(void);
BOOL SetThreadPriority(HANDLE h, int p);
DWORD SuspendThread(HANDLE h);
DWORD ResumeThread(HANDLE h);
DWORD SetThreadAffinityMask(HANDLE h, DWORD mask);
BOOL GetExitCodeThread(HANDLE h, LPDWORD code);
DWORD TlsAlloc(void);
BOOL TlsSetValue(DWORD idx, LPVOID v);
LPVOID TlsGetValue(DWORD idx);
void GetSystemInfo(LPSYSTEM_INFO si);
void GlobalMemoryStatus(LPMEMORYSTATUS ms);
void OutputDebugStringA(LPCSTR s);
void ExitProcess(UINT code);
void DebugBreak(void);
BOOL IsDebuggerPresent(void);
BOOL GetVersionExA(OSVERSIONINFOA *vi);
BOOL GetComputerNameA(LPSTR buf, LPDWORD n);
BOOL GetUserNameA(LPSTR buf, LPDWORD n);
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);

// ---- user32 / gdi32 / misc (declarations only; the web build replaces the window/input layer) ----
typedef struct tagMONITORINFO { DWORD cbSize; RECT rcMonitor; RECT rcWork; DWORD dwFlags; } MONITORINFO, *LPMONITORINFO;
typedef BOOL(__stdcall *MONITORENUMPROC)(HMONITOR, HDC, LPRECT, LPARAM);
typedef struct tagWNDCLASSA { UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; } WNDCLASSA, WNDCLASS;
typedef struct _devicemodeA { BYTE dmDeviceName[32]; WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; DWORD dmFields; BYTE pad_[96]; DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency; } DEVMODEA, DEVMODE;
#define WS_POPUP 0x80000000L
#define WS_VISIBLE 0x10000000L
#define WS_CAPTION 0x00C00000L
#define WS_SYSMENU 0x00080000L
#define WS_EX_TOPMOST 0x00000008L
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define EXCEPTION_EXECUTE_HANDLER 1
#define _TRUNCATE ((size_t)-1)
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM);
HWND GetActiveWindow(void);
HWND GetDesktopWindow(void);
BOOL DestroyWindow(HWND);
BOOL ShowWindow(HWND, int);
int ShowCursor(BOOL);
HWND SetFocus(HWND);
BOOL SetForegroundWindow(HWND);
HMODULE GetModuleHandleA(LPCSTR);
HANDLE GetCurrentProcess(void);
LONG GetWindowLongA(HWND, int);
LONG SetWindowLongA(HWND, int, LONG);
HDC GetDC(HWND);
int ReleaseDC(HWND, HDC);
int GetDeviceCaps(HDC, int);
HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
BOOL SetEnvironmentVariableA(LPCSTR, LPCSTR);
int GetSystemMetrics(int);
HMONITOR MonitorFromWindow(HWND, DWORD);
HMONITOR MonitorFromPoint(POINT, DWORD);
BOOL GetMonitorInfoA(HMONITOR, LPMONITORINFO);
int GetWindowTextA(HWND, LPSTR, int);
BOOL SetWindowTextA(HWND, LPCSTR);
BOOL GetProcessAffinityMask(HANDLE, DWORD_PTR *, DWORD_PTR *);

BOOL DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
void RaiseException(DWORD, DWORD, DWORD, const ULONG_PTR *);
BOOL SetFileAttributesA(LPCSTR, DWORD);
BOOL OpenClipboard(HWND); BOOL EmptyClipboard(void); BOOL CloseClipboard(void);
HANDLE SetClipboardData(UINT, HANDLE); HANDLE GetClipboardData(UINT);
HGLOBAL GlobalAlloc(UINT, SIZE_T); LPVOID GlobalLock(HGLOBAL); BOOL GlobalUnlock(HGLOBAL); HGLOBAL GlobalFree(HGLOBAL);
#define GMEM_MOVEABLE 0x2
#define GMEM_DDESHARE 0x2000
#define CF_TEXT 1
BOOL GetWindowRect(HWND, LPRECT); BOOL ClientToScreen(HWND, LPPOINT); BOOL ClipCursor(const RECT *);
BOOL GetCursorPos(LPPOINT); BOOL SetCursorPos(int, int);
BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT);
BOOL AdjustWindowRect(LPRECT, DWORD, BOOL); BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD);
BOOL IsWindow(HWND);
BOOL EnumDisplayMonitors(HDC, const RECT *, MONITORENUMPROC, LPARAM);
LONG ChangeDisplaySettingsA(DEVMODEA *, DWORD);
BOOL SetDeviceGammaRamp(HDC, LPVOID);
BOOL EnumThreadWindows(DWORD, BOOL(__stdcall *)(HWND, LPARAM), LPARAM);
BOOL SetProcessAffinityMask(HANDLE, DWORD_PTR);
BOOL SetPriorityClass(HANDLE, DWORD);
int GetThreadPriority(HANDLE);
HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
HWND SetCapture(HWND); BOOL ReleaseCapture(void);
BOOL RemoveDirectoryA(LPCSTR);
BOOL SystemTimeToFileTime(const SYSTEMTIME *, LPFILETIME);
LONG CompareFileTime(const FILETIME *, const FILETIME *);
BOOL SystemParametersInfoA(UINT, UINT, PVOID, UINT);
// ---- misc user32/gdi32/winmm-ish (declared; implemented only if needed) ----
int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type);
#define MessageBox MessageBoxA
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_YESNO 4
#define IDYES 6
#define IDNO 7
#define IDOK 1
#define IDCANCEL 2
SHORT GetAsyncKeyState(int vk);
#define CreateEvent CreateEventA
#define CreateFile CreateFileA
#define OutputDebugString OutputDebugStringA
#define GetFileAttributes GetFileAttributesA
#define FindFirstFile FindFirstFileA
#define FindNextFile FindNextFileA

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus
// Interlocked ops: templates, because the engine passes int32_t*/uint32_t*/long* interchangeably (MSVC tolerates it).
extern "C++" {
template <class T> static inline T InterlockedIncrement(volatile T *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
template <class T> static inline T InterlockedDecrement(volatile T *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
template <class T, class U> static inline T InterlockedExchange(volatile T *p, U v) { return __atomic_exchange_n(p, (T)v, __ATOMIC_SEQ_CST); }
template <class T, class U> static inline T InterlockedExchangeAdd(volatile T *p, U v) { return __atomic_fetch_add(p, (T)v, __ATOMIC_SEQ_CST); }
template <class T, class U, class V> static inline T InterlockedCompareExchange(volatile T *p, U xchg, V cmp) { T c = (T)cmp; __atomic_compare_exchange_n(p, &c, (T)xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
template <class T, class U> static inline T InterlockedOr(volatile T *p, U v) { return __atomic_fetch_or(p, (T)v, __ATOMIC_SEQ_CST); }
template <class T, class U> static inline T InterlockedAnd(volatile T *p, U v) { return __atomic_fetch_and(p, (T)v, __ATOMIC_SEQ_CST); }
}
#endif
