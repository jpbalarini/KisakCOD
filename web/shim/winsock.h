// winsock.h shim -- maps the tiny slice of Winsock the engine uses onto BSD sockets.
// In the browser, emscripten's POSIX socket emulation (WebSocket-proxied) backs these; multiplayer
// transport is a later milestone, M0 just needs the types to exist.
#pragma once
#include <Windows.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
typedef struct sockaddr SOCKADDR, *LPSOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN, *LPSOCKADDR_IN;
typedef struct in_addr IN_ADDR;
typedef struct hostent HOSTENT;
typedef struct { WORD wVersion, wHighVersion; char szDescription[257]; char szSystemStatus[129]; unsigned short iMaxSockets, iMaxUdpDg; char *lpVendorInfo; } WSADATA, *LPWSADATA;
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINTR EINTR
#define WSAEMSGSIZE EMSGSIZE
#define WSAECONNRESET ECONNRESET
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#define WSAENETUNREACH ENETUNREACH
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAEINVAL EINVAL
#define WSANOTINITIALISED 10093
#define WSAHOST_NOT_FOUND 11001
#define WSAGetLastError() (errno)
#define closesocket(s) close(s)
#define ioctlsocket(s, cmd, arg) ioctl((s), (cmd), (arg))
static inline int WSAStartup(WORD ver, LPWSADATA d) { (void)ver; if (d) memset(d, 0, sizeof(*d)); return 0; }
static inline int WSACleanup(void) { return 0; }
