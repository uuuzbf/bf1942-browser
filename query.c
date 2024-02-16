#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define UNICODE 1
#include <windows.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <WinSock2.h>
#include "query.h"

enum {
    QUERY_NONE = 0,
    QUERY_INFO,
    QUERY_PLAYERS,
    QUERY_SERVER_INFO,
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

void bf1942ToWideBuffer(const char* str, WCHAR*outbuff, int outbufflen)
{
    if(outbufflen < 1) return;
    if(outbufflen == 1) {
        *outbuff = 0;
        return;
    }
    // determine if the string should be parsed as cyrillic
    // algorithm is from henk's datafield
    int latinCount = 0;
    int cyrillicCount = 0;
    for(const uint8_t* c = (const uint8_t*)str; *c; c++){
        uint8_t C = *c;
        if((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'Z')) latinCount++;
        else if(C >= 0xC0) cyrillicCount++; // codes above 0xC0 are used for cyrillic
    }
    bool isCyrillic = (latinCount + cyrillicCount) > 0 && (((float)cyrillicCount / (latinCount + cyrillicCount)) >= 0.8);

    for(const uint8_t* c = (const uint8_t*)str; *c; c++){
        uint8_t C = *c;
        WCHAR W;
        // replace control characters and latin1 non printable
        if(C < 0x20 || (C >= 0x7F && C <= 0x9F)) W = UNICODE_NOCHAR;
        else if(isCyrillic && C >= 0xC0) W = (WCHAR)C - 0xC0 + 0x0410; // 0xC0... is mapped to unicode 0x0410... characters
        else W = C; // rest is latin1 which directly maps to unicode character codes
        // copy character to output buffer
        *(outbuff++) = W;
        // decrement buffer length, check if space is only enough for the terminating null character
        if(--outbufflen == 1) break;
    }
    *outbuff = 0;
}

// parses \key_INDEX\value\ into key, INDEX and value
#define GS_NO_INDEX (~(unsigned int)0)
char* GSParseNextKV(char** source, char** value, unsigned int* index)
{
    char* key = *source;
    // if the first character is a backslash, skip it
    if(*key == '\\')key++;
    // find the next backslash, the end of the key
    char* key_end = strchr(key, '\\');
    // check if there is a proper key
    if(!key_end || key_end == key) return 0; // error: missing key-value backslash or empty key
    // replace key-value delimiter backslash with nullbyte
    *key_end = 0;

    // start searching for an underscore at the end of the key
    char* index_start = key_end - 1;
    // walk backwards while there are digits and the beginning of the key is not reached
    while(isdigit(*index_start) && index_start != key) index_start--;
    // if the search stopped at an underscore and there are digits after it
    if(*index_start == '_' && (index_start + 1) != key_end){
        // index not empty
        *(index_start++) = 0; // replace key-index delimiter underscore with nullbyte
        *index = strtoul(index_start, 0, 10); // parse index as base-10 unsigned integer
    }
    else {
        *index = GS_NO_INDEX; // key has no index
    }

    // set value ptr to after the key-value delimiter
    *value = key_end + 1;
    // look for the backslash between the value and the next key
    char* value_end = strchr(*value, '\\');
    if(value_end){
        // backslash found before next key, zero it to terminate the value string
        *value_end = 0;
        // set the source pointer after the trailing backslash
        *source = value_end + 1;
    }
    else {
        // last value has no trailing backslash
        // mark end of source string by setting it to null
        *source = 0;
    }
    return key;
}

struct QueryPlayer* AllocPlayers(unsigned int count)
{
    if(count < 1 || count > 256) return 0;
    return calloc(count, sizeof(struct QueryPlayer));
}


int SortPlayers_compare(const void* lv, const void* rv)
{
    const struct QueryPlayer* left = lv;
    const struct QueryPlayer* right = rv;
    int cmp = (left->score < right->score) - (left->score > right->score);
    if(cmp) return cmp;
    cmp = (left->kills < right->kills) - (left->kills > right->kills);
    if(cmp) return cmp;
    return (left->deaths > right->deaths) - (left->deaths < right->deaths);
}

void SortPlayers(struct QueryPlayer* players, unsigned int count)
{
    qsort(players, count, sizeof(players[0]), SortPlayers_compare);
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
        case QUERY_SERVER_INFO:{
            // For measuring ping to the server.
            // This is not a regular query port packet, but a part of the game protocol, and sent to the game port.
            // This is used by the client when the server is changing maps and restarting, this packet is used to detect
            // when the server is up before the client restarts for the mapchange.
            // It is better to use this to measure pings, because the query packets are processed in the server's main thread,
            // so they may sit in the receive buffer for up to 16 milliseconds due to the server's tickrate.
            // The game protocol is processed on its own thread which is always receiving new packets so a SERVER_INFO_REQUEST
            // is answered instantly.

            // make sure we know the game port
            if(svr->hostPort == 0) return;
            buf = "\x00\x00\x90\x00\x00\x00\x00";
            len = 8;
            struct sockaddr_in addr = svr->queryAddress;
            addr.sin_port = htons(svr->hostPort);
            sendto(params.querysocket, buf, len, 0, (struct sockaddr*)&addr, sizeof(addr));
            svr->pingSendTime = ticks();
            printf("SERVER_INFO_REQUEST to %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
            svr->needPing = false;
            return;
        }
        default: return;
    }
    printf("%s (%d) to %s:%d\n", buf, type, inet_ntoa(svr->queryAddress.sin_addr), ntohs(svr->queryAddress.sin_port));
    sendto(params.querysocket, buf, len, 0, (struct sockaddr*)&svr->queryAddress, sizeof(svr->queryAddress));
    svr->pendingQuery = type;
}

void HandleInfoResponse(struct QueryServer* svr, char* data, size_t length)
{
    bool final = false;
    
    for(;;){
        char* value;
        unsigned int index;
        
        if(data == 0){
            printf("query end of data\n");
            // end of data
            break;
        }

        char* key = GSParseNextKV(&data, &value, &index);
        if(key == 0) {
            printf("query parse error\n");
            // parse error
            break;
        }
        
        if(!strcmp(key, "hostname")) bf1942ToWideBuffer(value, svr->hostname, ARRAYSIZE(svr->hostname));
        else if(!strcmp(key, "gameId")) bf1942ToWideBuffer(value, svr->modname, ARRAYSIZE(svr->modname));
        else if(!strcmp(key, "gametype")) bf1942ToWideBuffer(value, svr->gamemode, ARRAYSIZE(svr->gamemode));
        else if(!strcmp(key, "mapname")) bf1942ToWideBuffer(value, svr->mapname, ARRAYSIZE(svr->mapname));
        else if(!strcmp(key, "maxplayers")) svr->maxPlayers = (uint8_t)strtol(value, 0, 10);
        else if(!strcmp(key, "numplayers")) svr->playerCount = (uint8_t)strtol(value, 0, 10);
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

struct QueryServer* GetServerByGameAddress(struct sockaddr_in* addr)
{
    uint16_t port = ntohs(addr->sin_port);
    for(struct QueryServer* svr = params.server; svr != 0; svr = svr->next){
        if(svr->queryAddress.sin_addr.S_un.S_addr == addr->sin_addr.S_un.S_addr && svr->hostPort == port) return svr;
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
            char buffer[1501];
            struct sockaddr_in remote;
            int res;
            for(;;) {
                int recvlen = sizeof(remote);
                res = recvfrom(params.querysocket, buffer, 1500, 0, (struct sockaddr*)&remote, &recvlen);
                printf("data from %s:%d length %d\n", inet_ntoa(remote.sin_addr), ntohs(remote.sin_port), res);
                if(res < 0) break;

                // is it a SERVER_INFO_RESPONSE from a game port? see QUERY_SERVER_INFO in SendServerQuery
                // length is atleast 7, first byte is 1, third byte's top 4 bits is 0xA
                if(res >= 7 && ((*(uint32_t*)&buffer[0]) & 0x00F000FF) == 0x00A00001){
                    struct QueryServer* svr = GetServerByGameAddress(&remote);
                    if(svr && svr->pingSendTime != 0) {
                        svr->ping = ticks() - svr->pingSendTime;
                        svr->pingSendTime = 0;
                        svr->pingUpdated = true;
                        printf("SERVER_INFO_RESPONSE %dms\n", svr->ping);
                    }
                    continue;
                }

                // make sure there is a null terminator
                buffer[res] = 0;

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
                } else if(svr->needPing){
                    SendServerQuery(svr, QUERY_SERVER_INFO);
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

