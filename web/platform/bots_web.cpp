// bots_web.cpp -- the engine side of "Bot Warfare" (github.com/ineedbots/iw3_bot_warfare), the GSC bot AI that runs on top of the stock test
// clients.
//
// The AI itself is script (web/bots/maps/mp/bots/*.gsc, unmodified upstream files). It was written for the CoD4x server, which gives scripts
// a handful of built-ins to drive a test client like a human player. This file provides the same built-ins for this engine:
//
//   <bot> botaction("+fire")      press / release a button: fire melee frag smoke reload sprint ads holdbreath gostand gocrouch goprone
//                                 leanleft leanright activate (names as in CoD4x; "+" holds it, "-" releases it)
//   <bot> botstop()               release every button, stop moving
//   <bot> botmovement(fwd, right) move relative to the view direction, -127..127 (what the usercmd carries)
//   <bot> botmoveto(origin)       CoD4x's "walk straight to a point"; accepted and ignored: our adapter drives botmovement instead
//   <bot> botmeleeparams(yaw, d)  auto-melee lunge toward yaw, d units away (0, 0 = none)
//   <bot> isbot()                 is this client a test client?
//   fs_testfile / fs_fopen / fs_readline / fs_writeline / fs_fclose   read files below <bots dir>/scriptdata (waypoints, bot names); read only
//   printconsole(text)            print to the server console even with developer_script off
//
// Views are not a built-in: the mod calls setplayerangles(), which already exists. Everything a bot "presses" is kept in BotCmd and replayed by
// SV_BotUserMove() (server_mp/sv_main_mp.cpp) as the bot's usercmd on every server frame. A test client that no script has touched keeps the
// stock behaviour (random input), so scr_testclients still works as before.
//
// The scripts are read from a host directory, never from the game data: KISAK_BOTS_DIR (the dedicated server sets it from --bots=DIR). Without
// it none of this is active. Two console commands drive the bots by hand:   addbot [count]   kickbots
#include <universal/q_shared.h>
#include <Windows.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/msg_mp.h>
#include <script/scr_vm.h>
#include <script/scr_parser.h>
#include <universal/com_memory.h>
#include <game_mp/g_public_mp.h>
#include <game_mp/g_main_mp.h>
#include <server/sv_game.h>
#include <server_mp/server_mp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace {

struct BotCmd {
    bool controlled;            // a bot script has driven this client: replay BotCmd instead of the stock random input
    int buttons;
    signed char fwd, right;
    float meleeYaw;
    unsigned char meleeDist;
};
BotCmd g_cmd[MAX_CLIENTS];

struct ButtonName { const char *name; int bit; };
const ButtonName kButtons[] = {
    {"fire", BUTTON_ATTACK}, {"attack", BUTTON_ATTACK}, {"melee", BUTTON_MELEE}, {"frag", BUTTON_FRAG}, {"smoke", BUTTON_SMOKE},
    {"reload", BUTTON_RELOAD}, {"sprint", BUTTON_SPRINT}, {"ads", BUTTON_ADS}, {"holdbreath", BUTTON_BREATH},
    {"gostand", BUTTON_JUMP}, {"gocrouch", BUTTON_CROUCH}, {"goprone", BUTTON_PRONE},
    {"leanleft", BUTTON_LEAN_LEFT}, {"leanright", BUTTON_LEAN_RIGHT}, {"activate", BUTTON_USE}, {"use", BUTTON_USE},
};

// ---- the bots directory ---------------------------------------------------------------------------------------------------------------

std::string g_dir;              // no trailing slash; empty = bots are off
bool g_dirResolved = false;

bool FileExists(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

const std::string &BotsDir()
{
    if (!g_dirResolved) {
        g_dirResolved = true;
        const char *e = getenv("KISAK_BOTS_DIR");
        if (e && *e) {
            g_dir = e;
            while (g_dir.size() > 1 && g_dir.back() == '/') g_dir.pop_back();
            if (!FileExists(g_dir + "/maps/mp/bots/_kisak.gsc")) {
                fprintf(stderr, "[bots] %s has no maps/mp/bots/_kisak.gsc: bots are off\n", g_dir.c_str());
                g_dir.clear();
            } else {
                fprintf(stderr, "[bots] scripts from %s\n", g_dir.c_str());
            }
        }
    }
    return g_dir;
}

// A script-supplied path must stay inside <dir>/scriptdata.
bool ScriptDataPath(const char *rel, std::string *out)
{
    const std::string &d = BotsDir();
    if (d.empty() || !rel) return false;
    std::string r = rel;
    if (r.compare(0, 11, "scriptdata/") != 0 || r.find("..") != std::string::npos || r.find('\\') != std::string::npos) return false;
    *out = d + "/" + r;
    return true;
}

// ---- script built-ins -----------------------------------------------------------------------------------------------------------------

int BotClient(scr_entref_t entref)
{
    if (entref.classnum) { Scr_ObjectError("not an entity"); return -1; }
    if (entref.entnum >= MAX_CLIENTS || !g_entities[entref.entnum].client) {
        Scr_ObjectError(va("entity %i is not a player", (int)entref.entnum));
        return -1;
    }
    return entref.entnum;
}

bool IsBotClient(int n)
{
    return n >= 0 && n < sv_maxclients->current.integer && svs.clients[n].header.state != CS_FREE &&
           svs.clients[n].header.netchan.remoteAddress.type == NA_BOT;
}

void __cdecl BotAction(scr_entref_t entref)
{
    int n = BotClient(entref);
    if (n < 0) return;
    const char *a = Scr_GetString(0);
    if (!a || (a[0] != '+' && a[0] != '-')) { Scr_ParamError(0, "botaction: expected \"+action\" or \"-action\""); return; }
    for (const ButtonName &b : kButtons) {
        if (!I_stricmp(a + 1, b.name)) {
            g_cmd[n].controlled = true;
            if (a[0] == '+') g_cmd[n].buttons |= b.bit; else g_cmd[n].buttons &= ~b.bit;
            return;
        }
    }
    static bool warned = false;
    if (!warned) { warned = true; Com_Printf(CON_CHANNEL_DONT_FILTER, "[bots] unknown botaction \"%s\" (ignored; shown once)\n", a); }
}

void __cdecl BotStop(scr_entref_t entref)
{
    int n = BotClient(entref);
    if (n < 0) return;
    memset(&g_cmd[n], 0, sizeof g_cmd[n]);
    g_cmd[n].controlled = true;
}

signed char Clamp127(int v) { return (signed char)(v > 127 ? 127 : v < -127 ? -127 : v); }

void __cdecl BotMovement(scr_entref_t entref)
{
    int n = BotClient(entref);
    if (n < 0) return;
    g_cmd[n].controlled = true;
    g_cmd[n].fwd = Clamp127((int)Scr_GetFloat(0));      // scripts pass ints or floats (the VM converts ints, but rejects floats in GetInt)
    g_cmd[n].right = Clamp127((int)Scr_GetFloat(1));
}

void __cdecl BotMoveTo(scr_entref_t entref)
{
    int n = BotClient(entref);
    if (n < 0) return;
    float where[3];
    Scr_GetVector(0, where);   // validates the argument; botmovement() does the walking
}

void __cdecl BotMeleeParams(scr_entref_t entref)
{
    int n = BotClient(entref);
    if (n < 0) return;
    float yaw = Scr_GetFloat(0);
    int dist = (int)Scr_GetFloat(1);
    g_cmd[n].controlled = true;
    g_cmd[n].meleeYaw = yaw;
    g_cmd[n].meleeDist = (unsigned char)(dist < 0 ? 0 : dist > 255 ? 255 : dist);
}

void __cdecl BotIsBot(scr_entref_t entref)
{
    if (entref.classnum || entref.entnum >= MAX_CLIENTS) { Scr_AddBool(0); return; }
    Scr_AddBool(IsBotClient(entref.entnum) ? 1 : 0);
}

void __cdecl PrintConsole()
{
    Com_Printf(CON_CHANNEL_DONT_FILTER, "%s\n", Scr_GetString(0));
}

// -- files (read only; handles 1..N) --
FILE *g_files[16];

void __cdecl FsTestFile()
{
    std::string p;
    Scr_AddBool(ScriptDataPath(Scr_GetString(0), &p) && FileExists(p) ? 1 : 0);
}

void __cdecl FsFOpen()
{
    std::string p;
    const char *mode = Scr_GetNumParam() > 1 ? Scr_GetString(1) : "read";
    if (I_stricmp(mode, "read") || !ScriptDataPath(Scr_GetString(0), &p)) { Scr_AddInt(0); return; }   // writing is not supported
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) { Scr_AddInt(0); return; }
    for (int i = 1; i < 16; ++i) if (!g_files[i]) { g_files[i] = f; Scr_AddInt(i); return; }
    fclose(f);
    Scr_AddInt(0);
}

void __cdecl FsFClose()
{
    int h = Scr_GetInt(0);
    if (h > 0 && h < 16 && g_files[h]) { fclose(g_files[h]); g_files[h] = nullptr; }
}

void __cdecl FsReadLine()
{
    int h = Scr_GetInt(0);
    static char line[2048];
    if (h > 0 && h < 16 && g_files[h] && fgets(line, sizeof line, g_files[h])) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        Scr_AddString(line);
        return;
    }
    Scr_AddUndefined();
}

void __cdecl FsWriteLine() {}   // read only

struct FuncDef { const char *name; void (__cdecl *fn)(); };
const FuncDef kFuncs[] = {
    {"printconsole", PrintConsole}, {"fs_testfile", FsTestFile}, {"fs_fopen", FsFOpen}, {"fs_fclose", FsFClose},
    {"fs_readline", FsReadLine}, {"fs_writeline", FsWriteLine},
};
struct MethodDef { const char *name; void (__cdecl *fn)(scr_entref_t); };
const MethodDef kMethods[] = {
    {"botaction", BotAction}, {"botstop", BotStop}, {"botmovement", BotMovement}, {"botmoveto", BotMoveTo},
    {"botmeleeparams", BotMeleeParams}, {"isbot", BotIsBot},
};

// ---- console commands -------------------------------------------------------------------------------------------------------------------

cmd_function_s s_addBot, s_kickBots;

void AddBot_f()
{
    if (BotsDir().empty()) { Com_Printf(CON_CHANNEL_DONT_FILTER, "bots are off: the server was started without the bot scripts (--bots=DIR)\n"); return; }
    if (!com_sv_running || !com_sv_running->current.enabled) { Com_Printf(CON_CHANNEL_DONT_FILTER, "addbot: no game is running\n"); return; }
    int count = Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : 1;
    if (count < 1) count = 1;
    // The mod polls this dvar, adds that many bots and resets it to 0.
    Dvar_SetIntByName("bots_manage_add", Dvar_GetInt("bots_manage_add") + count);
    Com_Printf(CON_CHANNEL_DONT_FILTER, "adding %d bot%s\n", count, count == 1 ? "" : "s");
}

void KickBots_f()
{
    if (!sv_maxclients || !svs.clients) return;
    int kicked = 0;
    for (int i = 0; i < sv_maxclients->current.integer; ++i) {
        client_t *cl = &svs.clients[i];
        if (cl->header.state != CS_FREE && cl->header.netchan.remoteAddress.type == NA_BOT) {
            SV_DropClient(cl, "EXE_PLAYERKICKED", true);
            ++kicked;
        }
    }
    Dvar_SetIntByName("bots_manage_add", 0);
    Com_Printf(CON_CHANNEL_DONT_FILTER, "kicked %d bot%s\n", kicked, kicked == 1 ? "" : "s");
}

} // namespace

// ---- hooks the engine calls (all under KISAK_WEB) -----------------------------------------------------------------------------------------

void KisakBots_RegisterCommands()
{
    Cmd_AddCommandInternal("addbot", AddBot_f, &s_addBot);
    Cmd_AddCommandInternal("kickbots", KickBots_f, &s_kickBots);
}

bool KisakBots_Enabled() { return !BotsDir().empty(); }

void KisakBots_ResetClient(int n)
{
    if (n >= 0 && n < MAX_CLIENTS) memset(&g_cmd[n], 0, sizeof g_cmd[n]);
}

// Fills the usercmd of a script-driven bot; false = no script has touched it (the caller keeps the stock random input).
bool KisakBots_FillCmd(int n, usercmd_s *cmd)
{
    if (n < 0 || n >= MAX_CLIENTS || !g_cmd[n].controlled) return false;
    const BotCmd &b = g_cmd[n];
    cmd->buttons = b.buttons;
    cmd->forwardmove = b.fwd;
    cmd->rightmove = b.right;
    cmd->meleeChargeYaw = b.meleeYaw;
    cmd->meleeChargeDist = b.meleeDist;
    return true;
}

void (__cdecl *KisakBots_GetFunction(const char **pName, int *type))()
{
    for (const FuncDef &f : kFuncs) if (!strcmp(*pName, f.name)) { *pName = f.name; *type = 0; return f.fn; }
    return nullptr;
}

void (__cdecl *KisakBots_GetMethod(const char **pName))(scr_entref_t)
{
    for (const MethodDef &m : kMethods) if (!strcmp(*pName, m.name)) { *pName = m.name; return m.fn; }
    return nullptr;
}

// Scr_ReadFile hook: a script that exists below the bots directory is read from there. nullptr = not ours, use the normal lookup.
char *KisakBots_ReadScript(const char *extFilename, const char *codePos, bool archive)
{
    const std::string &d = BotsDir();
    if (d.empty() || !extFilename || strstr(extFilename, "..")) return nullptr;
    FILE *f = fopen((d + "/" + extFilename).c_str(), "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return nullptr; }
    char *buf = (char *)Hunk_AllocateTempMemoryHigh((int)len + 1, "Scr_ReadFile");
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    Scr_AddSourceBufferInternal(extFilename, codePos, buf, (int)got, 1, archive);
    return buf;
}
