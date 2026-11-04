# KisakCOD → WebAssembly (work in progress)

Port of the KisakCOD CoD4 reimplementation to the browser. Everything web-specific lives in this folder;
the engine sources under `src/` carry only small patches (see `git diff`).

## Status
- **M0 (done):** the real engine core (dvars, cmds, FS, memory, scripting tables, fastfile loader + database thread)
  builds as wasm32 and runs under node. It loads the **Mac retail** fastfiles unmodified: `code_post_gfx_mp`,
  `localized_code_post_gfx_mp`, `ui_mp`, `common_mp`, `localized_common_mp` (12,138 assets) and the map `mp_crash`
  (+1,798 assets, GfxWorld with 5,558 surfaces / 166k vertices).
- **M1 (done):** the engine runs in the browser: the real `Com_Init`/`Com_Frame` loop, a **D3D9 → WebGL2 device**
  (`web/gfx/`), and the game's own SM3 shaders translated at runtime from their D3D9 bytecode to GLSL ES 3.00
  (all 826 shaders in the startup zones translate and compile). Renders CoD4's real menus (profile selection screen)
  at ~65 fps from the Mac fastfiles, which are streamed to the engine over HTTP range requests from a local dev server.
  No GL errors with `?args=--gldebug`.
- **M2 (next):** player profile + keyboard/mouse input, loading screen while a map loads, map rendering (`+devmap mp_crash`
  currently stops at "You have not chosen a profile yet"), CPU skinning (`R_SkinXSurfaceSkinnedSse` is not built yet),
  depth-texture shadow maps, `texkill`/MRT corner cases. **M3:** audio (Web Audio backend for the OpenAL/EFX layer).
  **M4:** WebSocket networking.

## Build & run (needs emscripten, python3, node)
```bash
python3 web/tools/build.py --link m0        # compile all engine TUs to wasm objects + link web/build/m0/m0.js
web/run_m0.sh                               # runs against the Mac data dir; override with KISAK_INSTALL_PATH=...
KISAK_TRACE_FS=1 web/run_m0.sh              # trace every file open/stat the engine does
M0_WATCHDOG_SEC=60 web/run_m0.sh            # exit(3) after 60 s (use with node --cpu-prof to find stalls)
python3 web/tools/survey.py                 # syntax-check every TU, grouped by error (fast: ~15 s)
```
`web/tools/exclude.txt` lists the Windows-only source files that are replaced for the web.

## Run the browser build (M1)
```bash
python3 web/tools/build.py --link m1        # -> web/build/m1/index.{html,js,wasm}
web/run_m1.sh                               # serves it + your game data on http://localhost:8080 (open in Chrome)
```
URL options: `?args=--gldebug` (check GL errors after every draw), `?args=--trace-fs`, and any engine command line, e.g.
`?args=%2Bset%20developer%200`. The dev server (`web/tools/serve.py`) adds the COOP/COEP headers that wasm threads need
and serves the install dir read-only under `/data/` (never copied or uploaded).

## Layout
- `shim/`      Win32 / D3D9 / DirectInput / Winsock / MSVC-CRT compatibility headers + `win32_shim.cpp` (threads/events/critical
               sections over pthreads, overlapped reads) over `kvfs.cpp` (virtual files: POSIX under node; HTTP range
               requests + in-memory writes in the browser; Windows-path + case-insensitive handling).
- `gfx/`       the D3D9→WebGL2 device (`d3d9_gl_device.cpp`: state, FBOs, programs, draws; `d3d9_gl_resources.cpp`: textures,
               buffers, queries, formats) and `shader_xlate.cpp`, the engine-independent D3D9 bytecode→GLSL translator
               (native test driver: `tools/xlate_test.cpp`).
- `platform/`  web replacements for `win_main.cpp` (`sys_web.cpp`) and stand-ins for excluded subsystems (`stubs_web.cpp`).
- `m0/`, `m1/` the node harness / the browser entry point + HTML shell. `tools/` build/survey/link scripts + dev server.

## Gotchas found so far (all handled)
- wasm traps on mismatched `call_indirect` signatures; the decompiled code casts function pointers freely →
  `-sEMULATE_FUNCTION_POINTER_CASTS=1`.
- Retail text files are CRLF and the engine relies on MSVC text-mode translation → emulated in `kisak_fopen`.
- `CreateThread(CREATE_SUSPENDED)` / matching `GetCurrentThreadId()` ids are relied on by the database thread.
- Engine critical sections must be recursive (use the Win32 branch of `win_common.cpp`).
- Itanium ABI doesn't mangle return types → two same-named `static`-intended functions collided.
- `DB_LoadXAssets(..., sync=1)` holds the asset-hash write lock while the DB thread needs it → use sync=0 + `DB_SyncXAssets()`.

## Browser/WebGL gotchas (M1)
- A WebGL context belongs to one thread, but the engine creates GPU resources on its fastfile thread: resource creation, Lock
  and Unlock only touch CPU memory; GL objects are created lazily on first use by the engine's main thread, and destructors
  queue GL deletes for it. The engine's dedicated render thread is not used (`startedRenderThread = 0`, `sys_smp_allowed 0`).
- Frames only reach the screen when the worker yields, so the frame loop is `emscripten_set_main_loop` (one `Com_Frame` per rAF).
- WebGL never completes a query/fence in the task that issued it, and the engine spin-waits on them → `GetData` reports completion
  immediately (occlusion counts come back 0, so the sun-sprite calibration falls back to the low-quality path).
- D3D9→GL conventions fixed in the translated vertex shader: z [0,w]→[-w,w], y flip (render targets keep D3D row order,
  Present blits flipped), half-pixel offset. D3D alpha-test / sRGB-write render states are emulated in the pixel shader.
- The process "cwd" is the install root (`Sys_Cwd` is the engine's base path) and relative paths resolve against it.
