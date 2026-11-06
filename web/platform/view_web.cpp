// view_web.cpp -- fit the render resolution to the area the page shows the game in.
//
// The engine renders at a fixed resolution (r_mode: 1024x768, 1280x720, ...) and the page scales the canvas to its window. When that window (or the
// fullscreen screen) has another shape than the render target, the picture is letterboxed or stretched: not "fully fullscreen". So the page reports
// the size of the game's stage (CSS pixels; app/shell.html, kisakViewAttach) through a few ints in shared wasm memory, and
//   - kisak_fit_display() (called from R_SetWndParms) reshapes the resolution r_mode asks for to that aspect ratio, keeping the pixel count (so the
//     Video > Resolution menu stays the quality knob and the cost stays what the player chose);
//   - Frame() runs `vid_restart` when the shape changes later on (fullscreen on/off, a resized window), where the engine allows that.
// The engine supports any resolution: it derives the 4:3 / 16:10 / 16:9 HUD layout from the shape and corrects the pixel aspect.
#include <universal/q_shared.h>
#include <Windows.h>
#include <client/client.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <client_mp/client_mp.h>
#include <emscripten.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "view_web.h"

namespace {

// Written by the page (Atomics.store on the wasm memory), read by the engine thread.
struct ViewReq { volatile int32_t w, h, seq, pad; };
ViewReq g_req = {};
int g_initW = 0, g_initH = 0;      // --view=: what the page measured before the engine started
double g_applied = 0;              // aspect of the resolution the renderer was last set up with (0 = not yet)
double g_lastTryMs = -1e9;
int g_tries = 0;

const double kMinAspect = 4.0 / 3.0;    // narrower windows get black bars at the sides
const double kMaxAspect = 21.0 / 9.0;   // and wider ones (the 16:9 HUD layout is the widest the engine knows)

bool Wanted(double *aspect)
{
    int w = __atomic_load_n(&g_req.w, __ATOMIC_RELAXED), h = __atomic_load_n(&g_req.h, __ATOMIC_RELAXED);
    if (w <= 0 || h <= 0) { w = g_initW; h = g_initH; }
    if (w <= 0 || h <= 0) return false;
    double a = (double)w / (double)h;
    *aspect = a < kMinAspect ? kMinAspect : a > kMaxAspect ? kMaxAspect : a;
    return true;
}

} // namespace

void kisak_fit_display(int *width, int *height)
{
    double a;
    if (*width <= 0 || *height <= 0 || !Wanted(&a)) {
        if (*width > 0 && *height > 0) g_applied = (double)*width / (double)*height;
        return;
    }
    const double area = (double)*width * (double)*height;
    int h = (int)(sqrt(area / a) + 0.5) & ~1;
    int w = (int)(h * a + 0.5) & ~1;
    if (w < 320 || h < 240) return;
    fprintf(stderr, "[view] render size %dx%d -> %dx%d (stage aspect %.3f)\n", *width, *height, w, h, a);
    *width = w; *height = h;
    g_applied = (double)w / (double)h;
    g_tries = 0;
}

namespace webview {

void Attach()
{
    MAIN_THREAD_ASYNC_EM_ASM({
        if (typeof kisakViewAttach === 'function') kisakViewAttach(wasmMemory, $0);
    }, (uintptr_t)&g_req);
}

void SetInitial(int cssW, int cssH) { if (cssW > 0 && cssH > 0) { g_initW = cssW; g_initH = cssH; } }

void Frame()
{
    double a;
    if (g_applied <= 0 || !Wanted(&a)) return;
    if (fabs(a / g_applied - 1.0) < 0.03) { g_tries = 0; return; }   // close enough: the canvas is letterboxed by a few pixels at most
    if (g_tries >= 3) return;                                        // vid_restart did not take: do not loop
    // Only from the main menu. `vid_restart` refuses to run while this tab hosts a game ("Listen server cannot video restart.": the server shares
    // the renderer's memory), and in a match it reloads the map's assets and re-initialises the client, which a fullscreen toggle should not cause.
    if (com_sv_running->current.enabled || CL_GetLocalClientConnectionState(0) != CA_DISCONNECTED) return;
    double now = emscripten_get_now();
    if (now - g_lastTryMs < 3000.0) return;
    g_lastTryMs = now;
    ++g_tries;
    fprintf(stderr, "[view] stage aspect %.3f, render aspect %.3f: restarting the renderer\n", a, g_applied);
    Cbuf_AddText(0, "vid_restart\n");
}

} // namespace webview
