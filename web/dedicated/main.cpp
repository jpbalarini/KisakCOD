// Dedicated server entry point: the real engine (Com_Init / Com_Frame) with `dedicated 2`, run headless under node -- no browser, no window, no
// GPU. It is the same wasm32 build of the engine that runs in the browser, so the game code, scripts and netcode behave exactly like a listen
// server, minus the renderer / UI / sound.
//
//   KISAK_INSTALL_PATH=".../Call of Duty 4 Data" node --stack-size=7000 web/build/dedicated/dedicated.js \
//       --relay=ws://127.0.0.1:8081/?role=server --cfg=web/dedicated/server.cfg +map_rotate
//   (web/tools/serve.py --dedicated starts this together with the web server and the relay)
//
// Options (everything else is handed to the engine as command-line arguments, e.g. `+map mp_crash`, `+set sv_hostname "x"`):
//   --data=DIR     the CoD4 install directory (default: $KISAK_INSTALL_PATH). It is only ever READ: everything the engine writes stays in memory.
//   --relay=URL    the WebSocket relay that stands in for UDP (web/tools/relay.py); default ws://127.0.0.1:8081/?role=server
//   --cfg=FILE     a server config (host path), run as `exec server.cfg` before the other commands
//   --bots=DIR     the Bot Warfare scripts (web/bots; platform/bots_web.cpp). Without it the server has no bot support. It never adds a bot by
//                  itself: type `addbot [count]` / `kickbots` on the console, or set bots_manage_fill in the config.
// The console is stdin: type engine commands (`status`, `map mp_crash`, `kick <name>`, `say hi`, `quit`).
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
#include <win32/win_steam.h>
#include <win32/win_storage.h>
#include <win32/win_localize.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <string>
#include <emscripten.h>
#include "../shim/kvfs.h"

extern char sys_cmdline[1024];
void Sys_FindInfo();
extern bool g_webQuitting;   // platform/sys_web.cpp: Sys_Quit ran (quit command)

// ---- node side: stdin as a line queue, and reading a host file ----

EM_JS(void, kd_stdin_start, (), {
    const S = (globalThis.__kd = globalThis.__kd || { lines: [], buf: '', started: false, eof: false });
    if (S.started) return;
    S.started = true;
    if (typeof process === 'undefined' || !process.stdin) return;
    try {
        process.stdin.setEncoding('utf8');
        // (no backslashes in an EM_JS body: the preprocessor stringifies it and mangles them outside string literals)
        const LF = String.fromCharCode(10), CR = 13;
        process.stdin.on('data', (d) => {
            S.buf += d;
            let i;
            while ((i = S.buf.indexOf(LF)) >= 0) {
                let line = S.buf.slice(0, i);
                if (line.length && line.charCodeAt(line.length - 1) === CR) line = line.slice(0, -1);
                S.lines.push(line);
                S.buf = S.buf.slice(i + 1);
            }
        });
        process.stdin.on('end', () => { S.eof = true; });
        process.stdin.on('error', () => { S.eof = true; });
    } catch (e) { S.eof = true; }
});

// Pops one stdin line into out (0 = none queued).
EM_JS(int, kd_stdin_line, (char *out, int outLen), {
    const S = globalThis.__kd;
    if (!S || !S.lines.length) return 0;
    stringToUTF8(S.lines.shift(), out, outLen);
    return 1;
});

// Has stdin been closed? (A supervisor that dies closes the pipe: KISAK_EXIT_ON_STDIN_EOF makes the server stop with it.)
EM_JS(int, kd_stdin_eof, (), { const S = globalThis.__kd; return S && S.eof ? 1 : 0; });

// Reads a host file into dst (at most cap bytes). Returns its length, -1 if it cannot be read, -2 if it does not fit.
EM_JS(int, kd_read_host_file, (const char *path, char *dst, int cap), {
    try {
        const b = require('fs').readFileSync(UTF8ToString(path));
        if (b.length > cap) return -2;
        HEAPU8.set(b, dst);
        return b.length;
    } catch (e) { console.error('[dedicated] ' + e.message); return -1; }
});

// ---- console ----

static void PumpConsole()
{
    char line[1024];
    while (kd_stdin_line(line, sizeof line)) {
        size_t n = strlen(line);
        if (!n) continue;
        char *b = (char *)Com_AllocEvent((int)n + 1);
        I_strncpyz(b, line, (int)n + 1);
        Sys_QueEvent(0, SE_CONSOLE, 0, 0, (int)n + 1, b);   // Com_EventLoop hands it to the command buffer
    }
}

static bool g_exitOnEof = false;

static void FrameCb(void *)
{
    if (g_webQuitting) return;
    PumpConsole();
    static bool quitSent = false;
    if (g_exitOnEof && !quitSent && kd_stdin_eof()) {
        fprintf(stderr, "[dedicated] stdin closed (the supervisor is gone): shutting down\n");
        Cbuf_AddText(0, "quit\n");
        quitSent = true;
    }
    Com_Frame();
}

static bool InstallConfig(const char *hostPath, const std::string &root)
{
    static char text[256 * 1024];
    int n = kd_read_host_file(hostPath, text, (int)sizeof text);
    if (n < 0) { fprintf(stderr, "[dedicated] cannot read config %s%s\n", hostPath, n == -2 ? " (over 256 KB)" : ""); return false; }
    // Appears as <install>/main/server.cfg to the engine (in memory: the data directory is not touched).
    int h = kvfs::open(root + "/main/server.cfg", O_WRONLY | O_CREAT | O_TRUNC);
    if (h < 0) { fprintf(stderr, "[dedicated] cannot create server.cfg in memory\n"); return false; }
    kvfs::write(h, text, (size_t)n);
    kvfs::close(h);
    fprintf(stderr, "[dedicated] config %s -> exec server.cfg\n", hostPath);
    return true;
}

static void Append(char *cmd, size_t cap, const char *s)
{
    if (*cmd) strncat(cmd, " ", cap - strlen(cmd) - 1);
    strncat(cmd, s, cap - strlen(cmd) - 1);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // The engine prints every line twice: to stderr (everything) and, through Sys_Print, to stdout (filtered). A server log wants it once.
    if (!freopen("/dev/null", "w", stdout)) fprintf(stderr, "[dedicated] could not silence stdout\n");

    std::string dataDir = getenv("KISAK_INSTALL_PATH") ? getenv("KISAK_INSTALL_PATH") : "";
    std::string relay = "ws://127.0.0.1:8081/?role=server", cfg;
    std::string extra;   // engine arguments given on our command line
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--data=", 7)) { dataDir = argv[i] + 7; continue; }
        if (!strncmp(argv[i], "--relay=", 8)) { relay = argv[i] + 8; continue; }
        if (!strncmp(argv[i], "--cfg=", 6)) { cfg = argv[i] + 6; continue; }
        if (!strncmp(argv[i], "--bots=", 7)) { if (argv[i][7]) setenv("KISAK_BOTS_DIR", argv[i] + 7, 1); continue; }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr, "usage: dedicated.js [--data=DIR] [--relay=ws://host:8081/?role=server] [--cfg=server.cfg] [--bots=DIR] [+engine args ...]\n");
            return 0;
        }
        extra += extra.empty() ? "" : " ";
        // keep arguments with spaces as one token (the engine's tokenizer understands quotes)
        if (strchr(argv[i], ' ') && argv[i][0] != '"') extra += std::string("\"") + argv[i] + "\"";
        else extra += argv[i];
    }
    while (dataDir.size() > 1 && dataDir.back() == '/') dataDir.pop_back();
    if (dataDir.empty()) {
        fprintf(stderr, "[dedicated] no game data: set KISAK_INSTALL_PATH or pass --data=\"/path/to/Call of Duty 4 Data\"\n");
        return 2;
    }
    setenv("KISAK_INSTALL_PATH", dataDir.c_str(), 1);
    setenv("KISAK_MEMWRITE", "1", 1);   // everything the engine writes stays in memory
    kvfs::init(dataDir.c_str());

    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    track_init();
    Win_InitLocalization();
    Com_InitParse();
    Dvar_Init();
    InitTiming();
    Sys_FindInfo();

    // Defaults (our own arguments come after and can override them):
    //  - dedicated 2: a dedicated "internet" server (no client, no renderer, no UI; the script-visible `dedicated` dvar is 2);
    //  - sv_pure 0: web servers do not verify clients' .iwd checksums (see app/main.cpp);
    //  - sv_connectTimeout / cl_connectTimeout: a joining browser loads the map before it answers, which can take minutes (see app/main.cpp);
    //  - net_relay: where the UDP-over-WebSocket relay is. `//` starts a comment in the command tokenizer, hence the quotes.
    char cmd[1024] = "";
    //  - developer 0 / developer_script 0: this build defaults them on, which prints a snapshot dump per client and a debug line per player ping.
    Append(cmd, sizeof cmd, "+set dedicated 2 +set sv_pure 0 +set com_introPlayed 1 +set developer 0 +set developer_script 0"
                            " +set sv_connectTimeout 1800 +set cl_connectTimeout 3600");
    Append(cmd, sizeof cmd, (std::string("+set net_relay \"") + relay + "\"").c_str());
    bool haveCfg = !cfg.empty() && InstallConfig(cfg.c_str(), dataDir);
    if (haveCfg) Append(cmd, sizeof cmd, "+exec server.cfg");
    // A server with no map has nobody to play with: with a config, start its map rotation; without one, mp_crash.
    if (extra.find("+map") == std::string::npos && extra.find("+devmap") == std::string::npos)
        Append(cmd, sizeof cmd, haveCfg ? "+map_rotate" : "+map mp_crash");
    if (!extra.empty()) Append(cmd, sizeof cmd, extra.c_str());
    I_strncpyz(sys_cmdline, cmd, 1024);
    fprintf(stderr, "[dedicated] data: %s\n[dedicated] relay: %s\n[dedicated] engine args: %s\n", dataDir.c_str(), relay.c_str(), sys_cmdline);

    Sys_Milliseconds();
    Profile_Init();
    Profile_InitContext(0);
    Steam_Init();   // identity shim (platform/steam_web.cpp): the connect handshake checks a "KWEB" ticket
    kd_stdin_start();
    g_exitOnEof = getenv("KISAK_EXIT_ON_STDIN_EOF") != nullptr;
    fprintf(stderr, "[dedicated] Com_Init...\n");
    Com_Init(sys_cmdline);
    fprintf(stderr, "[dedicated] Com_Init done, entering frame loop (type engine commands here; `quit` stops the server)\n");
    // One Com_Frame per event-loop turn: the frame itself sleeps ~1 ms (the stock dedicated loop), and WebSocket packets from the relay are
    // delivered between frames.
    emscripten_set_main_loop_arg(FrameCb, nullptr, 0, 0);
    return 0;
}
