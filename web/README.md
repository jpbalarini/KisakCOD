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
  (`platform/input_web.cpp`), ~45 fps in game at the time (see Performance for the current numbers), 0 GL errors. A "Player" profile is pre-seeded so you skip the profile screen.
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
- **Dedicated server (done, local):** `serve.py --dedicated` runs the engine headless under node next to the web server and the relay, for a standalone machine (see "Dedicated server").
- **Next:** make it public (relay on a server, signed identities), persistence of
  profile/config, a warm shader list. Known rough edges: no reverb/EQ (EFX is stubbed), no HRTF/3D beyond left-right pan, the loading screen only redraws when the engine yields, depth-texture shadow maps use the colour
  fallback, occlusion queries report 0, Bink is a stub, profiles are saved per browser only (see Profiles), no gamepad, no voice chat.

## Build & run (needs emscripten, python3)
```bash
python3 web/tools/build.py --link app       # compile all engine TUs to wasm objects + link web/build/app/index.{html,js,wasm}
web/run.sh                                  # serves it + your game data on http://localhost:8080 (open in Chrome)
```
`web/tools/exclude.txt` lists the Windows-only source files that are replaced for the web. Other tools:
```bash
python3 web/tools/build.py --link loadtest  # the node load test: link web/build/loadtest/loadtest.js instead of the browser build
python3 web/tools/build.py --link dedicated # the headless dedicated server (web/build/dedicated/dedicated.js, runs under node): see "Dedicated server"
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
**Quit** (menu -> Quit -> Yes, or the `quit` command) tears the engine down and reloads the page back to this start screen (`Sys_Quit` in
`platform/sys_web.cpp` stops the frame loop and calls `kisakGameExited()` in `app/shell.html`). Before, `exit(0)` left the browser's frame loop running
against the freed UI and the log filled with `Could not find menu 'main'` forever.

**Fullscreen.** The start screen has a *Fullscreen* option next to Start (remembered, on by default): Start then enters the browser's real fullscreen (Fullscreen API on `#wrap`,
so the browser's toolbar and tabs disappear, unlike the browser's own "enter full screen" menu item, which keeps them on a Mac). In the game: **F11** or **Alt+Enter**
(Option+Enter on a Mac) toggles it, and so does the button at the top right that shows while the mouse moves. Esc is locked to the page while fullscreen (keyboard
lock, Chrome/Edge), so it still opens the in-game menu and leaving fullscreen takes a long press of Esc; elsewhere a single Esc leaves it. The page has no log panel any
more (`?log` shows it under the game; the text is always in `#log`); while the engine starts, its latest log line shows at the bottom left.
The picture has no black bars: `app/shell.html` sizes the canvas to the largest rectangle of its shape that fits the *stage* (the whole window / screen) and reports the
stage's size to the engine through shared memory (`kisakViewAttach`, and `--view=WxH` for the very first renderer). `platform/view_web.cpp` makes `R_SetWndParms`
(`kisak_fit_display`, a `KISAK_WEB` patch in `src/gfx_d3d/r_init.cpp`) reshape the resolution `r_mode` asks for to the stage's aspect ratio, **keeping the pixel count** (Options >
Video > Resolution stays the quality setting; e.g. 1024x768 -> 1118x702 on a 16:10 screen; aspect clamped to 4:3..21:9; an explicit `r_customMode` wins). When the stage's
shape changes later (fullscreen on/off, resizing) the engine runs `vid_restart` by itself, **only from the main menu**: the engine refuses it while this tab hosts a game
("Listen server cannot video restart"), and in a match it would reload the map's assets. Elsewhere the canvas is letterboxed to the old shape until the next menu visit.
`web/platform/view_web.cpp` logs `[view]` lines. Not tested in a real fullscreen Chrome window (the built-in pane refuses fullscreen): the Start screen falls back to windowed after 2 s there.

Controls: WASD / mouse / click as usual; Esc opens the in-game menu (the browser swallows Esc while the pointer is locked, so the
loss of pointer lock is turned back into an Esc press). The pointer is only locked while the player controls the game: not while a menu, the
console or a script menu has the cursor (those follow the *absolute* pointer position, which a locked pointer stops reporting, so the menu cursor
froze: clicking a team button right after joining used to lock it, because the client already counts as "in game" then). It is released when a menu
opens and re-requested when it closes (the browser performs that at the next click or key press). `--no-profile` shows the profile screen instead of seeding one.
The UI text fields and the console delete with the Backspace *character* (Windows sends it as `WM_CHAR` 8 and ignores the key event), so `platform/input_web.cpp`
sends that character on Backspace too; before, Backspace did nothing in the profile-name box.
URL options: `?args=--nosound` (skip the audio mixer), `?args=--gldebug` (check GL errors after every draw), `?args=--trace-fs`, and any engine command line, e.g.
`?args=%2Bset%20developer%200`. The dev server (`web/tools/serve.py`) adds the COOP/COEP headers that wasm threads need
and serves the install dir read-only under `/data/` (never copied or uploaded).


## Performance
The first browser builds ran at 12-20 fps on an M1 and 12-60 fps on an M5 Max (menus ~12, fire / smoke / shooting worst). Nothing was wrong with the engine
code: the time went into the browser's **GPU process**, which executes every GL call the engine thread issues (WebGL commands are asynchronous, but a
single thread's worth of ANGLE work per frame). Same scene (mp_crash, 1,620 draws, sun shadow + float-z passes), M5 Max, 1024x768:
29 fps -> 113 fps with the profiler on and `com_maxfps 0`, and vsync-limited (120 fps) with the profiler off; main menu 12 -> 88 fps (the engine's `com_maxfps 85` cap).
Measured on an M5 Max only: no M1 was available, expect lower absolute numbers there.

**What was wrong, in order of impact**
1. **Index-buffer updates between draws** (`gfx/d3d9_gl_resources.cpp`, `IndexBuffer::resolve`). The engine writes every batch of the dynamic index ring as
   Lock / memcpy / Unlock / DrawIndexedPrimitive (~80 per frame in the test scene, more with particle effects). Doing that as
   `glBufferSubData` into one big `GL_ELEMENT_ARRAY_BUFFER` costs **200-450 us per call** in Chrome on macOS, whatever the size (measured with a stand-alone
   WebGL microbenchmark: 1000 draws with a 300-byte update each = 455 ms/frame into a 2 MB ring, 198 ms into a 16 KB one, 9-15 ms when every update goes to a
   different small buffer). Fix: a D3DUSAGE_DYNAMIC index buffer only keeps its CPU copy; at draw time the bytes the draw reads are uploaded (the whole pending
   dirty range when it covers them) into the next of 256 pooled GL buffers with `glBufferData`, and the segment is remembered for further draws.
   This alone was 28 -> 86 fps (under the engine's 85 fps cap). Effect-heavy views use more batches, which fits them being the worst case. `--noidxstream` restores the old behaviour for comparison.
2. **Whole-buffer uploads**: the skinned-model cache (4.5 MB), the pre-tessellated world index buffer (2 MB) and the code mesh (0.5 MB) are locked in full every frame
   but only a prefix is filled, so ~7.5 MB per frame went through the command buffer. The three engine call sites (`r_model.cpp`, `r_pretess.cpp`, `r_drawsurf.cpp`,
   `#ifdef KISAK_WEB`) now call `kisak_buffer_written(buffer, usedBytes)` before Unlock (declared in `shim/d3d9.h`) so only the used part is uploaded (7.5 -> 0.9 MB).
3. **A blocking present**: Emscripten's `emscripten_webgl_commit_frame()` (renderViaOffscreenBackBuffer) blitted a second time inside `gl.getParameter()` calls, which are
   synchronous round trips that wait for the GPU process to drain the whole frame (20-60 ms stalls in "gl:commit"). The context now renders straight to the
   OffscreenCanvas (our own backbuffer FBO is blitted to the default framebuffer, the browser presents it when the frame task ends).
4. **Redundant GL calls**: every draw re-bound every texture (plus a scratch-unit bind in `TexCore::ensureGL`), every sampler, every vertex attribute (bind buffer + pointer + enable)
   and the constant buffers. Now: bind caches for textures / samplers / array + element buffers (`g_bind`, `BindBufferCached`), a per-location vertex attribute cache, constants
   that did not change are not re-sent (`TrimUnchanged`), clean buffers/textures return without touching GL, the two constant buffers stay bound to their own targets.
   (texture binds 2,500 -> 1,080 per frame, vertex attribute setup 3,400 -> 2,800 calls plus the redundant buffer binds / enables per attribute.) Worth little on its own once 1-3
   were fixed, but it keeps the GPU process lighter, which should matter most on slower Macs.
5. **Build flags**: the engine was compiled at `-O1` and linked with `-sASSERTIONS=1`, which links Emscripten's *debug* libc / libc++ / dlmalloc (`-DDLMALLOC_DEBUG`:
   a heap consistency check on every malloc / free). Now `-O2` objects, a `-O2` link and assertions off (JS glue 364 KB -> 171 KB, wasm 5.7 -> 4.7 MB). `KISAK_OPT=O1`,
   `KISAK_LINK_OPT=O1`, `KISAK_ASSERTIONS=1` (and `build.py --force`) bring the old behaviour back for debugging.

**What is still expensive (not changed)**: the scene is drawn up to three times: the sun shadow map (~390 draws), the **float-z pass** (~470 draws, only when a visible
particle material is soft-edged, i.e. fire / smoke; `r_zFeather 0` removes it and the soft edges, `+set r_zFeather 0` in `?args=`, latched at material load) and the
main scene (~760). `+set sm_enable 0` drops the sun shadow pass at any time. Draw calls are the cost unit in this browser path (~5-15 us each in the GPU process),
not triangles or pixels (3 indices per draw and a 1x1 viewport left the frame time unchanged).

**Profiling** (`?args=--prof`, add `+set com_maxfps 0` to see the uncapped frame time): the page log gets a `[prof]` report every 4 s with the heaviest zones (every engine
`PROF_SCOPED` span, plus `gl:*` spans of the D3D -> GL layer), per-frame counters (draws, state changes, uploads, draws / triangles / GPU ms per render target: `dr[1024x768 f21]`
is the main scene, `f114` = R32F: shadow map 1024x2048 and float-z 1024x768), a per-buffer upload table, and a `HITCH` line for every frame over 60 ms (`--prof-hitch=N`).
`--prof-sync` adds a `glFinish` per frame (`gl:finish(gpu wait)`). The profiler costs ~3 ms per frame itself (a timer pair per zone), so compare runs with it on or off,
not across. `web/shim/web_prof.h` / `platform/web_prof.cpp`. The console command `gfxexp <mask>` skips a class of GL work to measure it (1 = draws, 2 = constants, 4 = attribute
pointers, 8 = texture binds, 16 / 256 / 512 / 1024 / 2048 = buffer uploads (all / index / vertex / >= 2 MB / < 2 MB), 32 = 3 indices per draw, 64 = 1x1 viewport).
`tools/pane_console.js` (paste it into the page) drives the console from JavaScript (`await typeCmd('/setviewpos 440 300 165 85')`) and reads the report (`profLast()`), which is how
the numbers above were taken: noclip to mp_crash's burning helicopter (fires at ~(430..870, 380..650, 150)), same view for every run.

**Known remaining hitches**: the first use of a shader pair links a GL program synchronously (~3-4 ms each; ~60 at once when a map starts, 10-50 ms stalls when a new effect first
appears). `KHR_parallel_shader_compile` plus a recorded warm list of (vertex, pixel) shader pairs per map would remove these. A server snapshot after a teleport can take 300+ ms.

## Profiles (saved in the browser)
The engine's player profiles are files under `<install root>/players/`: `profiles/active.txt` (the profile in use), and per profile `config_mp.cfg`
(settings, keybinds, sensitivity) and `mpdata` (rank, unlocks, custom classes, challenges). `shim/kvfs.cpp` keeps engine-written files in memory and now
also mirrors everything under `players/` to the I/O worker (`app/io-worker.js`), which stores it in **IndexedDB** (database `kisak-profile`, store `files`,
records `{path (lower case key), orig (with the capitals), dir, data, mtime}`). At start-up `kvfs::init` asks the worker for the saved files before the
engine looks for profiles, so a reload (or a new visit) comes back with the same profile list, settings and stats; the seeded `Player` profile is only
created on a first run. Each save happens when the engine closes a written file (or deletes / creates a directory), so a tab closed mid-write can lose
that one write. Every save logs `[kvfs] saved players/...` in the page log.
- **Several profiles:** the game's own Select Profile / Create / Delete menus work and are saved. With more than one profile the game asks which one to
  use at start-up (CoD4's own behaviour). Names keep their capitals (the in-memory listing used to lower-case everything).
- **Per browser, per origin:** nothing is shared between browsers, devices or `localhost` vs `127.0.0.1`; clearing site data deletes it. Two tabs of one
  browser each load the profile at their start and the last one to save wins. Use **Export / Import** on the start screen to back a profile up or move it:
  Export downloads `cod4-web-profile-<date>.json` (all saved files, base64); Import merges such a file into the saved profiles (same paths are
  overwritten) and only accepts plain paths below `players/`; it takes effect the next time you press Start.
- **`?args=--nopersist`** neither restores nor saves (the profile then only lives in memory, as before); handy for tests.
- **Engine fixes this needed:** the engine creates and removes profile directories with `_mkdir` / `_rmdir` / `remove()`, which in the browser went to
  Emscripten's own file system instead of kvfs, so a "created" profile never really existed (it only appeared in the menu) and deleting one always failed.
  `kisak_compat.h` now routes `_mkdir` / `_rmdir` to kvfs (`kisak_mkdir` / `kisak_rmdir` in `win32_shim.cpp`), and `com_files.cpp` / `win_common.cpp` get a
  `#ifdef KISAK_WEB` block that does the same for `remove()`. kvfs ignores a trailing slash on directory names.

## Multiplayer
`web/run.sh` (i.e. `tools/serve.py`) also starts the relay on `ws://localhost:8081/` (`--relay-port 0` disables it; `tools/relay.py` runs it standalone).
Every tab that opens the game connects to it and is assigned a virtual address, printed in the log as `Network: relay ..., local address 10.77.0.N:28960`
(the first tab is `10.77.0.1`).

- **Host:** open `http://localhost:8080/`, choose **Start New Server** in the main menu, pick a map and press Start (or skip the menus with
  `?args=%2Bdevmap%20mp_crash`). It is a listen server: the hosting tab is the server, so keep it open (a hidden tab keeps serving through a
  worker-timer watchdog in `app/main.cpp`, since `requestAnimationFrame` stops in hidden tabs: ~25 fps there, versus 90+ when visible, so
  keep the host's window visible if you can).
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
    installed; a self-signed certificate you click through is *not* a secure context),
  - or, when players cannot install a root CA (Windows machines, phones), a real certificate for a domain of yours that points at the server's
    LAN address: [Trusted HTTPS for everyone on the LAN](#trusted-https-for-everyone-on-the-lan-your-own-domain--lets-encrypt), or
  - in Chrome on the client open `chrome://flags/#unsafely-treat-insecure-origin-as-secure`, add `http://192.168.x.y:8080`, relaunch.
  `--lan` serves your game data to everyone who can reach the port: use it on a network you trust, not through a public tunnel. A phone also lacks
  touch controls (keyboard + mouse only). Another computer without these steps can still reach it through `ssh -N -L 8080:localhost:8080
  -L 8081:localhost:8081 <this-mac>`: its browser then sees `localhost`.
- **Query a server** (works from anything that can speak WebSocket, e.g. a script): `getinfo`/`getstatus` out-of-band packets, see `tools/relay.py`'s
  `SimpleClient`. LAN broadcast (`255.255.255.255`) is forwarded to every other tab by the relay; that is what the in-game LAN browser sends.

### Trusted HTTPS for everyone on the LAN (your own domain + Let's Encrypt)
mkcert makes every client install its root CA first, which is a chore on Windows and phones. A certificate from a public CA needs nothing on the
client, but it is issued for a *name*, to whoever controls that name's DNS. So: use a domain you own, point a name under it at the server's
**private** LAN address, and prove ownership with a DNS challenge (DNS-01: a TXT record, no inbound connection from the internet). The server never
becomes reachable from outside; only players on that network can resolve to something they can reach. The steps below use `cod.example.com` and a
server at `192.168.1.125`; replace both.

1. **DNS record.** In the domain's DNS zone add an `A` record `cod.example.com` -> `192.168.1.125`, **DNS only**: no proxy/CDN in front (Cloudflare's
   orange cloud, CloudFront, a load balancer), the name must resolve straight to the private address. Give the server a fixed LAN address (a DHCP
   reservation in the router) so the record stays right.
2. **Certificate (Let's Encrypt, DNS-01).** Any ACME client works; this one is certbot with the Route 53 plugin (AWS is used for DNS only, see the note
   below). The domain can be registered anywhere as long as its zone is in Route 53 (or use another provider's certbot plugin, e.g.
   `certbot-dns-cloudflare`).
   - Create an IAM user for certbot that can change records in that one zone, so a leaked key can do little else:
     ```json
     { "Version": "2012-10-17", "Statement": [
       { "Effect": "Allow", "Action": ["route53:ListHostedZones", "route53:GetChange"], "Resource": "*" },
       { "Effect": "Allow", "Action": ["route53:ChangeResourceRecordSets"], "Resource": "arn:aws:route53:::hostedzone/<YOUR-ZONE-ID>" } ] }
     ```
     Give certbot its access key with `aws configure` or the environment variables `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`. Keep the key out
     of the repository, the bundle and chat.
   - Install and issue it (the three `--*-dir` options keep everything under your home directory, so no `sudo`; without them certbot writes to
     `/etc/letsencrypt`, which only root can read, and `serve.py` would need root to load the key):
     ```bash
     python3 -m venv ~/certbot-venv && ~/certbot-venv/bin/pip install certbot certbot-dns-route53
     ~/certbot-venv/bin/certbot certonly --dns-route53 -d cod.example.com \
         --config-dir ~/letsencrypt --work-dir ~/letsencrypt/work --logs-dir ~/letsencrypt/logs
     ```
     It asks once for an e-mail address and to accept the Let's Encrypt terms, adds the `_acme-challenge` TXT record, waits for the check and removes
     it. The result is `~/letsencrypt/live/cod.example.com/fullchain.pem` and `privkey.pem` (copy them elsewhere if you like: they are plain PEM files).
   - A DNS host without a certbot plugin works too, by hand: `certbot certonly --manual --preferred-challenges dns -d cod.example.com` tells you which
     TXT record to add. Renewals are then manual as well.
3. **Run it** with the certificate and the name:
   ```bash
   web/run.sh --dedicated --lan \
       --tls-cert ~/letsencrypt/live/cod.example.com/fullchain.pem \
       --tls-key  ~/letsencrypt/live/cod.example.com/privkey.pem \
       --allow-origin cod.example.com
   ```
   `--allow-origin` is required: with `--lan` the relay accepts pages from localhost, `*.local` and private IP *literals* only (it never resolves a public
   name, which would be a DNS-rebinding hole), so a domain has to be listed explicitly. Without it the relay answers `403` and prints
   `[relay] refused origin https://cod.example.com:8080`; the game page still loads, but its engine cannot reach the relay, so no server is listed.
   Players open **`https://cod.example.com:8080/`** (by the name: the certificate does not cover the IP address) and use **Join Game**. Ports 8080 (page and
   game data) and 8081 (relay, `wss://`) must be reachable on the server (allow `python3` through the macOS firewall prompt, or open both in `ufw`).
4. **Renew.** The certificate lasts 90 days and `serve.py` reads the files only at start, so renew and restart, e.g. weekly from cron/launchd:
   `~/certbot-venv/bin/certbot renew --quiet --config-dir ~/letsencrypt --work-dir ~/letsencrypt/work --logs-dir ~/letsencrypt/logs --deploy-hook "<command that restarts serve.py>"`
   (`renew` does nothing until the certificate is within 30 days of expiry; the hook runs only when it was renewed).

Troubleshooting and caveats:
- **The name does not resolve on the LAN** (`dig +short cod.example.com` / `nslookup` shows nothing): many routers and DNS filters (dnsmasq's
  `stop-dns-rebind`, OpenWrt, FRITZ!Box, pfSense, Pi-hole) drop public answers that point at private addresses ("DNS rebinding protection"). Add the
  domain to the router's exception list, or give the LAN's DNS an override for it.
- **The page opens but the game will not start:** the browser does not trust the certificate (opened by IP, name mismatch, expired). The padlock
  must be clean; check `https://cod.example.com:8080/` and the expiry date.
- **Keep `privkey.pem` private** (`chmod 600`): anyone with it can impersonate the name. `package_server.py` never includes it.
- **AWS is for DNS and the IAM user only.** Do not run the game server or serve the game data from EC2, CloudFront or a load balancer: the data is
  commercial and this project never hosts it publicly (`--lan` is for networks you trust). AWS Certificate Manager is no substitute: its public
  certificates cannot be exported, so `serve.py` could not load one.
- The record exposes the server's private address in public DNS (harmless, but visible). Players outside the network get an address they cannot
  reach; for friends elsewhere use a VPN (Tailscale, WireGuard) and the same name.

### Ping / latency
The scoreboard ping is the server's own measure (`SV_CalcPings`): the time from sending a snapshot until the first usercmd packet that acknowledges
it arrives, averaged over the last 32 snapshots. Every engine "packet" here crosses the browser's event loop and a TCP stream, so it contains:
the transport (Wi-Fi + TCP + the relay), the time a packet waits in the browser queue until the engine's next frame polls it (`Com_EventLoop`
runs once or twice per frame, so ~half a frame on each side), and the engine's reaction. Healthy numbers measured on one machine (network ~0,
both tabs ~90 fps): host 10 ms (loopback), remote client 13-20 ms. A LAN game with pings of 200+ is not expected and means one of those parts
is large. To find which:
- `web/run.sh --net-stats` (relay): pings every connected tab once a second and prints, every 5 s, `rtt avg/max/min` and packet rates per tab
  (`[relay] 10.77.0.2 (192.168.1.23): rtt avg 6.2 ms max 41.0 ms ...`). That is the transport plus how long the tab's event loop was busy:
  a high *max* with a low avg = stalls (Wi-Fi retransmissions: TCP holds back every later packet until a lost one is resent; macOS AWDL /
  AirDrop channel hopping; power saving), a high avg = a congested link (a game-data download on the same Wi-Fi, bufferbloat) or a slow link.
  Seconds-long values = the page was frozen (map load).
- `?args=%2Bset%20net_stats%201` (in the page, also `net_stats 1` in the console): `[net]` line in the page log (`?log`) every 5 s: this tab's
  frame period (avg/max), packets/s, **queue wait** (how long received packets sat before the engine polled them) and **reaction** (engine time
  from receiving a packet to sending the next one) and **wire rtt** (once a second the tab sends a timestamped datagram to `127.0.0.1`, which
  the relay loops straight back: Wi-Fi + TCP + relay, measured in the WebSocket message handler, so it needs no relay terminal; it includes up
  to one frame of waiting for the page's event loop, so ~9 ms on localhost at 90 fps is the floor). Queue wait of tens of ms = the tab runs
  slowly/hitches (slow GPU, hidden tab, shader compiles); ~2 ms = fine. `wire rtt` max/`>100 ms` count high = the link stalls; `NO REPLY` = nothing
  came back for 5 s.
- Gotchas: the scoreboard ping only measures the *freshest* snapshot a client acknowledges, so a client that polls slowly (a hidden tab at 6.7 fps)
  still shows a low ping while its game state is up to 150 ms stale: judge by `net_stats` frame period, not by the scoreboard alone. A tab that
  is hidden/occluded/minimised has no `requestAnimationFrame`; the watchdog drives it (before 2026-11-06 at 6.7 fps, now ~25 fps; `?args=--sim-hidden`
  reproduces that mode in a visible tab). The client sends one packet per rendered frame (~90/s): the engine treats the relay's 10.x addresses as
  a LAN and skips `cl_maxpackets`.

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

## Dedicated server (no browser)
A headless game server for a machine of its own (another Mac, a Linux mini-PC): the same wasm32 engine, run under **node** with `dedicated 2`, so no
window, GPU, UI or sound, and the game code, scripts and netcode behave as on a listen server. Players still join from their browsers.
`web/tools/serve.py --dedicated` starts all three parts from one command:

| part | what it does |
|---|---|
| `tools/serve.py` | the web server: the game page and your game data (read-only); starts and supervises the other two |
| `tools/relay.py` | the virtual LAN. The game server takes the reserved address **`10.77.0.1`**: only a loopback connection that asks for `?role=server` can have it, so it is the same address on every start and a restarted server takes it back over |
| `build/dedicated/dedicated.js` | the game server under node (`dedicated/main.cpp`): the real `Com_Init`/`Com_Frame`, console on stdin, a frame sleeps ~1 ms like the stock dedicated loop |

```bash
python3 web/tools/build.py --link app && python3 web/tools/build.py --link dedicated     # dedicated.js + .wasm in web/build/dedicated (no GL/audio sources)
web/run.sh --dedicated --lan [--tls-cert cert.pem --tls-key key.pem]                      # or: python3 web/tools/serve.py --data "<CoD4 Data dir>" --dedicated
python3 web/tools/probe.py [getinfo] [--relay host:8081 --tls]                            # is it up? hostname, map, players (exit status 0 = answered)
python3 web/tools/package_server.py                                                       # web/build/kisak-server.tar.gz for the other machine (below)
```
- **Needs** node 22 or newer (its global `WebSocket` is the relay transport) and python3. The machine that serves does not need emscripten.
  Players open `http://<machine>:8080/` and use **Join Game** (source Local lists it as "KisakCoD4 web server"), or `connect 10.77.0.1`.
  `--lan` and the secure-origin requirement are the same as for a hosting tab (see Multiplayer): give the server a certificate the players trust
  (for a LAN with Windows players the easiest is a domain + Let's Encrypt certificate, [steps here](#trusted-https-for-everyone-on-the-lan-your-own-domain--lets-encrypt)).
  With TLS the game server itself connects to the relay through a plain loopback-only listener, so it never needs the certificate.
- **Settings:** `dedicated/server.cfg` (name, slots, game type, scores, map rotation; it is run as `exec server.cfg` before the first map).
  `--dedicated-cfg FILE` picks another one, `--dedicated-args "+set sv_hostname Foo +set g_gametype dm"` adds engine arguments (they win over the file).
  Without `+map`/`+devmap` the server starts the config's `map_rotate` (with `--dedicated-cfg ''`, i.e. no config, it loads `mp_crash`).
- **Console:** serve.py's own stdin: `status`, `map mp_backlot`, `map_rotate`, `kick <name>`, `say <text>`, `quit` (`quit` stops the game server for good). The
  console shows the interesting lines (`[dedi]` prefix); `build/dedicated/server.log` keeps everything (`--dedicated-verbose` shows all).
- **Supervision:** a crash restarts the game server (after 2 s, doubling to 30 s; `--no-dedicated-restart` turns it off). If serve.py dies, even by `kill -9`,
  the game server follows (its stdin closes), so none is left running. `dedicated.js` can also be run by hand:
  `KISAK_INSTALL_PATH=... node --stack-size=7000 web/build/dedicated/dedicated.js [--relay=ws://host:8081/?role=server] [--cfg=FILE] [+engine args]`.
- **The data directory is never written** (`KISAK_MEMWRITE`, set by `dedicated.js`): kvfs reads from the real files and keeps everything the engine writes (config,
  profile, logs) in memory, copying a file up only when it is opened for update. The child's working directory is `build/dedicated/home`.
- **Moving it to the other machine:** `package_server.py` makes a tarball with the scripts, both wasm builds, `server.cfg`, the load traces and a `run-server.sh` /
  `README.txt`; it contains **no game data**: copy your own CoD4 install over (this project never ships or hosts it, and `--lan` serves it to everyone who can reach
  the port: private networks only). On the machine: `tar xzf kisak-server.tar.gz && cd kisak-server && ./run-server.sh --data "<CoD4 Data dir>"`.
- **Limits:** players have no real identity (the Steam shim accepts the page's random `KWEB` ticket, so bans are by that id only); `sv_pure 0` (no .iwd checksum check);
  `dedicated 2` is the default but 1 and 2 behave alike here (no heartbeat/master server). Developed and tested on macOS; Linux is expected to work (nothing in it is
  macOS-specific) but has not been tried yet. A server-side `Sys_Error` ends the process (exit code 1) and the supervisor restarts it.
- Gotchas: `exit()` hangs under node (a static destructor waits for a worker), so `Sys_Quit`/`Sys_Error` call `process.exit()` directly there (`ExitProcess` in
  `platform/sys_web.cpp`); `EM_JS` bodies must not contain backslashes (the preprocessor stringifies them); `stringToNewUTF8` is not linked, use a caller buffer +
  `stringToUTF8`; a browser engine worker has no `document` (detect node with `process.versions.node`, not by the lack of a DOM).

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
- `app/`       the browser entry point (`main.cpp`), HTML shell, I/O worker and audio worklet. `loadtest/` the node load test. `dedicated/` the headless server (`main.cpp`, `server.cfg`). `tools/` build/survey/link scripts + dev server.

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

## Joining a game crashed in the material sort (fixed)
Symptom: a client joining a LAN game sat on the loading screen forever; its page log had `worker sent an error ... RuntimeError: memory access out of bounds` and the stack ended in
`Material_Compare` called from `std::sort` (`Material_SortInternal`, run by `R_BeginFrame` once the world is set). The host never crashed and the crash came and went with
unrelated changes (printing something before the sort hid it), which is the signature of an uninitialised read. Cause: a decompilation bug in `R_RegisterShaderConst`
(`gfx_d3d/r_material_load_obj.cpp`): its insertion loop compared `*(&consts->count + sortedIndex + 1)`, a 32-bit read stepping from the `count` field, instead of the 16-bit
`consts->dest[sortedIndex - 1]`, i.e. it compared against uninitialised stack. The pixel literal constants of a material came out in a random order, `Material_Compare` (which
compares them) stopped being a strict weak ordering (it answered `a<b` and `b<a` for a pair of materials), and libc++'s `std::sort` walked past the end of the array. What the
stack held depended on what ran earlier, so any change that moves stack contents (new frame layout, profiler zones, build flags) could expose it, and it only showed on a joining
client. Fixed in the loop (a plain ascending insertion). How it was found: forwarding the worker's stack to the page log (a `self.addEventListener('error', ...)` that posts
`{cmd: 9, handler: 'printErr', args: [stack]}` to the main thread, prepended to `build/app/index.js`: workers only report `message`, no stack), replaying `std::sort` on a copy of the
array with a comparator that validated its arguments (it was handed `0`, `0x9d1`, `0x27082a38`), then checking `comp(a,b) && comp(b,a)` for the pairs it saw.
Test it with two tabs: host `?autostart&args=%2Bdevmap%20mp_crash`, join from `http://127.0.0.1:8080/?autostart&args=%2Bconnect%2010.77.0.N` (N = the host's address in its log; the
127.0.0.1 origin has its own cache, so it behaves like another computer).

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
