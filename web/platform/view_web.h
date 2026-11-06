// view_web.h -- the shape of the area the page shows the game in (see view_web.cpp).
#pragma once

// Called by the renderer (R_SetWndParms) with the resolution the game would use (from r_mode); reshapes it, keeping the pixel count, to the
// aspect ratio of the area the page shows the game in, so the canvas fills a fullscreen / resized browser window without black bars.
void kisak_fit_display(int *width, int *height);

namespace webview {
void Attach();                          // tells the page where the shared request lives (browser main thread callback kisakViewAttach)
void SetInitial(int cssW, int cssH);    // --view=WxH on the command line: the stage size the page measured before the engine started
void Frame();                           // per engine frame: restarts the renderer when the shape has changed and that is allowed
}
