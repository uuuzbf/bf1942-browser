#include <WinSock2.h>
#include <windows.h>


struct QueryPlayer {
    WCHAR name[32];
    int score;
    int ping;
};

struct QueryServer {
    struct QueryServer* next;
    WCHAR hostname[64];
    WCHAR mapname[32];
    WCHAR gamemode[16];
    WCHAR modname[16];
    int hostPort;
    int playerCount;
    int maxPlayers;
    int ping;
    int tickets[2];
    int roundTimeRemaining;
    bool punkbuster;
    bool passworded;
    struct sockaddr_in queryAddress;
    unsigned int pingSendTime;
    int pendingQuery;
    bool pingUpdated;
    bool infoUpdated;
    bool playersUpdated;
    bool needInfo;
    bool needPlayers;
    struct QueryPlayer* players;
    int playersLength; // length of players array, not necessarily the same as playerCount
};

struct QueryState {
    HANDLE mutex;
    HANDLE thread;
    int querysocket;
    struct QueryServer* server;
    struct QueryServer* last_server;
};

void utf8ToWideBuffer(const char* str, WCHAR* outbuff, int outbufflen);

struct QueryPlayer* AllocPlayers(int count);
struct QueryServer* AddServer(const char* ip, unsigned short queryport);
struct QueryServer* GetServerByIndex(int n);
void RemoveAllServers();
struct QueryState* QueryInit();