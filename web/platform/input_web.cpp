// input_web.cpp -- browser keyboard / mouse input, replacing win_input.cpp (DirectInput) and the key handling in win_wndproc.cpp.
//
// DOM events are registered from the engine thread with the html5 API; the callbacks run on that same thread between frames
// (the browser frame loop yields), so they can post straight into the engine's event queue / mouse accumulators.
#include <universal/q_shared.h>
#include <Windows.h>
#include <client/client.h>
#include <qcommon/qcommon.h>
#include <client_mp/client_mp.h>
#include <ui/keycodes.h>
#include <win32/win_local.h>
#include <win32/win_input.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>

static const char *kCanvas = "#canvas";

static std::unordered_map<std::string, int> g_keyMap;
static bool g_inited = false;
static bool g_hadPointerLock = false;   // the browser reports the pointer as locked
static bool g_unlockedByUs = false;     // we asked for the current/next unlock (it is not the player pressing Esc)
static bool g_wantedLock = false;       // WantPointerLock() as of the previous frame

// Should the browser hold the pointer (first-person aiming from relative motion)? Only while the player controls the game. A menu or the
// console is driven by the *absolute* pointer position, which a locked pointer stops reporting (the cursor freezes), and
// CL_IsLocalClientInGame() is already true while the team / class menus are open after joining.
static bool WantPointerLock()
{
    return CL_IsLocalClientInGame(0) && !Key_IsCatcherActive(0, KEYCATCH_UI | KEYCATCH_CONSOLE | KEYCATCH_SCRIPT);
}

// accumulated mouse state, consumed by IN_Frame
static int g_mouseX = 0, g_mouseY = 0, g_dx = 0, g_dy = 0;
static bool g_mouseMoved = false;

static void BuildKeyMap()
{
    auto &m = g_keyMap;
    for (char c = 'a'; c <= 'z'; ++c) { char buf[8]; snprintf(buf, sizeof buf, "Key%c", c - 32); m[buf] = c; }
    for (char c = '0'; c <= '9'; ++c) { char buf[8]; snprintf(buf, sizeof buf, "Digit%c", c); m[buf] = c; }
    m["Space"] = K_SPACE; m["Enter"] = K_ENTER; m["Tab"] = K_TAB; m["Escape"] = K_ESCAPE; m["Backspace"] = K_BACKSPACE;
    m["Minus"] = '-'; m["Equal"] = '='; m["BracketLeft"] = '['; m["BracketRight"] = ']'; m["Semicolon"] = ';'; m["Quote"] = '\'';
    m["Backquote"] = '`'; m["Comma"] = ','; m["Period"] = '.'; m["Slash"] = '/'; m["Backslash"] = '\\'; m["IntlBackslash"] = '\\';
    m["ArrowUp"] = K_UPARROW; m["ArrowDown"] = K_DOWNARROW; m["ArrowLeft"] = K_LEFTARROW; m["ArrowRight"] = K_RIGHTARROW;
    m["ShiftLeft"] = m["ShiftRight"] = K_SHIFT; m["ControlLeft"] = m["ControlRight"] = K_CTRL; m["AltLeft"] = m["AltRight"] = K_ALT;
    m["Insert"] = K_INS; m["Delete"] = K_DEL; m["PageDown"] = K_PGDN; m["PageUp"] = K_PGUP; m["Home"] = K_HOME; m["End"] = K_END;
    m["CapsLock"] = K_CAPSLOCK; m["Pause"] = K_PAUSE;
    for (int i = 1; i <= 12; ++i) { char buf[8]; snprintf(buf, sizeof buf, "F%d", i); m[buf] = K_F1 + (i - 1); }
    m["Numpad0"] = K_KP_INS; m["Numpad1"] = K_KP_END; m["Numpad2"] = K_KP_DOWNARROW; m["Numpad3"] = K_KP_PGDN;
    m["Numpad4"] = K_KP_LEFTARROW; m["Numpad5"] = K_KP_5; m["Numpad6"] = K_KP_RIGHTARROW; m["Numpad7"] = K_KP_HOME;
    m["Numpad8"] = K_KP_UPARROW; m["Numpad9"] = K_KP_PGUP; m["NumpadEnter"] = K_KP_ENTER; m["NumpadDecimal"] = K_KP_DEL;
    m["NumpadAdd"] = K_KP_PLUS; m["NumpadSubtract"] = K_KP_MINUS; m["NumpadMultiply"] = K_KP_STAR; m["NumpadDivide"] = K_KP_SLASH;
    m["NumLock"] = K_KP_NUMLOCK;
}

static int KeyFromCode(const char *code)
{
    auto it = g_keyMap.find(code);
    return it == g_keyMap.end() ? 0 : it->second;
}

// Let the browser keep its own shortcuts (reload, devtools, copy/paste...) but swallow everything the game uses.
static bool ShouldPassToBrowser(const EmscriptenKeyboardEvent *e)
{
    if (e->ctrlKey || e->metaKey) return strcmp(e->code, "ControlLeft") && strcmp(e->code, "ControlRight");
    return !strcmp(e->code, "F5") || !strcmp(e->code, "F11") || !strcmp(e->code, "F12");
}

// The engine's view of which keys / mouse buttons are held. A key-up must reach the engine for every key it saw go down (else the key stays "held":
// the player keeps running, the scoreboard stays open), whatever modifiers are down at that moment, and must never be sent for a key it did not
// see go down (CL_KeyEvent counts held keys and would be thrown off). Everything still held is released when the window loses the focus: the
// key-up then goes to whatever has the focus (the browser's toolbar after a Tab, another application after Cmd+Tab), not to the page.
static bool g_down[256];

static void QueKey(int key, bool down)
{
    if (key <= 0 || key >= 256) return;
    if (!down && !g_down[key]) return;
    g_down[key] = down;
    Sys_QueEvent(0, SE_KEY, key, down ? 1 : 0, 0, 0);
}

static void ReleaseAllKeys()
{
    for (int key = 1; key < 256; ++key) QueKey(key, false);
    g_dx = g_dy = 0;
}

static bool OnKey(int type, const EmscriptenKeyboardEvent *e, void *)
{
    bool down = type == EMSCRIPTEN_EVENT_KEYDOWN;
    int key = KeyFromCode(e->code);
    if (!down) {                       // (even Ctrl+<key>: let go of W while Ctrl, the crouch key, is still held)
        if (key) QueKey(key, false);
        return key != 0;
    }
    if (ShouldPassToBrowser(e)) return false;
    if (key) QueKey(key, true);
    // Characters for text fields (console, profile name, chat): taken from keydown so they follow the key event, as WM_CHAR does
    if (!e->ctrlKey && !e->metaKey && !e->altKey && e->key[0] && !e->key[1]) {
        unsigned char ch = (unsigned char)e->key[0];
        if (ch >= 32 && ch < 127) Sys_QueEvent(0, SE_CHAR, ch, 0, 0, 0);
    }
    // Windows delivers Backspace as a character (WM_CHAR, 8) as well, and that is what the UI text fields and the console delete with (they ignore the
    // key event). Held down, keydown repeats, like WM_CHAR does.
    if (!strcmp(e->code, "Backspace") && !e->ctrlKey && !e->metaKey && !e->altKey) Sys_QueEvent(0, SE_CHAR, 8, 0, 0, 0);
    // (The value cannot cancel the browser's default action: these callbacks run on the engine thread, the event was long since handled on the page's
    // thread. That is why shell.html has its own keydown listener that calls preventDefault() on the keys the game uses -- Tab above all, which would
    // otherwise move the keyboard focus out of the page to the browser's toolbar.)
    return key != 0 || (e->key[0] && !e->key[1]);
}

// The window lost the keyboard focus (Tab / a click moved it to the browser's toolbar, Cmd+Tab, a dialog) or the tab was hidden: the key-ups of
// everything still held will not come to the page.
static bool OnWindowBlur(int, const EmscriptenFocusEvent *, void *) { ReleaseAllKeys(); return false; }
static bool OnVisibility(int, const EmscriptenVisibilityChangeEvent *e, void *) { if (e->hidden) ReleaseAllKeys(); return false; }

static void CanvasScale(double *sx, double *sy)
{
    int bw = 0, bh = 0;
    double cw = 0, ch = 0;
    emscripten_get_canvas_element_size(kCanvas, &bw, &bh);
    emscripten_get_element_css_size(kCanvas, &cw, &ch);
    *sx = (cw > 0 && bw > 0) ? bw / cw : 1.0;
    *sy = (ch > 0 && bh > 0) ? bh / ch : 1.0;
}

static bool OnMouseMove(int, const EmscriptenMouseEvent *e, void *)
{
    double sx, sy;
    CanvasScale(&sx, &sy);
    g_mouseX = (int)(e->targetX * sx);
    g_mouseY = (int)(e->targetY * sy);
    g_dx += (int)(e->movementX * sx);
    g_dy += (int)(e->movementY * sy);
    g_mouseMoved = true;
    return true;
}

static int ButtonKey(unsigned short button)
{
    switch (button) {
    case 0: return K_MOUSE1;
    case 2: return K_MOUSE2;
    case 1: return K_MOUSE3;
    case 3: return K_MOUSE4;
    case 4: return K_MOUSE5;
    default: return 0;
    }
}

static bool OnMouseButton(int type, const EmscriptenMouseEvent *e, void *)
{
    int key = ButtonKey(e->button);
    if (!key) return false;
    bool down = type == EMSCRIPTEN_EVENT_MOUSEDOWN;
    QueKey(key, down);
    // capture the mouse for first-person control while the player is controlling the game (not while a menu has the cursor)
    if (down && WantPointerLock()) emscripten_request_pointerlock(kCanvas, true);
    return true;
}

static bool OnWheel(int, const EmscriptenWheelEvent *e, void *)
{
    int key = e->deltaY < 0 ? K_MWHEELUP : K_MWHEELDOWN;
    Sys_QueEvent(0, SE_KEY, key, 1, 0, 0);
    Sys_QueEvent(0, SE_KEY, key, 0, 0, 0);
    return true;
}

// The browser swallows Esc while the pointer is locked and only reports the lock being lost: turn that back into an Esc
// press so the in-game menu opens.
static bool OnPointerLockChange(int, const EmscriptenPointerlockChangeEvent *e, void *)
{
    if (g_hadPointerLock && !e->isActive && !g_unlockedByUs) {
        Sys_QueEvent(0, SE_KEY, K_ESCAPE, 1, 0, 0);
        Sys_QueEvent(0, SE_KEY, K_ESCAPE, 0, 0, 0);
    }
    g_unlockedByUs = false;
    g_hadPointerLock = e->isActive != 0;
    return true;
}

static bool OnContextMenu(int, const EmscriptenMouseEvent *, void *) { return true; }

// (Not `typeof document`: in the browser the engine runs in a worker, which has none -- the html5 calls below are proxied to the page's main thread.)
EM_JS(int, kn_is_node, (), { return (typeof process !== 'undefined' && process.versions && process.versions.node) ? 1 : 0; });

void __cdecl IN_Init()
{
    if (g_inited) return;
    g_inited = true;
    BuildKeyMap();
    if (kn_is_node()) return;   // the dedicated server runs under node: no page, no input devices
    EMSCRIPTEN_RESULT r1 = emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, OnKey);
    EMSCRIPTEN_RESULT r2 = emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, OnKey);
    if (r1 || r2) fprintf(stderr, "[input] key callback registration failed: %d %d\n", r1, r2);
    emscripten_set_mousemove_callback(kCanvas, nullptr, true, OnMouseMove);
    emscripten_set_mousedown_callback(kCanvas, nullptr, true, OnMouseButton);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, OnMouseButton);
    emscripten_set_wheel_callback(kCanvas, nullptr, true, OnWheel);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, OnPointerLockChange);
    // (not captured: a capturing listener on the window would also see the blur of every element inside the page)
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, OnWindowBlur);
    emscripten_set_visibilitychange_callback(nullptr, true, OnVisibility);
    emscripten_set_contextmenu_callback(kCanvas, nullptr, true, OnContextMenu);
    Com_Printf(CON_CHANNEL_SYSTEM, "Browser input initialised\n");
}

void __cdecl IN_Shutdown() {}

void __cdecl IN_Frame()
{
    // A menu opened while the pointer is locked (the game opens one by itself, e.g. after dying or on a team change): give the pointer back so the
    // menu cursor can move. When the menu closes, ask to lock again; the browser performs the request at the next click or key press.
    const bool want = WantPointerLock();
    if (!want && g_hadPointerLock) {
        if (!g_unlockedByUs) {               // (once: the browser confirms the unlock a few frames later)
            g_unlockedByUs = true;
            emscripten_exit_pointerlock();
        }
    } else if (want && !g_wantedLock && !g_hadPointerLock) {
        emscripten_request_pointerlock(kCanvas, true);
    }
    g_wantedLock = want;
    if (g_mouseMoved) {
        int dx = g_dx, dy = g_dy;
        g_dx = g_dy = 0;
        g_mouseMoved = false;
        CL_MouseEvent(g_mouseX, g_mouseY, dx, dy);
    }
}

void __cdecl IN_ShowSystemCursor(BOOL) {}
void __cdecl IN_RecenterMouse() {}
void IN_ActivateMouse(qboolean) {}
void __cdecl IN_SetCursorPos(tagPOINT p) { g_mouseX = p.x; g_mouseY = p.y; }
bool __cdecl IN_IsForegroundWindow() { return true; }
void __cdecl IN_SetForegroundWindow() {}
void IN_ClearStates() {}
