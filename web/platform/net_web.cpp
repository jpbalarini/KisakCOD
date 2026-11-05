// net_web.cpp -- UDP-over-WebSocket transport, replacing win_net.cpp (Winsock) in the browser build (M4).
//
// Browsers cannot send or receive UDP, so every engine "UDP packet" travels as one binary WebSocket frame to a small relay
// (web/tools/relay.py) which acts as a virtual LAN switch:
//
//   client -> relay : [dst ip: 4][dst port: 2, network order][payload]       (dst ip 255.255.255.255 = broadcast to everyone else)
//   relay -> client : [src ip: 4][src port: 2, network order][payload]       (src = the sender's virtual address)
//   relay -> client : text frame {"ip":"10.77.0.2","port":28960}             (once on connect: this tab's virtual address)
//
// The relay hands each tab a unique virtual IPv4 address (10.77.x.y, port 28960), so the engine sees ordinary NA_IP peers: one tab can run
// a listen server (`devmap`/`map`) and the others `connect 10.77.0.1` to it, server browser/LAN broadcast included. The unmodified
// netchan / challenge / snapshot code sits on top. The WebSocket lives in the engine's worker; events are delivered between frames
// (the browser frame loop yields), and Sys_GetPacket drains a small queue filled by the onmessage handler.
#include <universal/q_shared.h>
#include <Windows.h>
#include <win32/win_local.h>
#include <win32/win_net.h>
#include <qcommon/qcommon.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

static const dvar_s *net_relay;
void Web_RegisterCommands();   // devcmds_web.cpp

// ---- JS side: one WebSocket + an incoming packet queue, kept in globalThis.__kn of the engine worker ----

// Keep the connection to `url` alive (retry every 3 s); an empty url disables networking. Returns 0 = idle, 1 = connecting, 2 = up (virtual
// address assigned).
EM_JS(int, kn_service, (const char *urlPtr), {
    const S = (globalThis.__kn = globalThis.__kn || { ws: null, q: [], ip: null, port: 28960, url: null, retryAt: 0, dropped: 0 });
    const url = UTF8ToString(urlPtr);
    if (S.url !== url) {
        if (S.ws) { try { S.ws.close(); } catch (e) {} S.ws = null; }
        S.url = url; S.ip = null; S.q.length = 0; S.retryAt = 0;
    }
    if (!url) return 0;
    if (!S.ws && Date.now() >= S.retryAt) {
        S.retryAt = Date.now() + 3000;
        let ws = null;
        try { ws = new WebSocket(url); } catch (e) { ws = null; }
        if (ws) {
            ws.binaryType = "arraybuffer";
            S.ws = ws;
            ws.onmessage = function (ev) {
                if (typeof ev.data === "string") {
                    try {
                        const h = JSON.parse(ev.data);
                        if (h.ip) { S.ip = h.ip.split(".").map(Number); S.port = h.port | 0; }
                    } catch (e) {}
                    return;
                }
                const b = new Uint8Array(ev.data);
                if (b.length < 6) return;
                if (S.q.length >= 1024) { S.dropped++; return; }
                S.q.push(b);
            };
            ws.onclose = function () { if (S.ws === ws) { S.ws = null; S.ip = null; } };
            ws.onerror = function () {};
        }
    }
    return S.ip ? 2 : (S.ws ? 1 : 0);
});

// Pop one packet: payload -> dst, source address -> fromIp[4] / fromPort[2] (network order). Returns the payload length, -1 if the queue is
// empty, -2 if the packet did not fit in maxLen (dropped).
EM_JS(int, kn_recv, (uint8_t *dst, int maxLen, uint8_t *fromIp, uint8_t *fromPort), {
    const S = globalThis.__kn;
    if (!S || !S.q.length) return -1;
    const b = S.q.shift();
    const n = b.length - 6;
    if (n > maxLen) return -2;
    HEAPU8.set(b.subarray(0, 4), fromIp);
    HEAPU8[fromPort] = b[4];
    HEAPU8[fromPort + 1] = b[5];
    HEAPU8.set(b.subarray(6), dst);
    return n;
});

// Send one datagram to ip[4]:port[2] (network order). Returns 0 if the relay is not connected (the packet is lost, as UDP would).
EM_JS(int, kn_send, (const uint8_t *data, int len, const uint8_t *ip, const uint8_t *port), {
    const S = globalThis.__kn;
    if (!S || !S.ws || S.ws.readyState !== 1 || !S.ip) return 0;
    if (S.ws.bufferedAmount > (1 << 20)) return 0;
    const f = new Uint8Array(6 + len);
    f.set(HEAPU8.subarray(ip, ip + 4), 0);
    f[4] = HEAPU8[port];
    f[5] = HEAPU8[port + 1];
    f.set(HEAPU8.subarray(data, data + len), 6);
    S.ws.send(f);
    return 1;
});

EM_JS(void, kn_local_address, (uint8_t *ip, uint8_t *port), {
    const S = globalThis.__kn;
    if (!S || !S.ip) return;
    HEAPU8.set(S.ip, ip);
    HEAPU8[port] = (S.port >> 8) & 255;
    HEAPU8[port + 1] = S.port & 255;
});

// "auto" -> ws(s)://<host the page was served from>:8081/
EM_JS(void, kn_default_url, (char *out, int outLen), {
    const l = self.location;
    stringToUTF8((l.protocol === "https:" ? "wss://" : "ws://") + l.hostname + ":8081/", out, outLen);
});

// ---- engine-facing API (the pieces of win_net.cpp the engine calls) ----

static char s_url[256];
static int s_state = 0;
static bool s_warned = false;

static void NetService()
{
    if (!net_relay) return;
    const char *v = net_relay->current.string;
    if (!strcmp(v, "auto")) kn_default_url(s_url, sizeof s_url);
    else I_strncpyz(s_url, v, sizeof s_url);

    int st = kn_service(s_url);
    if (st != s_state) {
        if (st == 2) {
            netadr_t a; memset(&a, 0, sizeof a);
            kn_local_address(a.ip, (uint8_t *)&a.port);
            Com_Printf(CON_CHANNEL_SYSTEM, "Network: relay %s, local address %i.%i.%i.%i:%i\n", s_url, a.ip[0], a.ip[1], a.ip[2], a.ip[3],
                       (a.port & 0xFF) << 8 | (a.port >> 8));
            s_warned = false;
        } else if (st == 0 && s_state == 2) {
            Com_Printf(CON_CHANNEL_SYSTEM, "Network: relay connection lost, retrying\n");
        } else if (st == 0 && !s_warned && s_url[0]) {
            s_warned = true;
            Com_Printf(CON_CHANNEL_SYSTEM, "Network: relay %s is not reachable (is web/tools/serve.py running?), multiplayer is offline; retrying in the background\n", s_url);
        }
        s_state = st;
    }
}

void NET_Init(void)
{
    Web_RegisterCommands();   // NET_Init runs inside Com_Init after Cmd_Init but before the startup +commands execute
    net_relay = Dvar_RegisterString("net_relay", "auto", DVAR_ARCHIVE, "WebSocket relay used instead of UDP ('auto' = same host, port 8081; empty = offline)");
    NetService();
}

void NET_Shutdown(void) { kn_service(""); s_state = 0; }
void NET_Restart(void) { NetService(); }
void NET_Config(bool) { NetService(); }
void NET_Sleep(int msec) { Sleep(msec > 0 ? (DWORD)msec : 0); }
const char *NET_ErrorString(void) { return "network error"; }
uint32_t __cdecl NET_TCPIPSocket(const char *, int, int) { return (uint32_t)-1; }   // HTTP downloads are not supported in the web build

// Never called by the game logic, just the system event queueing (NET_GetClientPacket / NET_GetServerPacket).
qboolean Sys_GetPacket(netadr_t *net_from, msg_t *net_message)
{
    NetService();
    for (;;) {
        int n = kn_recv(net_message->data, net_message->maxsize, net_from->ip, (uint8_t *)&net_from->port);
        if (n == -1) return 0;
        if (n == -2) { Com_Printf(CON_CHANNEL_SYSTEM, "Oversize packet dropped\n"); continue; }
        net_from->type = NA_IP;
        memset(net_from->ipx, 0, sizeof net_from->ipx);
        net_message->readcount = 0;
        net_message->cursize = n;
        return 1;
    }
}

char __cdecl Sys_SendPacket(int length, unsigned __int8 *data, netadr_t to)
{
    uint8_t ip[4];
    switch (to.type) {
    case NA_BROADCAST: memset(ip, 255, 4); break;
    case NA_IP: memcpy(ip, to.ip, 4); break;
    case NA_LOOPBACK: case NA_BOT: return 1;
    default: Com_Error(ERR_FATAL, "Sys_SendPacket: bad address type"); return 0;
    }
    if (length > 0) kn_send(data, length, ip, (const uint8_t *)&to.port);
    return 1;
}

// Dotted quads only: there is no DNS in the virtual LAN. "localhost" is accepted for convenience.
qboolean Sys_StringToAdr(const char *s, netadr_t *a)
{
    memset(a, 0, sizeof *a);
    unsigned v[4];
    char tail;
    if (!strcmp(s, "localhost")) { a->ip[0] = 127; a->ip[3] = 1; a->type = NA_IP; return 1; }
    if (sscanf(s, "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4 || v[0] > 255 || v[1] > 255 || v[2] > 255 || v[3] > 255) {
        a->type = NA_BAD;
        return 0;
    }
    for (int i = 0; i < 4; ++i) a->ip[i] = (uint8_t)v[i];
    a->type = (v[0] == 255 && v[1] == 255 && v[2] == 255 && v[3] == 255) ? NA_BROADCAST : NA_IP;
    return 1;
}

bool Sys_IsLANAddress_IgnoreSubnet(netadr_t adr)
{
    if (adr.type == NA_LOOPBACK || adr.type == NA_BOT) return true;
    if (adr.type != NA_IP) return false;
    if (adr.ip[0] == 10 || adr.ip[0] == 127) return true;                       // includes the relay's 10.77.x.y virtual LAN
    if (adr.ip[0] == 169 && adr.ip[1] == 254) return true;
    if (adr.ip[0] == 172 && (adr.ip[1] & 0xF0) == 0x10) return true;
    return adr.ip[0] == 192 && adr.ip[1] == 168;
}

bool Sys_IsLANAddress(netadr_t adr) { return Sys_IsLANAddress_IgnoreSubnet(adr); }

void Sys_ShowIP()
{
    netadr_t a; memset(&a, 0, sizeof a);
    kn_local_address(a.ip, (uint8_t *)&a.port);
    if (a.ip[0]) Com_Printf(CON_CHANNEL_SYSTEM, "IP: %i.%i.%i.%i\n", a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
    else Com_Printf(CON_CHANNEL_SYSTEM, "IP: not connected to the relay\n");
}
