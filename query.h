#include <WinSock2.h>
#include <windows.h>

struct QueryServer {
    struct QueryServer* next;
    char hostname[128];
    char mapname[32];
    char gamemode[16];
    char modname[16];
    int playerCount;
    int maxPlayers;
    int ping;
    struct sockaddr_in queryAddress;
    LARGE_INTEGER pingSendTime;
    bool pingUpdated;
    bool serverInfoUpdated;
    bool playersUpdated;
};
struct QueryThreadParameters {
    HANDLE mutex;
    struct QueryServer* server;
};

unsigned int __stdcall QueryThreadMain(void* arg);
