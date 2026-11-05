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
- **M2 (done):** playable offline. `?args=%2Bdevmap%20mp_crash` loads the real `mp_crash` map, runs the real GSC scripts, shows the
  loading screen, the team/class menus and drops you into first person (M16A4 viewmodel, HUD, minimap); keyboard + mouse work
  (`platform/input_web.cpp`), ~45 fps in game, 0 GL errors. A "Player" profile is pre-seeded so you skip the profile screen.
- **M3 (done):** sound. `audio/al_web.cpp` implements the OpenAL subset the engine's sound driver uses (buffers, static + queued/streaming
  sources, looping, pitch, gain, L/R pan from the source position, soft limiter) as a mixer on its own pthread, writing 48 kHz stereo
  into a ring buffer in shared wasm memory; `app/audio-worklet.js` (an AudioWorklet) plays it. Emscripten's own OpenAL can't be used:
  it needs an `AudioContext`, which doesn't exist in the engine's worker. The page creates the context suspended and resumes it on
  the first click/key (an on-page hint shows until then). `kisakAudio.peak()` / `.maxPeak` in the JS console show the output level.
- **M4 (done, local):** multiplayer between browser tabs. Browsers can't do UDP, so `platform/net_web.cpp` replaces Winsock with a UDP-over-WebSocket
  transport and `tools/relay.py` is a tiny virtual LAN switch (each tab gets an address like `10.77.0.1`). The unmodified netcode runs on top:
  `getinfo`/`getstatus` queries, challenge, connect, stats upload, snapshots, usercmds, team/class menus and spawning all work between a
  hosting tab and a joining tab. Hosting is "Start New Server" in the menu, joining is the in-game server browser (source Local). `run.sh --lan`
  opens it to other computers on your network. See "Multiplayer" below.
- **Next:** make it public (relay on a server, signed identities, a dedicated server build instead of a hosting tab), persistence of
  profile/config, performance of menus. Known rough edges: no reverb/EQ (EFX is stubbed), no HRTF/3D beyond left-right pan, menu frames
  are slow (~12 fps, blur passes), the loading screen only redraws when the engine yields, depth-texture shadow maps use the colour
  fallback, occlusion queries report 0, Bink is a stub, profile/config files are memory-only (lost on reload), no gamepad, no voice chat.

## Build & run (needs emscripten, python3)
```bash
python3 web/tools/build.py --link app       # compile all engine TUs to wasm objects + link web/build/app/index.{html,js,wasm}
web/run.sh                                  # serves it + your game data on http://localhost:8080 (open in Chrome)
```
`web/tools/exclude.txt` lists the Windows-only source files that are replaced for the web. Other tools:
```bash
python3 web/tools/build.py --link loadtest  # the node load test: link web/build/loadtest/loadtest.js instead of the browser build
web/run_loadtest.sh                         # loads the fastfiles with the unmodified engine loader; override the data dir with KISAK_INSTALL_PATH=...
KISAK_TRACE_FS=1 web/run_loadtest.sh        # trace every file open/stat the engine does
LOADTEST_WATCHDOG_SEC=60 web/run_loadtest.sh   # exit(3) after 60 s (use with node --cpu-prof to find stalls)
python3 web/tools/survey.py                 # syntax-check every TU, grouped by error (fast: ~15 s)
```
(The load test runs the engine under node with direct file access (`NODERAWFS`), so it can create stray files in the game data directory;
the browser build never writes there.)

**Start screen.** Opening the page is a full-screen key-art start screen (`app/hero.webp`, the game's own artwork: keep it out of anything you publish) with how much a first
start downloads and a **Start** button (also Enter). Until you press it the page requests nothing of the game (no wasm, no workers, no data; only `hero.webp` and
the tiny `/start-info.json`): `app/shell.html` keeps emcc's loader script inert in a `<template>` and adds it on click. The click is also the user
gesture the browser wants before it lets audio play, so sound starts without a second click. The size comes from `serve.py` (`/start-info.json`: all game
data on the server, and an estimate of "menus + the largest recorded map" from the load traces below, x1.25 for what the traces miss; until the
traces exist it shows the total instead). If the browser cannot run the game (page not cross-origin isolated, e.g. plain `http://<lan ip>`; no WebGL 2)
the screen says so and disables Start instead of showing a black canvas. `?autostart` skips the button (for development and scripts).

Controls: WASD / mouse / click as usual; Esc opens the in-game menu (the browser swallows Esc while the pointer is locked, so the
loss of pointer lock is turned back into an Esc press). The pointer is only locked while the player controls the game: not while a menu, the
console or a script menu has the cursor (those follow the *absolute* pointer position, which a locked pointer stops reporting, so the menu cursor
froze: clicking a team button right after joining used to lock it, because the client already counts as "in game" then). It is released when a menu
opens and re-requested when it closes (the browser performs that at the next click or key press). `--no-profile` shows the profile screen instead of seeding one.
URL options: `?args=--nosound` (skip the audio mixer), `?args=--gldebug` (check GL errors after every draw), `?args=--trace-fs`, and any engine command line, e.g.
`?args=%2Bset%20developer%200`. The dev server (`web/tools/serve.py`) adds the COOP/COEP headers that wasm threads need
and serves the install dir read-only under `/data/` (never copied or uploaded).


## Multiplayer
`web/run.sh` (i.e. `tools/serve.py`) also starts the relay on `ws://localhost:8081/` (`--relay-port 0` disables it; `tools/relay.py` runs it standalone).
Every tab that opens the game connects to it and is assigned a virtual address, printed in the log as `Network: relay ..., local address 10.77.0.N:28960`
(the first tab is `10.77.0.1`).

- **Host:** open `http://localhost:8080/`, choose **Start New Server** in the main menu, pick a map and press Start (or skip the menus with
  `?args=%2Bdevmap%20mp_crash`). It is a listen server: the hosting tab is the server, so keep it open (a hidden tab keeps serving through a
  worker-timer watchdog in `app/main.cpp`, since `requestAnimationFrame` stops in hidden tabs).
- **Join:** open the game in another tab (or computer, see below), choose **Join Game**. The browser starts on source *Local* and lists the
  servers found on the relay's virtual LAN (Refresh List rescans); select one and press Join Server. The joiner loads the map, then picks a
  team/class like in single player. (`?args=%2Bconnect%2010.77.0.1`, or `connect <address>` in the console, still works; the host's address is
  in its log as `Network: relay ..., local address 10.77.0.N` and is *not* always `.1`: addresses are handed out in connection order.)
  **Join timeouts:** while a joiner loads the map its engine thread sends no packets, and from a cold cache over a network that takes
  minutes. The stock `sv_connectTimeout` (45 s) would drop it with *Notice: Server Disconnected* (`EXE_TIMEDOUT` in the host's log), so
  `app/main.cpp` defaults `sv_connectTimeout 1800` and `cl_connectTimeout 3600` (their maximums). The host's tab decides the server side:
  reload the hosting tab after updating the build.
- **Other computers / phones on your network:** `web/run.sh --lan` listens on all interfaces and lets the relay accept pages served from
  private-network addresses (`192.168.x.y`, `10.x.y.z`, `*.local`; matched by IP literal, never by resolving a public name). It prints the URLs to
  open. **Browsers only enable `SharedArrayBuffer` and AudioWorklet in a secure context** (https, or `http://localhost`), so a plain
  `http://192.168.x.y:8080` page cannot run the game. Either
  - give the server a certificate the client trusts: `mkcert 192.168.1.100 my-mac.local`, then
    `web/run.sh --lan --tls-cert <cert.pem> --tls-key <key.pem>` (HTTPS and `wss://` on the same certificate; the client needs mkcert's root CA
    installed; a self-signed certificate you click through is *not* a secure context), or
  - in Chrome on the client open `chrome://flags/#unsafely-treat-insecure-origin-as-secure`, add `http://192.168.x.y:8080`, relaunch.
  `--lan` serves your game data to everyone who can reach the port: use it on a network you trust, not through a public tunnel. A phone also lacks
  touch controls (keyboard + mouse only). Another computer without these steps can still reach it through `ssh -N -L 8080:localhost:8080
  -L 8081:localhost:8081 <this-mac>`: its browser then sees `localhost`.
- **Query a server** (works from anything that can speak WebSocket, e.g. a script): `getinfo`/`getstatus` out-of-band packets, see `tools/relay.py`'s
  `SimpleClient`. LAN broadcast (`255.255.255.255`) is forwarded to every other tab by the relay; that is what the in-game LAN browser sends.

How it fits together:
- `platform/net_web.cpp`: `Sys_GetPacket`/`Sys_SendPacket`/`Sys_StringToAdr`/`Sys_IsLANAddress`/`NET_*` over one WebSocket owned by the engine's worker
  (created with `EM_JS`, packets queued by `onmessage`, drained between frames). Dvar `net_relay` (`auto` = same host, port 8081; empty = offline).
  Frames are `[ip:4][port:2][payload]` in both directions; the relay stamps the sender's address on forwarded datagrams.
- `platform/steam_web.cpp` replaces `win_steam.cpp`: the engine's connect handshake needs a SteamID64 and an auth ticket. Each page load makes a random
  SteamID64-shaped identity and a ticket `"KWEB" + id`, which the server accepts if the id matches. **That proves nothing about who the player
  is** (anyone can claim any id): a public deployment needs the relay (or a login service) to sign identities and the server to verify them.
- The relay only accepts WebSocket connections whose `Origin` is localhost (or `--allow-origin`; with `--lan` also private-network pages), binds
  127.0.0.1 unless `--lan`, and never inspects payloads. `python3 web/tools/relay.py --selftest` checks routing, broadcast, loopback, address
  reuse and the origin policy.
- **Why the in-game browser was empty at first:** discovery worked (broadcast `getinfo` -> `infoResponse`), but the stock browser filters hid
  every web server: "pure servers only" (`ui_browserShowPure 1`) and "mod = 0" (`ui_browserMod 0`), while a web server runs `sv_pure 0` (it does
  not check .iwd checksums), so its infoResponse has no `pure` key and says `mod 1`; and the default source was Internet (`ui_netSource 1`, no
  master server here). `app/main.cpp` now starts with `+set ui_netSource 0 +set ui_browserShowPure 0 +set ui_browserMod -1`; a page's `?args=` come
  after and can override them.
- `app/main.cpp` calls `LiveStorage_ReadStats()` after `Com_Init`: the seeded profile never goes through the menu profile switch that normally
  initialises the player's stats, and the game scripts kick any remote player whose class stats are all zero (host exempt).
- `dumprawfile <name>` (devcmds_web.cpp) prints a game script/rawfile from the loaded fastfiles to the log, e.g.
  `?args=%2Bdevmap%20mp_crash%20%2Bdumprawfile%20maps/mp/gametypes/_globallogic.gsc` (this is how the stats kick was found).
- In the Claude desktop browser pane only the tab you drive is reliable for input (and screenshots lag a frame); two game tabs side by side work, but
  both share the CPU, so expect single-digit fps on the hidden one.

## Load time (game data over the network)
The engine reads the game data with *synchronous* reads from its own worker threads; in the browser each one becomes an HTTP range request to
`tools/serve.py`. Measured with `serve.py --sim-rtt-ms 8 --sim-mbps 150` (a decent Wi-Fi link), page open until the team-select menu of `mp_crash`:

| | time | notes |
|---|---|---|
| original (1 MB block per open handle) | > 4 min | 1.17 GB fetched after 90 s for 188 MB actually read; only `common_mp` loaded by then |
| 64 KB shared block cache + adaptive read-ahead | 57 s | 490 MB fetched, 2246 requests |
| + I/O worker, cold persistent cache | 45 s | parallel read-ahead for the fastfiles |
| + I/O worker, warm persistent cache (2nd load) | **10 s** | 26 requests (11 MB) to the server, 452 MB from local disk |

What was wrong: the `.iwd` archives (167 MB each) are read with tiny reads (3 B, 1 KB, a few KB) at scattered offsets, and every one fetched a whole
1 MB block (6x more data than used overall, 17x for some archives), while the fastfiles are big sequential reads. A bigger chunk size would have made
it worse. Now (all in `shim/kvfs.cpp`, `app/io-worker.js`):
- **Block cache:** random reads fetch only the 64 KB blocks they touch, kept in a global 96 MB LRU shared by all file handles. A miss right where the
  previous fetch ended is treated as a sequential stream: the window doubles (128 KB ... 4 MB) and goes to a private per-handle buffer, so streaming a 57 MB
  fastfile does not flush the archive blocks.
- **I/O worker + persistent cache:** engine threads post each range read to `io-worker.js` (control block in shared memory, `Atomics.wait`). It serves what
  it already has from a log file in the browser's Origin Private File System (`data.bin` + an append-only `index.bin`, keyed by each file's size and mtime
  from `/data-manifest.json`), fetches only the missing gaps (up to 4 in parallel) and reads ahead on sequential streams. So the *second* page load, or the next map, mostly
  reads local disk. Cap 6 GB: once `data.bin` would pass it, nothing new is cached for the rest of the session (cached ranges are still served) and the next page load empties the cache and starts over (a plain wipe, not LRU; entries of changed game files are never reclaimed individually). `?args=--nocache` skips it, `?args=--clearcache` empties it first; if the worker or OPFS is unavailable the
  engine falls back to plain synchronous XHR.
- A second tab of the same browser profile cannot open the (exclusive) cache files, so it runs network-only (still with the block cache).

**Cold loads on a slower or higher-latency link (joining a map from another computer).** Even with the above, a *cold* map load is a long chain of
synchronous round trips: the engine asks for one image (100-300 KB at a scattered offset in a 167 MB `.iwd`), waits, asks for the next, ... about 2000 times,
~450 MB. Per-request latency, not bandwidth, sets the speed, and the engine only ever has one request outstanding. Measured with
`serve.py --sim-rtt-ms 10 --sim-mbps 50` (each connection capped at 50 Mbit/s), cold cache, until the team-select menu: **93 s -> 22 s** for `devmap mp_crash`,
and a cold *joiner* (own origin, own cache) **117 s -> 26 s**. What changed:
- **Load traces.** What a phase reads is the same every time, so `io-worker.js` records it and the server keeps it (`web/build/traces/<phase>.json`,
  a few KB each). A *phase* is `startup` (until the first fastfile is read) and then one per zone fastfile read from offset 0 (`localized_common_mp`,
  `common_mp`, `ui_mp`, `mp_crash`, ...). When a phase starts the worker asks `GET /traces/<phase>.json`; if the server has it, the worker fetches every
  range in it in parallel (5 connections, 1 MB requests, ordered by when the engine first needed them) *ahead of* the engine, which then finds its reads in the
  cache; if not (404), it records the phase's reads for 90 s and uploads them (`POST`, validated against the manifest, first upload wins; `?args=--learn`
  re-records and replaces). So the **first** load of a map on a server is as slow as before, and every load after it, from any client, is not. The host
  of a map learns it as a side effect of loading it. Traces only name files and byte ranges of your own game data (no content); delete
  `web/build/traces/` to forget them. Prefetching needs the persistent cache (nowhere to keep the bytes otherwise).
- **`cache: 'no-store'` on the worker's fetches.** Chrome's HTTP cache takes a per-URL lock that serialises parallel range requests to the same file:
  5 parallel 1 MB fetches ran at 11.8 MB/s with the default and 23-27 MB/s without it (simulated 50 Mbit/s per connection).
- Up to 6 connections (Chrome's per-origin limit for HTTP/1.1; prefetch leaves one free for the engine), engine reads before prefetch in the queue.
- Server: `TCP_NODELAY` (with keep-alive, Nagle holds back each response's last partial segment until the client's delayed ACK, 40-200 ms per request on a
  real network, invisible on loopback) and a longer accept backlog.
- The page shows a small *Downloading game data ... MB/s* badge while data arrives (the game's own loading screen cannot redraw while it blocks), and the
  worker's `[io]` lines go to the page log.
- Tools: `?args=--fsstats` logs requests / MB fetched vs wanted / time blocked per file every 5 s (`--fstrace` also lists the first `.iwd` reads);
  `serve.py --sim-rtt-ms N --sim-mbps N` simulates a slower network on localhost. Plain HTTP caching does not work for this: Chrome did not cache the 206 responses
  (a reload sent every request again).

## Layout
- `shim/`      Win32 / D3D9 / DirectInput / Winsock / MSVC-CRT compatibility headers + `win32_shim.cpp` (threads/events/critical
               sections over pthreads, overlapped reads) over `kvfs.cpp` (virtual files: POSIX under node; HTTP range
               requests + in-memory writes in the browser; Windows-path + case-insensitive handling).
- `audio/`     `al_web.cpp`: OpenAL-subset software mixer (own thread) feeding the page's AudioWorklet.
- `gfx/`       the D3D9→WebGL2 device (`d3d9_gl_device.cpp`: state, FBOs, programs, draws; `d3d9_gl_resources.cpp`: textures,
               buffers, queries, formats) and `shader_xlate.cpp`, the engine-independent D3D9 bytecode→GLSL translator
               (native test driver: `tools/xlate_test.cpp`).
- `platform/`  web replacements for `win_main.cpp` (`sys_web.cpp`) and stand-ins for excluded subsystems (`stubs_web.cpp`).
- `app/`       the browser entry point (`main.cpp`), HTML shell, I/O worker and audio worklet. `loadtest/` the node load test. `tools/` build/survey/link scripts + dev server.

## Gotchas found so far (all handled)
- wasm traps on mismatched `call_indirect` signatures; the decompiled code casts function pointers freely →
  `-sEMULATE_FUNCTION_POINTER_CASTS=1`.
- Retail text files are CRLF and the engine relies on MSVC text-mode translation → emulated in `kisak_fopen`.
- `CreateThread(CREATE_SUSPENDED)` / matching `GetCurrentThreadId()` ids are relied on by the database thread.
- Engine critical sections must be recursive (use the Win32 branch of `win_common.cpp`).
- Itanium ABI doesn't mangle return types → two same-named `static`-intended functions collided.
- `DB_LoadXAssets(..., sync=1)` holds the asset-hash write lock while the DB thread needs it → use sync=0 + `DB_SyncXAssets()`.

## Browser/WebGL gotchas
- A WebGL context belongs to one thread, but the engine creates GPU resources on its fastfile thread: resource creation, Lock
  and Unlock only touch CPU memory; GL objects are created lazily on first use by the engine's main thread, and destructors
  queue GL deletes for it. The engine's dedicated render thread is not used (`startedRenderThread = 0`, `sys_smp_allowed 0`).
- Frames only reach the screen when the worker yields, so the frame loop is `emscripten_set_main_loop` (one `Com_Frame` per rAF).
- WebGL never completes a query/fence in the task that issued it, and the engine spin-waits on them → `GetData` reports completion
  immediately (occlusion counts come back 0, so the sun-sprite calibration falls back to the low-quality path).
- D3D9→GL conventions fixed in the translated vertex shader: z [0,w]→[-w,w], y flip (render targets keep D3D row order,
  Present blits flipped), half-pixel offset. D3D alpha-test / sRGB-write render states are emulated in the pixel shader.
- The process "cwd" is the install root (`Sys_Cwd` is the engine's base path) and relative paths resolve against it.

## Engine/ABI bugs found while bringing up M2 (all fixed; none are wasm-specific logic bugs in the game)
- A `char` parameter's address was read as a `uint32_t` by a callback (`Dvar_InfoString`): fine on x86 where the stack slot is
  zero-extended, garbage in wasm → random dvars leaked into the server-info string and truncated it (empty `mapname`).
- `XAssetHeader` is a union passed by value; the wasm C ABI passes unions by reference, so a table entry that cast
  `Image_Free(GfxImage*)` to `void(*)(XAssetHeader)` received a pointer to the union (`DB_RemoveImage` wrapper).
- `scrVmGlob` landed directly after `scrVmPub` in memory, so the script VM's "eval stack" pointer (`&scrVmGlob - 1`) aliased
  `scrVmPub.maxstack` and every field read reported "Internal script stack overflow" (separator padding).
- `VirtualFree(MEM_DECOMMIT)` must not `free()` (the Hunk decommits ranges of one reservation).
- MSVC CRT semantics the engine relied on: `rand()` returns 0..32767 (the engine divides by 32767/32768/0x8000 and masks with 0x7FFF). The
  web libc returns up to 2^31, which made `random()` return values up to ~65536 and inflated weapon kick, bullet spread, effects and sound
  variation. `kisak_rand`/`kisak_srand` (win32_shim.cpp, macros in kisak_compat.h) reproduce MSVC's generator for the C++ engine code.
- `Lock()` memory must be 16-byte aligned: `R_LockSkinnedCache` rejects a misaligned lock and then skips model skinning for the frame
  (the viewmodel vanished on alternate frames, since there are two skin-cache buffers). `std::vector` only gives 8-byte alignment in
  wasm, so buffer/texture shadow memory uses a 64-byte aligned allocator (`Bytes` in `d3d9_gl_internal.h`).
- `build.py` has no header dependency scanning for engine files (use `--force` after touching `shim/*.h`); web-only sources are rebuilt
  automatically when any header under `web/` changes.
- WebGL needs all framebuffer attachments the same size; D3D9 allows a larger depth buffer (substitute depth renderbuffers).

## Platform-assumption audit (MSVC/x86 → clang/wasm32)
After the `rand()` bug, the whole engine was audited for other places where decompiled MSVC-era code silently assumes its original platform.
What was checked, and what came out of it:

| Check | Result |
|---|---|
| **Compiler semantics.** The code type-puns through casts, relies on wrapping signed arithmetic and dereferences before null-checking; MSVC never exploited that, clang does. | `build.py` now passes `-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks`. |
| **`long double`** is 64-bit on MSVC but a 128-bit software-float type on wasm (220 uses, mostly IDA `fp2`/`st7` temporaries; also changed struct sizes, e.g. `source_s`). | `-Xclang -mlong-double-64`; guarded in `platform/layout_guard.cpp`. `std::format` in a dev-only asset-dump command still produces three harmless `to_chars` link-signature warnings. |
| **`_snprintf`/`_vsnprintf`** return -1 on truncation on MSVC, the would-be length with C99. `va()` has `if (len >= 1024) Com_Error("va: too short")`, which never fires on MSVC (it truncates silently) but would have become a fatal error for any >1023 char string. | Legacy semantics emulated in `kisak_compat.h` (`snprintf` itself stays C99, as in MSVC 2015+). |
| **Struct layouts.** The headers carry the retail 32-bit size of ~1000 structs (`// sizeof=0x...`). | `python3 web/tools/layout_check.py` compiles a `static_assert` per struct: 15 differences, all explained (SP/MP duplicates, empty structs, ODE internals, unpacked `VoicePacket_t`, intentionally enlarged loopback buffers). The check is the audit's regression test. |
| **Hard-coded byte offsets** in decompiled code (`*(_DWORD*)&buf[8*n - 9596]`, `scale[i - 3]`, `serverHardwareIconList[40*n - 5119]`). | `SV_GetCachedSnapshotInternal` and `Outdoor_*` rewritten to use the real members; the UI map-list offsets are verified at compile time (`layout_guard.cpp`). |
| **Union-by-value callbacks** (`XAssetHeader`) reached through function-pointer casts. | Only two were wrong: `Image_Free` (M2) and `FX_SaveEffectDefTableEntry_FileLoadObj` (`fx_archive.cpp`, the effect-table save path). The technique-set handler takes the union by value too, so it is correct. |
| **Uninitialised variables** (`clang -Wuninitialized -Wsometimes-uninitialized -Wconditional-uninitialized` over all 444 TUs). | 32 warnings: all decompiler artifacts (loop counters returned as `result`, flags correlated with an assignment) except one `strlen()` of an unfilled CD-key buffer in `ui_main_mp.cpp` (initialised). |

Real upstream bug found on the way: `SV_SetClientStat`/`SV_GetClientStat` addressed the 32-bit stats (index ≥ 2000) as `voicePackets[17].data[4*index+75]`,
which only lands in `client_t::stats` for the retail packed `VoicePacket_t`; with the header's natural alignment it pointed 69 bytes before the slot and
overlapped the byte stats. They now index `stats[4*index - 5996]` directly (matters for the multiplayer server, M4).

Still open, deliberately: the `ui_main_mp.cpp` server-browser code has ~60 more raw-offset accesses into `sharedUiInfo`/`info.text` (only the map-list ones are
verified), `qsort` (~30 uses) is not stable and orders ties differently from MSVC's (the iwd-name sort has a total order; the runtime ones — player use list, scoreboard rank, touch-image list, server list — only see a different but valid order on ties; the material/BSP/anim sorts are in `*_load_obj` raw-asset paths that fastfile loading does not take), and `float→int` casts of out-of-range values saturate on wasm
instead of producing 0x80000000.
