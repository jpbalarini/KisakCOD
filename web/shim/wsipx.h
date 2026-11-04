// wsipx.h shim -- IPX is dead; only the struct name is referenced.
#pragma once
#include <winsock.h>
struct sockaddr_ipx { short sa_family; char sa_netnum[4]; char sa_nodenum[6]; unsigned short sa_socket; };
#define NSPROTO_IPX 1000
