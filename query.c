#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define UNICODE 1
#include <windows.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <WinSock2.h>
#include "query.h"

enum {
    QUERY_NONE = 0,
    QUERY_INFO,
    QUERY_PLAYERS
};

struct QueryState params;
LARGE_INTEGER performanceFrequency;

unsigned int ticks()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (unsigned int)(now.QuadPart / (performanceFrequency.QuadPart / 1000));
}

void utf8ToWideBuffer(const char* str, WCHAR* outbuff, int outbufflen)
{
    if(outbufflen <= 0) return;
    if(!str){
        *outbuff = 0;
        return;
    }
    MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, str, -1, outbuff, outbufflen - 1);
    // MultiByteToWideChar does not terminate the string if it runs out of buffer space
    outbuff[outbufflen - 1] = 0;
}

// parses \key_INDEX\value\ into key, INDEX and value
#define GS_NO_INDEX (~(unsigned int)0)
char* GSParseNextKV(char** source, char** value, unsigned int* index)
{
    // when at the start of the string, remove first backslash
    char* s = *source;
    if(*s == '\\')s++;
    char* key_end = strchr(s, '\\');
    if(!key_end || key_end == s) return 0; // error: missing key end or empty key
    *key_end = 0; // replace key-value delimiter \ with nullbyte
    char* index_start = key_end - 1;
    while(isdigit(*index_start)) index_start--; // walk backwards while there are digits
    if(*index_start == '_' && (index_start + 1) != key_end){
        // index not empty
        *(index_start++) = 0; // replace key-index delimiter _ with nullbyte
        *index = strtoul(index_start, 0, 10);
    }
    else {
        *index = GS_NO_INDEX;
    }
    char* key = s;
    s = key_end + 1;
    char* value_end = strchr(s, '\\');
    *value = s;
    if(value_end){
        // \ found before next key, zero it and set the source pointer after it
        *value_end = 0;
        *source = value_end + 1;
    }
    else *source = 0; // mark end of source string
    return key;
}

struct QueryPlayer* AllocPlayers(unsigned int count)
{
    if(count < 1 || count > 256) return 0;
    return calloc(count, sizeof(struct QueryPlayer));
}

struct QueryServer* AddServer(const char* ip, unsigned short queryport)
{
    // calloc returns zeroed memory
    struct QueryServer* svr = calloc(1, sizeof(struct QueryServer));
    svr->queryAddress.sin_family = AF_INET;
    svr->queryAddress.sin_addr.S_un.S_addr = inet_addr(ip);
    svr->queryAddress.sin_port = htons(queryport);
    svr->ping = -1;
    if(params.server){
        params.last_server->next = svr;
        params.last_server = svr;
    }
    else {
        params.server = params.last_server = svr;
    }
    return svr;
}

void RemoveAllServers()
{
    for(struct QueryServer* svr = params.server; svr != 0; ){
        void* temp = svr;
        svr = svr->next;
        free(temp);
    }
    params.server = params.last_server = 0;
}

struct QueryServer* GetServerByIndex(int n)
{
    for(struct QueryServer* svr = params.server; svr != 0; svr = svr->next){
        if(n-- == 0) return svr;
    }
    return 0;
}

void SendServerQuery(struct QueryServer* svr, int type)
{
    const char* buf;
    int len;
    switch(type){
        case QUERY_INFO: buf = "\\info\\", len = 6; svr->needInfo = false; break;
        case QUERY_PLAYERS: buf = "\\players\\", len = 9; svr->needPlayers = false; break;
        default: return;
    }
    printf("%s (%d) to %s:%d\n", buf, type, inet_ntoa(svr->queryAddress.sin_addr), ntohs(svr->queryAddress.sin_port));
    sendto(params.querysocket, buf, len, 0, (struct sockaddr*)&svr->queryAddress, sizeof(svr->queryAddress));
    if(type == QUERY_INFO)svr->pingSendTime = ticks();
    svr->pendingQuery = type;
}

void HandleInfoResponse(struct QueryServer* svr, char* data, size_t length)
{
    bool final = false;
    
    for(;;){
        char* value;
        unsigned int index;
        char* key = GSParseNextKV(&data, &value, &index);
        if(key == 0) {
            printf("query parse error\n");
            // parse error
            break;
        }
        if(data == 0){
            printf("query end of data\n");
            // end of data
            break;
        }
        if(!strcmp(key, "hostname")) utf8ToWideBuffer(value, svr->hostname, ARRAYSIZE(svr->hostname));
        else if(!strcmp(key, "gameId")) utf8ToWideBuffer(value, svr->modname, ARRAYSIZE(svr->modname));
        else if(!strcmp(key, "gametype")) utf8ToWideBuffer(value, svr->gamemode, ARRAYSIZE(svr->gamemode));
        else if(!strcmp(key, "mapname")) utf8ToWideBuffer(value, svr->mapname, ARRAYSIZE(svr->mapname));
        else if(!strcmp(key, "maxplayers")) svr->maxPlayers = strtol(value, 0, 10);
        else if(!strcmp(key, "numplayers")) svr->playerCount = strtol(value, 0, 10);
        else if(!strcmp(key, "roundTimeRemain")) svr->roundTimeRemaining = strtol(value, 0, 10);
        else if(!strcmp(key, "tickets1")) svr->tickets[0] = strtol(value, 0, 10);
        else if(!strcmp(key, "tickets2")) svr->tickets[1] = strtol(value, 0, 10);
        else if(!strcmp(key, "password")) svr->passworded = strtol(value, 0, 10) != 0;
        else if(!strcmp(key, "sv_punkbuster")) svr->punkbuster = strtol(value, 0, 10) != 0;
        else if(!strcmp(key, "final")) final = true;
        else continue;
        printf(" parsed %s[%i] = %s\n", key, index, value);
    }
    if(final){        
        svr->pendingQuery = QUERY_NONE;
        svr->infoUpdated = true;
    }
}

void HandleServerResponse(struct QueryServer* svr, char* data, size_t length)
{
    if(svr->pingSendTime != 0){
        svr->ping = ticks() - svr->pingSendTime;
        svr->pingSendTime = 0;
        svr->pingUpdated = true;
    }

    printf("response from %s:%d - %d\n", inet_ntoa(svr->queryAddress.sin_addr), ntohs(svr->queryAddress.sin_port), svr->pendingQuery);

    if(svr->pendingQuery == QUERY_INFO){
        HandleInfoResponse(svr, data, length);
    }
}

struct QueryServer* GetServerByQueryAddress(struct sockaddr_in* addr)
{
    for(struct QueryServer* svr = params.server; svr != 0; svr = svr->next){
        if(svr->queryAddress.sin_addr.S_un.S_addr == addr->sin_addr.S_un.S_addr && svr->queryAddress.sin_port == addr->sin_port) return svr;
    }
    return 0;
}

DWORD __stdcall QueryThreadMain(void* arg)
{
    unsigned int lastSend = 0;
    for(;;){
        bool can_read = false;
        if(params.querysocket != -1){
            static const struct timeval sleeptime = {0, 100000};
            fd_set read;
            FD_ZERO(&read);
            FD_SET((unsigned)params.querysocket, &read);
            int result = select(params.querysocket + 1, &read, 0, 0, &sleeptime);
            if(result == -1){
                printf("select failed\n");
                return 1;
            }
            can_read = result > 0;
            //printf("select says there is %s\n", can_read?"data":"no data");
        }
        else {
            printf("QueryThreadMain: no socket\n");
            Sleep(100);
            continue;
        }
        if(WaitForSingleObject(params.mutex, INFINITE) == WAIT_FAILED)break;
        if(can_read){
            char buffer[1500];
            struct sockaddr_in remote;
            int res;
            for(;;) {
                int recvlen = sizeof(remote);
                res = recvfrom(params.querysocket, buffer, 1500, 0, (struct sockaddr*)&remote, &recvlen);
                printf("data from %s:%d length %d\n", inet_ntoa(remote.sin_addr), ntohs(remote.sin_port), res);
                if(res < 0) break;
                struct QueryServer* svr = GetServerByQueryAddress(&remote);
                if(!svr) continue;
                HandleServerResponse(svr, buffer, res);
            }
        }
        unsigned int now = ticks();
        if(now - lastSend >= 100){
            lastSend = now;
            int sends = 6;
            for(struct QueryServer* svr = params.server; svr != 0; svr = svr->next){
                if(svr->pendingQuery != QUERY_NONE) continue;
                if(svr->needInfo){
                    SendServerQuery(svr, QUERY_INFO);
                } else if(svr->needPlayers){
                    SendServerQuery(svr, QUERY_PLAYERS);
                    svr->needPlayers = false;
                }
                else continue;
                if(--sends == 0)break;
            }
        }
        ReleaseMutex(params.mutex);
    }
    printf("QueryThreadMain exits\n");
    return 0;
}

struct QueryState* QueryInit()
{
    if(params.mutex != 0) {
        MessageBox(0, L"QueryInit called twice", L"Error", MB_ICONERROR);
        exit(1);
    }

    QueryPerformanceFrequency(&performanceFrequency);

    params.mutex = CreateMutex(0, false, 0);

    struct WSAData d;
    WSAStartup(MAKEWORD(1, 1), &d);

    params.querysocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(params.querysocket == -1){
        printf("failed to create socket: %d\n", WSAGetLastError());
        return &params;
    }

    unsigned long nonblocking = 1;
    ioctlsocket(params.querysocket, FIONBIO, &nonblocking);

    struct sockaddr_in bindaddr = {0};
    bindaddr.sin_family = AF_INET;
    if(bind(params.querysocket, (struct sockaddr*)&bindaddr, sizeof(bindaddr)) != 0){
        closesocket(params.querysocket);
        params.querysocket = -1;
        printf("failed to bind socket: %d\n", WSAGetLastError());
        return &params;
    }

    params.thread = CreateThread(0, 0, QueryThreadMain, 0, 0, 0);

    return &params;
}

