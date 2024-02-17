#define UNICODE 1
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <windows.h>
#include <DbgHelp.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <winhttp.h>
#include <Mmsystem.h>

#include "cjson/cJSON.h"
#include "query.h"
#include "debug.h"

enum {
    ID_SERVERLIST = 100,
    ID_PLAYERLIST,
    ID_ADDRESSLABEL,
    ID_SERVERINFOLABEL,
    ID_REFRESHBTN,
    ID_PULSE_TIMER,
    ID_SECOND_TIMER
};

void ReloadServers();
void PulseTimer();
void PulseSecond();

// return values of this are never freed so if you click around too much in the serverlist
// eventually you run out of memory
WCHAR* utf8ToWide(const char* str)
{
    int size = MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, str, -1, 0, 0);
    if(size <= 0) return 0;
    WCHAR* result = malloc(size*sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, str, -1, result, size);
    return result;
}

WCHAR bf1942_path[1024];
HWND mainwindow = 0;
HWND serverlist = 0;
HWND playerlist = 0;
HWND addresslabel = 0;
HWND serverinfolabel = 0;
//cJSON* serverdata = 0;
struct QueryState* queryState = 0;


void initBF1942Path()
{
    // try to read first line from bf1942_path.txt
    FILE* h = fopen("bf1942_path.txt", "r");
    if(h){
        WCHAR* path = fgetws(bf1942_path, 1024, h);
        fclose(h);
        if(path){
            // trim trailing \r\n
            size_t pathlen = wcslen(path);
            while(pathlen > 0 && (path[pathlen-1] == '\n' || path[pathlen-1] == '\r')){
                path[--pathlen] = 0;
            }
            // if file exists
            h = _wfopen(path, L"rb");
            if(h){
                fclose(h);
                // done
                return;
            }
        }

    }
    else {
        h = fopen("BF1942.exe", "rb");
        if(h){
            fclose(h);
            // bf1942.exe is right next to ours, set path to .\bf1942.exe
            wcscpy(bf1942_path, L".\\BF1942.exe");
            return;
        }
    }
    // we dont have a BF1942.exe path, ask the user
    OPENFILENAME of = {0};
    bf1942_path[0] = 0;
    of.lStructSize = sizeof(of);
    of.hwndOwner = mainwindow;
    of.lpstrFile = bf1942_path;
    of.nMaxFile = sizeof(bf1942_path);
    of.lpstrTitle = L"Select BF1942.exe in your game folder";
    of.lpstrFilter = L"BF1942.exe\0BF1942.exe\0";
    of.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if(GetOpenFileName(&of)){
        // store path
        h = fopen("bf1942_path.txt", "w");
        if(h){
            fputws(bf1942_path, h);
            fclose(h);
            dbgprintf("bf1942 path saved\n");
        }
        else{
            dbgprintf("failed to save path, unable to open txt file\n");
        }
    }
    // no path
}

int GetSelectedServerID()
{
    // somebody on the internet said to not trust this
    // ListView_GetSelectionMark(serverlist);
    return ListView_GetNextItem(serverlist, -1, LVNI_SELECTED);
}

struct QueryServer* GetSelectedServer()
{
    int selectedServer = GetSelectedServerID();
    //dbgprintf("ConnectToServer selectedServer = %d\n", selectedServer);
    if(selectedServer < 0) return 0;
    
    struct QueryServer* svr = GetServerByIndex(selectedServer);
    if(!svr){
        dbgprintf("GetSelectedServer - unknown server\n");
        return 0;
    }
    return svr;
}

void CopySelectedServerAddress()
{
    struct QueryServer* svr = GetSelectedServer();
    if(!svr) return;

    WaitForSingleObject(queryState->mutex, INFINITE);
    WCHAR address[32];
    int length = _snwprintf(address, 32, L"%hs:%d", inet_ntoa(svr->queryAddress.sin_addr), svr->hostPort); // %hs is MSVC specific
    ReleaseMutex(queryState->mutex);

    if(length <= 0) return;

    if(!OpenClipboard(mainwindow)){
        dbgprintf("OpenClipboard failed %d\n", GetLastError());
        return;
    }

    EmptyClipboard();

    size_t bytes = (length + 1) * sizeof(WCHAR);
    HGLOBAL buffer = GlobalAlloc(GMEM_MOVEABLE, bytes);

    void* p = GlobalLock(buffer);
    memcpy(p, address, bytes);
    GlobalUnlock(buffer);

    SetClipboardData(CF_UNICODETEXT, buffer);

    CloseClipboard();

    PlaySound(L"MouseClick", NULL, SND_SYNC); 
}

void UpdateServerInfo(struct QueryServer* svr)
{
    WCHAR serverinfo[64];
    int len = _snwprintf(serverinfo, 64, L"Axis: %d Allies: %d", svr->tickets[0], svr->tickets[1]);

    if(svr->roundTimeRemaining != -1){
        len += _snwprintf(serverinfo + len, 64 - len, L" Timer: %d:%02d", svr->roundTimeRemaining / 60, svr->roundTimeRemaining % 60);
    }
    else wcscat_s(serverinfo, 64, L" Timer: none");

    if(svr->punkbuster) wcscat_s(serverinfo, 64, L" [PB]");
    if(svr->passworded) wcscat_s(serverinfo, 64, L" [PW]");

    SetWindowText(serverinfolabel, serverinfo);
}

// needs a cleared player list!
void PopulatePlayerList(struct QueryServer* svr)
{
    // if there are no players on the server this array will be missing
    if(svr->players != 0){
        for(unsigned i = 0; i < svr->playersLength; i++){
            struct QueryPlayer* player = svr->players + i;

            WCHAR scorestr[32], pingstr[16];
            _snwprintf(scorestr, 32, L"%d/%d/%d", player->score, player->kills, player->deaths);
            _snwprintf(pingstr, 16, L"%d", player->ping);

            LV_ITEM li = {0};
            li.mask = LVIF_TEXT;
            li.iItem = i; // row number
            li.iSubItem = 0; // player name
            li.pszText = player->name;
            ListView_InsertItem(playerlist, &li);
            li.iSubItem = 1; // score
            li.pszText = scorestr;
            ListView_SetItem(playerlist, &li);
            li.iSubItem = 2; // ping
            li.pszText = pingstr;
            ListView_SetItem(playerlist, &li);
        }
    }
}

void SelectServer(int index)
{
    dbgprintf("selecting server %d\n", index);
    ListView_DeleteAllItems(playerlist);

    struct QueryServer* svr = GetServerByIndex(index);
    if(!svr){
        dbgprintf("ConnectToServer - unknown server\n");
        return;
    }

    WaitForSingleObject(queryState->mutex, INFINITE);
    WCHAR address[32];
    _snwprintf(address, 32, L"%hs:%d", inet_ntoa(svr->queryAddress.sin_addr), svr->hostPort); // %hs is MSVC specific
    SetWindowText(addresslabel, address);

    // query info when server selected
    svr->needInfo = true;
    svr->needPing = true;

    PopulatePlayerList(svr);

    UpdateServerInfo(svr);

    ReleaseMutex(queryState->mutex);
}

void ConnectToServer()
{
    struct QueryServer* svr = GetSelectedServer();
    if(!svr){
        return;
    }

    if(!bf1942_path[0]){
        initBF1942Path();
        if(!bf1942_path[0]) return;
    }

    WCHAR exe_args[256];
    _snwprintf(exe_args, 256, L"BF1942.exe +restart 1 +joinServer %hs:%d +game %ls", inet_ntoa(svr->queryAddress.sin_addr), svr->hostPort, svr->modname); // %hs is MSVC specific

    WCHAR bf_dir[1024] = {0};
    WCHAR* dirslash = wcsrchr(bf1942_path, '\\');
    if(dirslash != 0){
        for(int i = 0; &bf1942_path[i] != dirslash; i++){
            bf_dir[i] = bf1942_path[i];
        }
    }
    dbgprintf("executing \"%ls\" %ls\n", bf1942_path, exe_args);
    dbgprintf("in dir \"%ls\"\n", bf_dir);

    STARTUPINFO sui = {0};
    sui.cb = sizeof(sui);
    PROCESS_INFORMATION pi = {0};
    bool ok = CreateProcess(bf1942_path, exe_args, 0, 0, false, CREATE_NEW_PROCESS_GROUP, 0, bf_dir, &sui, &pi);
    if(!ok){
        dbgprintf("CreateProcess failed: %d\n", GetLastError());
    }
    else {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void __stdcall ListViewNotify(NMHDR* notify)
{
    //dbgprintf("WM_NOTIFY: code=%d hwnd=%p id=%d\n", notify->code, notify->hwndFrom, notify->idFrom);
    NMITEMACTIVATE* act = (NMITEMACTIVATE*)notify;
    switch(notify->code){
        case NM_CLICK:
            dbgprintf("clicked %d %d\n", act->iItem, act->iSubItem);
            break;
        case NM_DBLCLK:
            ConnectToServer();
            break;
        case LVN_ODSTATECHANGED:{
            NMLVODSTATECHANGE* ch = (NMLVODSTATECHANGE*)notify;
            //dbgprintf("statechange %08X -> %08X %d-%d\n", ch->uOldState, ch->uNewState, ch->iFrom, ch->iTo);
            if((ch->uNewState & LVIS_SELECTED) && !(ch->uOldState & LVIS_SELECTED)){
                if(ch->iFrom != ch->iTo)MessageBox(0, L"wat", L"????", MB_ICONQUESTION);
                SelectServer(ch->iFrom);
            }
            break;
        }
        case LVN_ITEMCHANGED:{
            NMLISTVIEW* lv = (NMLISTVIEW*)notify;
            //dbgprintf("itemchanged %d %d %08X -> %08X\n", lv->iItem, lv->iSubItem, lv->uOldState, lv->uNewState);
            if((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)){
                SelectServer(lv->iItem);
            }
            break;
        }

    }
}

LRESULT __stdcall WndProcMain(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg){
        // case WM_ERASEBKGND:{
        //     HDC dc = (HDC)wParam;
        //     HBRUSH backgroundColor = GetStockObject(COLOR_BTNFACE + 1);
        //     RECT rect;
        //     GetClientRect(hwnd, &rect);
        //     FillRect(dc, &rect, backgroundColor);
        //     return 1;
        // }
        case WM_COMMAND:
            dbgprintf("WM_COMMAND: %d\n", wParam);
            if(wParam == IDCLOSE){
                DestroyWindow(hwnd);
            } else if(wParam == ID_REFRESHBTN){
                ReloadServers();
            } else if(wParam == ID_ADDRESSLABEL){
                CopySelectedServerAddress();
            }
            break;
        //case WM_SETFOCUS:
        //    dbgprintf("SETFOCUS %08X -> %p\n", wParam, hwnd);
        //    break;
        //case WM_KILLFOCUS:
        //    dbgprintf("KILLFOCUS %p -> %08X\n", hwnd, wParam);
        //    break;
        case WM_ACTIVATE:
        //    dbgprintf("ACTIVATE %p state %d to %08X\n", hwnd, wParam, lParam);
            if(wParam == WA_ACTIVE || wParam == WA_CLICKACTIVE){
                SetTimer(mainwindow, ID_PULSE_TIMER, 200, 0);
                SetTimer(mainwindow, ID_SECOND_TIMER, 2000, 0);
            }
            else if(wParam == WA_INACTIVE){
                KillTimer(mainwindow, ID_PULSE_TIMER);
                KillTimer(mainwindow, ID_SECOND_TIMER);
            }
            break;
        case WM_NOTIFY:{
            NMHDR* notify = (NMHDR*)lParam;
            switch(wParam){
                case ID_SERVERLIST: ListViewNotify(notify); break;
            }

            break;
        }
        case WM_TIMER:
            if(wParam == ID_PULSE_TIMER) PulseTimer();
            else if(wParam == ID_SECOND_TIMER) PulseSecond();
            break;

    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

char* GetServerList(DWORD* length)
{
    // http://master.bf1942.org/json/?full
    // use WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY? Win8.1+ only
    // flag WINHTTP_FLAG_ASYNC exists, will be useful later
    HINTERNET session = 0, connect = 0, request = 0;
    char* body = 0;
    bool result = false;
    session = WinHttpOpen(0, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 /* flags */);
    if(session == 0){
        dbgprintf("failed to create winhttp session\n");
        goto cleanup;
    }
    connect = WinHttpConnect(session, L"master.bf1942.org", INTERNET_DEFAULT_PORT, 0);
    if(connect == 0){
        dbgprintf("failed to create winhttp connect\n");
        goto cleanup;
    }
    // flag WINHTTP_FLAG_SECURE may be useful later
    // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpopenrequest
    request = WinHttpOpenRequest(connect, L"GET", L"/json/?full", 0 /* HTTP 1.1 */, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if(request == 0){
        dbgprintf("failed to create winhttp request\n");
        goto cleanup;
    }
    // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpsendrequest
    result = WinHttpSendRequest(request, 0, 0, 0, 0, 0, 0);
    if(!result){
        dbgprintf("request failed!\n");
        goto cleanup;
    }

    // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpreceiveresponse
    result = WinHttpReceiveResponse(request, 0);
    if(!result){
        dbgprintf("receive response failed!\n");
        goto cleanup;
    }

    DWORD body_length = 0;
    for(;;){
        DWORD bytes;
        // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpquerydataavailable
        if(!WinHttpQueryDataAvailable(request, &bytes)){
            dbgprintf("query data available failed\n");
            result = false; goto cleanup;
        }
        if(bytes == 0)break;
        //dbgprintf("receiving %u bytes\n", bytes);
        DWORD new_body_length = body_length + bytes;
        if(new_body_length > 1024*1024*64){
            // a 64MB json file? huh this game was never that popular
            dbgprintf("response json data too big!\n");
            result = false; goto cleanup;
        }
        body = realloc(body, new_body_length + 1);
        if(body == 0){
            dbgprintf("get_server_list realloc failed\n");
            result = false; goto cleanup;
        }
        DWORD bytes_read;
        WinHttpReadData(request, body + body_length, bytes, &bytes_read);
        body_length += bytes_read;
        //dbgprintf("read %u bytes\n", bytes_read);
    }
    body[body_length] = 0;
    dbgprintf("got server list json, %u bytes\n", body_length);
    if(length != 0) *length = body_length;
cleanup:
    if(!result && body != 0){
        free(body);
        body = 0;
    }
    if(session) WinHttpCloseHandle(session);
    if(connect) WinHttpCloseHandle(connect);
    if(request) WinHttpCloseHandle(request);
    return body;
}

// needs query mutex lock
void LoadServerListFromJSON(char* json, DWORD length)
{
    cJSON* root = cJSON_ParseWithLength(json, length);
    if(root == 0){
        dbgprintf("failed to parse root json object\n");
        return;
    }
    dbgprintf("parsed root object of type %d\n", root->type);
    if(!cJSON_IsArray(root)){
        dbgprintf("root object is not array!\n");
        goto cleanup;
    }

    // clear the lists now because clearing the QueryServers will break the lists anyway
    ListView_DeleteAllItems(serverlist);
    ListView_DeleteAllItems(playerlist);
    SetWindowText(addresslabel, L"Select a server");

    RemoveAllServers();

    for(int i = 0; i < cJSON_GetArraySize(root); i++){
        cJSON* server = cJSON_GetArrayItem(root, i);
        cJSON* query = cJSON_GetObjectItem(server, "query");
        cJSON* queryPort = cJSON_GetObjectItem(server, "queryPort");
        if(!query || !queryPort) { dbgprintf("malformed server data\n"); continue; }
        cJSON* IP = cJSON_GetObjectItem(server, "IP");
        cJSON* hostport = cJSON_GetObjectItem(query, "hostport");
        if(!(IP && hostport)) { dbgprintf("malformed query data\n"); continue; }

        struct QueryServer* svr = AddServer(cJSON_GetStringValue(IP), (unsigned short)cJSON_GetNumberValue(queryPort));
        if(!svr){
            dbgprintf("AddServer failed for %s:%d\n", cJSON_GetStringValue(IP), (int)cJSON_GetNumberValue(queryPort));
            continue;
        }
        svr->hostPort = (unsigned short)cJSON_GetNumberValue(hostport);
        svr->maxPlayers = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(query, "maxplayers"));
        svr->playerCount = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(query, "numplayers"));
        utf8ToWideBuffer(cJSON_GetStringValue(cJSON_GetObjectItem(query, "hostname")), svr->hostname, ARRAYSIZE(svr->hostname));
        utf8ToWideBuffer(cJSON_GetStringValue(cJSON_GetObjectItem(query, "mapname")), svr->mapname, ARRAYSIZE(svr->mapname));
        utf8ToWideBuffer(cJSON_GetStringValue(cJSON_GetObjectItem(query, "gameId")), svr->modname, ARRAYSIZE(svr->modname));
        utf8ToWideBuffer(cJSON_GetStringValue(cJSON_GetObjectItem(query, "gametype")), svr->gamemode, ARRAYSIZE(svr->gamemode));
        svr->tickets[0] = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(query, "tickets1"));
        svr->tickets[1] = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(query, "tickets2"));
        svr->roundTimeRemaining = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(query, "roundTimeRemain"));
        svr->punkbuster = cJSON_IsTrue(cJSON_GetObjectItem(query, "sv_punkbuster"));
        svr->passworded = cJSON_IsTrue(cJSON_GetObjectItem(query, "password"));

        // request info query from server
        svr->needInfo = true;
        svr->needPing = true;

        cJSON* jplayers = cJSON_GetObjectItem(query, "players");
        if(!jplayers){
            dbgprintf("%s:%d has no playerlist\n", cJSON_GetStringValue(IP), (int)cJSON_GetNumberValue(queryPort));
            continue;
        }
        unsigned int numPlayers = cJSON_GetArraySize(jplayers);

        if(numPlayers > 0) {
            struct QueryPlayer* players = AllocPlayers(numPlayers);
            if(!players){
                dbgprintf("failed to allocate players structs for %s:%d\n", cJSON_GetStringValue(IP), (int)cJSON_GetNumberValue(queryPort));
                continue;
            }
            for(unsigned int j = 0; j < numPlayers; j++){
                cJSON* jplayer = cJSON_GetArrayItem(jplayers, j);
                if(!jplayer) continue;
                struct QueryPlayer* player = players + j;
                utf8ToWideBuffer(cJSON_GetStringValue(cJSON_GetObjectItem(jplayer, "playername")), player->name, ARRAYSIZE(player->name));
                player->score = (short)cJSON_GetNumberValue(cJSON_GetObjectItem(jplayer, "score"));
                player->kills = (short)cJSON_GetNumberValue(cJSON_GetObjectItem(jplayer, "kills"));
                player->deaths = (short)cJSON_GetNumberValue(cJSON_GetObjectItem(jplayer, "deaths"));
                player->team = (char)cJSON_GetNumberValue(cJSON_GetObjectItem(jplayer, "team"));
                player->ping = (short)cJSON_GetNumberValue(cJSON_GetObjectItem(jplayer, "ping"));
            }

            SortPlayers(players, numPlayers);
            svr->players = players;
            svr->playersLength = numPlayers;
        }
    }
cleanup:
    if(root != 0)cJSON_Delete(root);
}

// needs query mutex lock
void PopulateServerList()
{
    int i = 0;
    for(struct QueryServer* svr = GetServerByIndex(0); svr != 0; svr = svr->next, i++){
        WCHAR players[32];
        _snwprintf(players, 32, L"%d / %d", svr->playerCount, svr->maxPlayers);

        WCHAR ping[8];
        if(svr->ping != -1) _snwprintf(ping, 8, L"%d", svr->ping);
        else wcscpy(ping, L"?");

        //dbgprintf("%-32s %3d / %-3d %s\n", cJSON_GetStringValue(hostname), (int)cJSON_GetNumberValue(numplayers), (int)cJSON_GetNumberValue(maxplayers), cJSON_GetStringValue(mapname));

        LV_ITEM li = {0};
        li.mask = LVIF_TEXT;
        li.iItem = i; // row number
        li.iSubItem = 0; // server name column
        li.pszText = svr->hostname;
        ListView_InsertItem(serverlist, &li);
        li.iSubItem = 1; // players
        li.pszText = players;
        ListView_SetItem(serverlist, &li);
        li.iSubItem = 2; // ping
        li.pszText = ping;
        ListView_SetItem(serverlist, &li);
        li.iSubItem = 3; // map
        li.pszText = svr->mapname;
        ListView_SetItem(serverlist, &li);
        li.iSubItem = 4; // gamemode
        li.pszText = svr->gamemode;
        ListView_SetItem(serverlist, &li);
        li.iSubItem = 5; // mod
        li.pszText = svr->modname;
        ListView_SetItem(serverlist, &li);

    }
}

void ReloadServers()
{
    DWORD server_list_json_length;
    char* server_list_json = GetServerList(&server_list_json_length);
    if(server_list_json){
        WaitForSingleObject(queryState->mutex, INFINITE);

        // prevent flickering while the list is updated
        SendMessage(serverlist, WM_SETREDRAW, false, 0);

        LoadServerListFromJSON(server_list_json, server_list_json_length);

        free(server_list_json);
        server_list_json = 0;

        PopulateServerList();

        SendMessage(serverlist, WM_SETREDRAW, true, 0);

        ReleaseMutex(queryState->mutex);
    }
    else {
        MessageBox(mainwindow, L"Failed to download server list", L"Error", MB_ICONERROR);
    }
}

void PulseSecond()
{
    dbgprintf("second\n");
    WaitForSingleObject(queryState->mutex, INFINITE);

    struct QueryServer* svr = GetSelectedServer();
    if(svr){
        if(svr->pendingQuery == 0){
            svr->needInfo = true;
            svr->needPing = true;
        }
        else {
            // probably a timeout or error occured, reset pending query so next time new requests are sent
            ResetPendingQuery(svr);
        }
    }

    ReleaseMutex(queryState->mutex);
}

void PulseTimer()
{
    //dbgprintf("pulse\n");
    int selectedServer = GetSelectedServerID();
    WaitForSingleObject(queryState->mutex, INFINITE);

    int i = 0;
    // prevent flickering while the list is updated
    SendMessage(serverlist, WM_SETREDRAW, false, 0);
    for(struct QueryServer* svr = GetServerByIndex(0); svr != 0; svr = svr->next, i++){
        if(svr->pingUpdated){
            WCHAR ping[8];
            _snwprintf(ping, 8, L"%d", svr->ping);
            ListView_SetItemText(serverlist, i, 2, ping);
            svr->pingUpdated = false;
        }
        if(svr->infoUpdated){
            ListView_SetItemText(serverlist, i, 0, svr->hostname);
            WCHAR players[32];
            _snwprintf(players, 32, L"%d / %d", svr->playerCount, svr->maxPlayers);
            ListView_SetItemText(serverlist, i, 1, players);
            ListView_SetItemText(serverlist, i, 3, svr->mapname);
            ListView_SetItemText(serverlist, i, 4, svr->gamemode);
            ListView_SetItemText(serverlist, i, 5, svr->modname);

            svr->infoUpdated = false;

            if(i == selectedServer){
                UpdateServerInfo(svr);

                // if player count not zero, check if the playerlist needs an update:
                // - if the list was never updated
                // - the list was updated more than 15 seconds ago
                // - the player count is different from the list length
                if(svr->playerCount != 0 && (
                    svr->playersLastUpdated == 0 ||
                    (seconds() - svr->playersLastUpdated) > 15 ||
                    svr->playerCount != svr->playersLength))
                {
                    svr->needPlayers = true;
                }
            }
        }
        if(svr->playersUpdated){
            if(i == selectedServer){
                ListView_DeleteAllItems(playerlist);
                SortPlayers(svr->players, svr->playerCount);
                PopulatePlayerList(svr);
            }

            svr->playersUpdated = false;
        }
    }
    
    SendMessage(serverlist, WM_SETREDRAW, true, 0);

    ReleaseMutex(queryState->mutex);
}

int __stdcall WinMain(HINSTANCE instance, HINSTANCE previnstance, LPSTR commandline, int cmdshow)
{
    InitCrashHandler();
#ifdef DEBUG
    InitConsole();
#endif

    INITCOMMONCONTROLSEX cc;
    cc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&cc);

    WNDCLASSEX wndclass = {0};
    wndclass.cbSize = sizeof(wndclass);
    wndclass.lpszClassName = L"BrowserMainClass";
    wndclass.lpfnWndProc = WndProcMain;
    wndclass.hbrBackground = /*(HBRUSH)(COLOR_WINDOW + 1);*/ CreateSolidBrush(RGB(0xf0, 0xf0, 0xf0)); /*GetSysColorBrush(COLOR_WINDOW);*/
    wndclass.hInstance = instance;

	//wndclass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	//wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    //wndclass.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    if(!RegisterClassEx(&wndclass)){
        MessageBox(0, L"RegisterClassEx failed", L":(", MB_ICONERROR);
        return 1;
    }

    mainwindow = CreateWindowEx(WS_EX_OVERLAPPEDWINDOW, L"BrowserMainClass", L"BF1942 Server List", WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME, CW_USEDEFAULT, CW_USEDEFAULT, 960, 600, 0, 0, instance, 0);
    if(mainwindow == 0){
        MessageBox(0, L"CreateWindowEx failed", L":(", MB_ICONERROR);
        return 1;
    }
    NONCLIENTMETRICS ncm;
    ncm.cbSize = sizeof(NONCLIENTMETRICS);
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &ncm, 0);
    HFONT hFont = CreateFontIndirect(&ncm.lfMessageFont);
    SendMessage(mainwindow, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));

    RECT clientarea;
    GetClientRect(mainwindow, &clientarea);
    int csizex = clientarea.right-clientarea.left, csizey = clientarea.bottom-clientarea.top;

    SendMessage(CreateWindow(L"BUTTON", L"Close", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, csizex - 110, csizey - 40, 100, 30, mainwindow, (HMENU)IDCLOSE, instance, 0), WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
    SendMessage(CreateWindow(L"BUTTON", L"Refresh", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 680, csizey - 40, 100, 30, mainwindow, (HMENU)ID_REFRESHBTN, instance, 0), WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));

    addresslabel = CreateWindow(WC_STATIC, L"Select a server", WS_CHILD | WS_VISIBLE | SS_NOTIFY, 10, csizey-35, 170, 16, mainwindow, (HMENU)ID_ADDRESSLABEL, instance, 0);
    serverinfolabel = CreateWindow(WC_STATIC, L"", WS_CHILD | WS_VISIBLE, 180, csizey-35, 300, 16, mainwindow, (HMENU)ID_SERVERINFOLABEL, instance, 0);
    
    SendMessage(addresslabel, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
    SendMessage(serverinfolabel, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));

    // LVS_LIST means we get a details view with columns
    serverlist = CreateWindow(WC_LISTVIEW,  L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 10, 10, 660, csizey-50, mainwindow, (HMENU)ID_SERVERLIST, instance, 0);
    ListView_SetExtendedListViewStyle(serverlist, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_HEADERDRAGDROP | LVS_EX_DOUBLEBUFFER);
    LVCOLUMN col;
    col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
    col.fmt = LVCFMT_LEFT;
    col.iSubItem = 0;
    col.pszText = L"Server name";
    col.cx = 215;
    ListView_InsertColumn(serverlist, col.iSubItem, &col);
    col.iSubItem = 1;
    col.pszText = L"Players";
    col.cx = 50;
    ListView_InsertColumn(serverlist, col.iSubItem, &col);
    col.iSubItem = 2;
    col.pszText = L"Ping";
    col.cx = 40;
    ListView_InsertColumn(serverlist, col.iSubItem, &col);
    col.iSubItem = 3;
    col.pszText = L"Map";
    col.cx = 160;
    ListView_InsertColumn(serverlist, col.iSubItem, &col);
    col.iSubItem = 4;
    col.pszText = L"Gamemode";
    col.cx = 90;
    ListView_InsertColumn(serverlist, col.iSubItem, &col);
    col.iSubItem = 5;
    col.pszText = L"Mod";
    col.cx = 85;
    ListView_InsertColumn(serverlist, col.iSubItem, &col);

    playerlist = CreateWindow(WC_LISTVIEW,  L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL, 680, 10, csizex-680-10, csizey - 60, mainwindow, (HMENU)ID_PLAYERLIST, instance, 0);
    ListView_SetExtendedListViewStyle(playerlist, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_HEADERDRAGDROP);
    col.iSubItem = 0;
    col.pszText = L"Player name";
    col.cx = 140;
    ListView_InsertColumn(playerlist, col.iSubItem, &col);
    col.iSubItem = 1;
    col.pszText = L"S/K/D";
    col.cx = 58;
    ListView_InsertColumn(playerlist, col.iSubItem, &col);
    col.iSubItem = 2;
    col.pszText = L"Ping";
    col.cx = 30;
    ListView_InsertColumn(playerlist, col.iSubItem, &col);

    ShowWindow(mainwindow, cmdshow);

    queryState = QueryInit();
    ReloadServers();

    //CreateThread(0, 0, QueryThreadMain, 0, 0, 0);
    MSG msg;
    int res;
    while((res = GetMessage(&msg, mainwindow, 0, 0)) != 0){
        if(res == -1) return 0;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}