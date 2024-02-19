#include <WinSock2.h>
#include <windows.h>
#include <stdint.h>

enum GameState {
    GS_PREGAME = 0,
    GS_PLAYING,
    GS_ENDGAME,
    GS_PAUSED,
    GS_UNKNOWN,
};

struct QueryPlayer {
    WCHAR name[32];
    short score, kills, deaths, ping;
    unsigned char team;
};

struct QueryServer {
    struct QueryServer* next;
    WCHAR hostname[64];
    WCHAR mapname[32];
    WCHAR gamemode[16];
    WCHAR modname[16];
    unsigned short hostPort;
    uint8_t playerCount;
    uint8_t maxPlayers;
    int16_t ping;
    int tickets[2];
    int roundTimeRemaining;
    bool punkbuster;
    bool passworded;
    uint8_t gameState;
    struct sockaddr_in queryAddress;
    unsigned int pingSendTime;
    unsigned int playersLastUpdated;
    unsigned int pendingQuery;
    bool pingUpdated;
    bool infoUpdated;
    bool playersUpdated;
    bool needInfo;
    bool needPlayers;
    bool needPing;
    uint8_t finalPacketNumber;
    uint32_t receivedPacketMask;
    struct QueryPlayer* players;
    size_t playersLength; // length of players array, not necessarily the same as playerCount
    // playerlist currently read from network
    struct QueryPlayer* playersNew;
    size_t playersNewLength;
    size_t playersNewMaxIdx;
};

struct QueryState {
    HANDLE mutex;
    HANDLE sleepevent;
    HANDLE thread;
    int querysocket;
    struct QueryServer* server;
    struct QueryServer* last_server;
    bool wantSleep;
};

unsigned int seconds();
void UTF8ToWideBuffer(const char* str, WCHAR* outbuff, int outbufflen);

enum GameState ParseGameState(const char* gamestate);

struct QueryPlayer* AllocPlayers(unsigned int count);
void SortPlayers(struct QueryPlayer* players, unsigned int count);
struct QueryServer* AddServer(const char* ip, unsigned short queryport);
struct QueryServer* GetServerByIndex(int n);
void RemoveAllServers();
void ResetPlayerQuery(struct QueryServer* svr);
void ResetPendingQuery(struct QueryServer* svr);
struct QueryState* QueryInit();