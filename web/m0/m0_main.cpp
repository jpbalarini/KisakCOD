// M0 harness: boot the real KisakCOD core (dvars, cmd, filesystem, memory, database thread) under node/wasm32
// and load the stock fastfiles from the Mac retail data with the *unmodified* engine loader.
//
//   KISAK_INSTALL_PATH=".../Call of Duty 4 Data" node web/build/m0/m0.js [zone ...]
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
#include <database/database.h>
#include <win32/win_local.h>
#include <win32/win_localize.h>
#include <stdio.h>
#include <emscripten/heap.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_gfx.h>

#define STEP(name) do { fprintf(stderr, "[m0] %s\n", name); fflush(stderr); } while (0)

extern char sys_cmdline[1024];
void Sys_FindInfo();

// engine entry points used by Com_Init_Try_Block_Function that are not exposed through a header we include
void Com_InitDvars();
void Com_InitXAssets();
void __cdecl Com_ParseCommandLine(char *commandLine);
void SL_Init();
void Swap_Init();
void Cbuf_Init();
void Cmd_Init();
void __cdecl Com_StartupVariable(const char *match);
void CCS_InitConstantConfigStrings();
void PMem_Init();
void PMem_BeginAlloc(const char *name, unsigned int allocType);
void PMem_EndAlloc(const char *name, unsigned int allocType);
void FS_InitFilesystem();
void Con_InitChannels();
void Com_InitHunkMemory();
void Hunk_InitDebugMemory();
char **__cdecl Sys_ListFiles(const char *directory, const char *extension, const char *filter, int *numfiles, int wantsubs);

static const char *kTypeNames[ASSET_TYPE_COUNT] = {
    "xmodelpieces","physpreset","xanimparts","xmodel","material","techset","image","sound","soundcurve","loadedsound",
    "clipmap","clipmap_pvs","comworld","gameworld_sp","gameworld_mp","map_ents","gfxworld","lightdef","ui_map","font",
    "menulist","menu","localize","weapon","snddriver","fx","impactfx","aitype","mptype","character","xmodelalias","rawfile","stringtable" };

struct EnumCtx { XAssetType type; int count; int shown; };
static void __cdecl EnumCb(XAssetHeader h, void *ud)
{
    EnumCtx *c = (EnumCtx *)ud;
    if (c->shown < 2) {
        const char *nm = DB_GetXAssetHeaderName(c->type, &h);
        fprintf(stderr, "[m0]      e.g. %s '%s'\n", kTypeNames[c->type], nm ? nm : "?");
        c->shown++;
    }
    c->count++;
}

static void ReportAssets(const char *title)
{
    fprintf(stderr, "[m0] ===== asset report: %s (heap %.1f MB) =====\n", title, emscripten_get_heap_size() / 1048576.0);
    int total = 0;
    for (int t = 0; t < ASSET_TYPE_COUNT; ++t) {
        EnumCtx c = { (XAssetType)t, 0, 0 };
        DB_EnumXAssets((XAssetType)t, EnumCb, &c, 0);
        if (c.count) fprintf(stderr, "[m0]   %-13s %6d\n", kTypeNames[t], c.count);
        total += c.count;
    }
    fprintf(stderr, "[m0]   total assets   %6d\n", total);
}

// ---- shader bytecode survey (M1 design input): what do the Mac fastfiles carry inside techsets?
#include <set>
#include <map>
#include <gfx_d3d/r_material.h>
struct ShaderSurvey {
    std::set<const void *> vs, ps; int techsets = 0, techniques = 0, passes = 0;
    std::map<unsigned, int> vsVer, psVer, vsRenderer, psRenderer; unsigned maxVsWords = 0, maxPsWords = 0; unsigned vsNull = 0, psNull = 0;
};
static void __cdecl TechsetCb(XAssetHeader h, void *ud)
{
    ShaderSurvey *s = (ShaderSurvey *)ud;
    MaterialTechniqueSet *ts = h.techniqueSet;
    s->techsets++;
    for (int i = 0; i < TECHNIQUE_COUNT; ++i) {
        MaterialTechnique *t = ts->techniques[i];
        if (!t) continue;
        s->techniques++;
        for (int p = 0; p < t->passCount; ++p) {
            MaterialPass *pass = &t->passArray[p];
            s->passes++;
            if (pass->vertexShader && s->vs.insert(pass->vertexShader).second) {
                auto &ld = pass->vertexShader->prog.loadDef;
                if (!ld.program) { s->vsNull++; continue; }
                s->vsVer[*(unsigned *)ld.program]++; s->vsRenderer[ld.loadForRenderer]++;
                if (ld.programSize > s->maxVsWords) s->maxVsWords = ld.programSize;
            }
            if (pass->pixelShader && s->ps.insert(pass->pixelShader).second) {
                auto &ld = pass->pixelShader->prog.loadDef;
                if (!ld.program) { s->psNull++; continue; }
                s->psVer[*(unsigned *)ld.program]++; s->psRenderer[ld.loadForRenderer]++;
                if (ld.programSize > s->maxPsWords) s->maxPsWords = ld.programSize;
            }
        }
    }
}
static void ShaderSurveyRun()
{
    ShaderSurvey s;
    DB_EnumXAssets(ASSET_TYPE_TECHNIQUE_SET, TechsetCb, &s, 0);
    fprintf(stderr, "[m0] shader survey: techsets=%d techniques=%d passes=%d uniqueVS=%zu uniquePS=%zu nullVS=%u nullPS=%u\n",
            s.techsets, s.techniques, s.passes, s.vs.size(), s.ps.size(), s.vsNull, s.psNull);
    for (auto &kv : s.vsVer) fprintf(stderr, "[m0]   VS version token %08x : %d\n", kv.first, kv.second);
    for (auto &kv : s.psVer) fprintf(stderr, "[m0]   PS version token %08x : %d\n", kv.first, kv.second);
    for (auto &kv : s.vsRenderer) fprintf(stderr, "[m0]   VS loadForRenderer %u : %d\n", kv.first, kv.second);
    for (auto &kv : s.psRenderer) fprintf(stderr, "[m0]   PS loadForRenderer %u : %d\n", kv.first, kv.second);
    fprintf(stderr, "[m0]   max VS words=%u PS words=%u\n", s.maxVsWords, s.maxPsWords);
    if (const char *dir = getenv("SHADER_DUMP_DIR")) {
        int n = 0;
        for (const void *p : s.vs) { auto *v = (const MaterialVertexShader *)p; char fn[512];
            snprintf(fn, sizeof fn, "%s/vs_r%u_%04d_%s.bin", dir, v->prog.loadDef.loadForRenderer, n, v->name);
            if (FILE *f = fopen(fn, "wb")) { fwrite(v->prog.loadDef.program, 4, v->prog.loadDef.programSize, f); fclose(f); ++n; } }
        for (const void *p : s.ps) { auto *v = (const MaterialPixelShader *)p; char fn[512];
            snprintf(fn, sizeof fn, "%s/ps_r%u_%04d_%s.bin", dir, v->prog.loadDef.loadForRenderer, n, v->name);
            if (FILE *f = fopen(fn, "wb")) { fwrite(v->prog.loadDef.program, 4, v->prog.loadDef.programSize, f); fclose(f); ++n; } }
        fprintf(stderr, "[m0] dumped %d shaders to %s\n", n, dir);
    }
    if (!s.vs.empty()) {
        auto *v = (MaterialVertexShader *)*s.vs.begin();
        const unsigned *w = (const unsigned *)v->prog.loadDef.program;
        fprintf(stderr, "[m0]   first VS '%s' words=%u:", v->name, v->prog.loadDef.programSize);
        for (int i = 0; i < 12 && i < v->prog.loadDef.programSize; ++i) fprintf(stderr, " %08x", w[i]);
        fprintf(stderr, "\n");
    }
}

static void CheckContents()
{
    // Spot-check real data through the engine's own lookup path (only names we know exist: DB_IsXAssetDefault
    // leaks the hash read-lock when asked about an asset that is not in the table).
    const char *raws[] = { "maps/mp/gametypes/dm.gsc", "maps/mp/_utility.gsc" };
    for (const char *name : raws) {
        RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, name).rawfile;
        fprintf(stderr, "[m0] rawfile '%s': len=%d first bytes: '%.40s'\n", name, rf->len, rf->buffer);
    }
    LocalizeEntry *le = DB_FindXAssetHeader(ASSET_TYPE_LOCALIZE_ENTRY, "MENU_MULTIPLAYER").localize;
    fprintf(stderr, "[m0] localize MENU_MULTIPLAYER = '%s'\n", le->value);
}

static void __cdecl GfxWorldCb(XAssetHeader h, void *)
{
    GfxWorld *gw = h.gfxWorld;
    fprintf(stderr, "[m0] GfxWorld '%s': surfaces=%d vertices=%u indices=%d planes=%d nodes=%d reflectionProbes=%u\n",
            gw->name, gw->surfaceCount, gw->vertexCount, gw->indexCount, gw->planeCount, gw->nodeCount, gw->reflectionProbeCount);
}
static void __cdecl ClipMapCb(XAssetHeader h, void *)
{
    clipMap_t *cm = h.clipMap;
    fprintf(stderr, "[m0] clipMap '%s': planes=%d staticModels=%u materials=%u brushSides=%u\n",
            cm->name, cm->planeCount, cm->numStaticModels, cm->numMaterials, cm->numBrushSides);
}
static void CheckMap(const char *)
{
    DB_EnumXAssets(ASSET_TYPE_GFXWORLD, GfxWorldCb, nullptr, 0);
    DB_EnumXAssets(ASSET_TYPE_CLIPMAP, ClipMapCb, nullptr, 0);
}

static void LoadZones(const char *const *extra, int extraCount)
{
    XZoneInfo zi[8] = {};
    int n = 0;
    zi[n++] = { "code_post_gfx_mp", DB_ZONE_CODE, 0 };
    zi[n++] = { "localized_code_post_gfx_mp", DB_ZONE_CODE_LOC, 0 };
    zi[n++] = { "ui_mp", DB_ZONE_GAME, 0 };
    zi[n++] = { "common_mp", DB_ZONE_COMMON, 0 };
    zi[n++] = { "localized_common_mp", DB_ZONE_COMMON_LOC, 0 };
    for (int i = 0; i < extraCount && n < 8; ++i) zi[n++] = { extra[i], DB_ZONE_GAME, 0 };
    fprintf(stderr, "[m0] DB_LoadXAssets(%d zones)\n", n);
    DB_LoadXAssets(zi, n, 0);   // same as R_LoadGraphicsAssets: async request ...
    DB_SyncXAssets();           // ... then wait for the database thread (sync=1 would hold the hash write-lock)
}

#include <thread>
#include <chrono>
static void StartWatchdog()
{
    const char *e = getenv("M0_WATCHDOG_SEC");
    if (!e) return;
    int sec = atoi(e);
    std::thread([sec] { std::this_thread::sleep_for(std::chrono::seconds(sec)); fprintf(stderr, "[m0] watchdog fired after %d s\n", sec); fflush(stderr); exit(3); }).detach();
}

int main(int argc, char **argv)
{
    StartWatchdog();
    setvbuf(stdout, nullptr, _IONBF, 0);
    STEP("Sys_InitializeCriticalSections");
    Sys_InitializeCriticalSections();
    STEP("Sys_InitMainThread");
    Sys_InitMainThread();
    STEP("track_init");
    track_init();

    STEP("Com_InitParse / Dvar_Init / InitTiming / Sys_FindInfo");
    Com_InitParse();
    Dvar_Init();
    InitTiming();
    Sys_FindInfo();
    Sys_Milliseconds();
    Profile_Init();
    Profile_InitContext(0);

    STEP("Win_InitLocalization");
    Win_InitLocalization();

    fprintf(stderr, "[m0] bring-up ok (language=%s)\n", Win_GetLanguage());

    // --- trimmed Com_Init_Try_Block_Function: everything the fastfile loader needs, nothing that needs a window ---
    STEP("Com_ParseCommandLine");
    sys_cmdline[0] = 0;
    Com_ParseCommandLine(sys_cmdline);
    STEP("SL_Init / Swap_Init / Cbuf_Init / Cmd_Init");
    SL_Init();
    Swap_Init();
    Cbuf_Init();
    Cmd_Init();
    Com_StartupVariable(0);
    STEP("Com_InitDvars");
    Com_InitDvars();
    CCS_InitConstantConfigStrings();
    STEP("PMem_Init / DB_SetInitializing");
    PMem_Init();
    DB_SetInitializing(1);
    PMem_BeginAlloc("$init", 1u);
    STEP("Com_InitXAssets (spawn database thread)");
    Com_InitXAssets();
    {   // sanity-check the directory listing shim against the engine's own Sys_ListFiles
        char dir[512]; snprintf(dir, sizeof(dir), "%s/main", getenv("KISAK_INSTALL_PATH"));
        int n = -1; char **l = Sys_ListFiles(dir, "iwd", nullptr, &n, 0);
        fprintf(stderr, "[m0] Sys_ListFiles(main, iwd) -> %d files (first: %s)\n", n, l && l[0] ? l[0] : "-");
        n = -1; l = Sys_ListFiles(dir, "/", nullptr, &n, 1);
        fprintf(stderr, "[m0] Sys_ListFiles(main, dirs) -> %d\n", n);
    }
    {   // is the zone file visible from the main thread / from a worker thread?
        char p[600]; snprintf(p, sizeof(p), "%s\\zone\\english\\code_post_gfx_mp.ff", getenv("KISAK_INSTALL_PATH"));
        HANDLE h = CreateFileA(p, 0x80000000, 1, 0, 3, 0x60000000u, 0);
        fprintf(stderr, "[m0] main-thread CreateFileA(zone) -> %s (GetFileSize=%lu)\n", h == INVALID_HANDLE_VALUE ? "INVALID" : "ok", h == INVALID_HANDLE_VALUE ? 0ul : GetFileSize(h, 0));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    STEP("FS_InitFilesystem");
    FS_InitFilesystem();
    Con_InitChannels();
    STEP("Com_InitHunkMemory");
    Com_InitHunkMemory();
    Hunk_InitDebugMemory();
    PMem_EndAlloc("$init", 1u);
    DB_SetInitializing(0);

    STEP("load zones");
    LoadZones(argc > 1 ? argv + 1 : nullptr, argc > 1 ? argc - 1 : 0);
    fprintf(stderr, "[m0] startup zones loaded (hash lock r=%d w=%d)\n", (int)db_hashCritSect.readCount, (int)db_hashCritSect.writeCount);
    ReportAssets("startup zones (code, ui, common, localized)");
    fprintf(stderr, "[m0] after ReportAssets (hash lock r=%d w=%d)\n", (int)db_hashCritSect.readCount, (int)db_hashCritSect.writeCount);
    CheckContents();
    ShaderSurveyRun();
    fprintf(stderr, "[m0] after CheckContents (hash lock r=%d w=%d)\n", (int)db_hashCritSect.readCount, (int)db_hashCritSect.writeCount);

    // Load a multiplayer map the way SV_SpawnServer does: "<map>_load" (loading-screen zone) then the map zone itself.
    const char *map = "mp_crash";
    {
        char loadName[64]; snprintf(loadName, sizeof(loadName), "%s_load", map);
        XZoneInfo zl = { loadName, DB_ZONE_LOAD, 0 };   // the game also frees LOAD|DEV; unloading needs the renderer (D3D objects), see M1
        DB_ResetZoneSize(0);
        STEP("load map loading-screen zone");
        DB_LoadXAssets(&zl, 1, 0);
        DB_SyncXAssets();
        XZoneInfo zm = { map, DB_ZONE_GAME, 0 };         // the game passes freeFlags=DB_ZONE_GAME (unloads ui_mp + previous map) - M1
        STEP("load map zone");
        unsigned t0 = Sys_Milliseconds();
        DB_LoadXAssets(&zm, 1, 0);
        DB_SyncXAssets();
        fprintf(stderr, "[m0] map zone loaded in %u ms\n", Sys_Milliseconds() - t0);
    }
    ReportAssets("after mp_crash");
    CheckMap(map);
    fprintf(stderr, "[m0] M0 COMPLETE\n");
    return 0;
}
