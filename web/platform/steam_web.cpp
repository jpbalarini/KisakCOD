// steam_web.cpp -- identity/ticket stand-in for win_steam.cpp in the browser build.
//
// The engine authenticates a connecting client with a Steam auth ticket (sent in `getchallenge`) and uses the SteamID64 as the player's GUID
// (bans, stats). There is no Steam in a browser, so each page load generates a random SteamID64-shaped identity and a ticket that is just
// "KWEB" + that id. The server accepts a ticket whose id matches the claimed SteamID64.
//
// NOTE: this proves nothing about who the player is (anyone can claim any id). It only keeps the connect handshake of the unmodified netcode
// working. A public deployment needs the relay (or a login service) to sign identities and the server to verify them (see web/README.md).
#include <universal/q_shared.h>
#include <Windows.h>
#include <win32/win_local.h>
#include <win32/win_steam.h>
#include <qcommon/qcommon.h>
#include <emscripten.h>
#include <string.h>

bool g_steamInitialized = false;
static uint64_t s_id = 0;
static unsigned char s_ticket[16];
static uint32 s_ticketSize = 0;

EM_JS(double, kn_random32, (), { return crypto.getRandomValues(new Uint32Array(1))[0]; });

void Steam_Init()
{
    if (g_steamInitialized) return;
    // 76561197960265728 = 0x0110000100000000: public universe, individual account type, instance 1; low 32 bits = account id
    s_id = 0x0110000100000000ull + (uint32_t)kn_random32();
    memcpy(s_ticket, "KWEB", 4);
    memcpy(s_ticket + 4, &s_id, 8);
    memset(s_ticket + 12, 0xA5, 4);
    s_ticketSize = sizeof s_ticket;
    g_steamInitialized = true;
}

void Steam_Shutdown() { g_steamInitialized = false; }

bool Steam_UpdateClientAuthTicket(netadr_t) { Steam_Init(); return true; }

bool Steam_GetRawClientTicket(unsigned char **pBuffer, uint32 *pSize)
{
    Steam_Init();
    *pBuffer = s_ticket;
    *pSize = s_ticketSize;
    return true;
}

void Steam_CancelClientTicket() {}

uint64_t Steam_GetClientSteamID64() { Steam_Init(); return s_id; }

bool Steam_CheckClientTicket(const void *pAuthTicket, uint32 authTicketLen, uint64_t steamID64)
{
    if (authTicketLen < 12 || memcmp(pAuthTicket, "KWEB", 4)) return false;
    uint64_t id;
    memcpy(&id, (const unsigned char *)pAuthTicket + 4, 8);
    return id == steamID64;
}

void Steam_CheckClients() {}
void Steam_OnClientDropped(uint64_t) {}
void Steam_SV_AddTestCommands() {}
