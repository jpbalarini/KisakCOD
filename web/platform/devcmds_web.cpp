// devcmds_web.cpp -- developer console commands that only make sense for the web port.
//
//   dumprawfile <name>   print a rawfile asset (GSC script, .csv, .arena...) from the loaded fastfiles to the log between
//                        "[[rawfile NAME begin]]" / "[[rawfile NAME end]]" markers. Useful to read the game scripts the engine runs,
//                        e.g.  ?args=%2Bdevmap%20mp_crash%20%2Bdumprawfile%20maps/mp/gametypes/_globallogic.gsc
#include <universal/q_shared.h>
#include <Windows.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <database/database.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cmd_function_s s_dumpRawFile, s_gfxExp;
void KisakBots_RegisterCommands();   // bots_web.cpp
namespace d3dgl { void SetGfxExperiment(int); }
static void GfxExp_f()
{
    d3dgl::SetGfxExperiment(Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : 0);
}

static void DumpRawFile_f()
{
    if (Cmd_Argc() < 2) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "usage: dumprawfile <name>\n");
        return;
    }
    const char *name = Cmd_Argv(1);
    RawFile *rf = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, name).rawfile;
    if (!rf || !rf->buffer || strcmp(rf->name, name)) {
        Com_Printf(CON_CHANNEL_DONT_FILTER, "rawfile '%s' is not loaded\n", name);
        return;
    }
    fprintf(stderr, "[[rawfile %s begin]]\n", name);
    const char *p = rf->buffer;
    int left = rf->len;
    while (left > 0) {                       // line by line so the log buffer is never handed one giant string
        const char *nl = (const char *)memchr(p, '\n', left);
        int n = nl ? (int)(nl - p) + 1 : left;
        fwrite(p, 1, n, stderr);
        p += n; left -= n;
    }
    fprintf(stderr, "\n[[rawfile %s end]]\n", name);
}

void Web_RegisterCommands()
{
    Cmd_AddCommandInternal("dumprawfile", DumpRawFile_f, &s_dumpRawFile);
    KisakBots_RegisterCommands();   // addbot / kickbots (bots_web.cpp)
    Cmd_AddCommandInternal("gfxexp", GfxExp_f, &s_gfxExp);   // diagnostic: skip classes of GL work, see gfx/d3d9_gl_internal.h
}
