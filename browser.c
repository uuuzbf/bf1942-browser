#define UNICODE 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <winhttp.h>

#include "cjson/cJSON.h"
#include "query.h"

enum {
    ID_SERVERLIST = 100,
    ID_PLAYERLIST,
    ID_ADDRESSLABEL,
    ID_REFRESHBTN,
    ID_PULSE_TIMER,
    ID_SECOND_TIMER
};

// defined in show_console.c
void InitConsole();

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
cJSON* serverdata = 0;


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
    // we dont have a BF1942.exe path, ask the user
    OPENFILENAME of = {0};
    bf1942_path[0] = 0;
    of.lStructSize = sizeof(of);
    of.hwndOwner = mainwindow;
    of.lpstrFile = bf1942_path;
    of.nMaxFile = sizeof(bf1942_path);
    of.lpstrTitle = L"Select BF1942.exe in your game folder";
    of.lpstrFilter = L"BF1942.exe\0BF1942.exe\0";
    of.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if(GetOpenFileName(&of)){
        // store path
        h = fopen("bf1942_path.txt", "w");
        if(h){
            fputws(bf1942_path, h);
            fclose(h);
            printf("bf1942 path saved\n");
        }
        else{
            printf("failed to save path, unable to open txt file\n");
        }
    }
    // no path
}

void SelectServer(int index){
    printf("selecting server %d\n", index);
    ListView_DeleteAllItems(playerlist);
    cJSON* server = cJSON_GetArrayItem(serverdata, index);
    if(!server) return;
    cJSON* query = cJSON_GetObjectItem(server, "query");
    if(!query) return;

    WCHAR address[32];
    cJSON* IP = cJSON_GetObjectItem(server, "IP");
    cJSON* hostport = cJSON_GetObjectItem(query, "hostport");
    _snwprintf(address, 32, L"%hs:%d", cJSON_GetStringValue(IP), (int)cJSON_GetNumberValue(hostport)); // %hs is MSVC specific
    SetWindowText(addresslabel, address);

    cJSON* players = cJSON_GetObjectItem(query, "players");
    if(!players) return;
    int numPlayers = cJSON_GetArraySize(players);

    for(int i = 0; i < numPlayers; i++){
        cJSON* player = cJSON_GetArrayItem(players, i);
        if(!player) continue;
        cJSON* playername = cJSON_GetObjectItem(player, "playername");
        cJSON* score = cJSON_GetObjectItem(player, "score");
        cJSON* ping = cJSON_GetObjectItem(player, "ping");
        WCHAR scorestr[16], pingstr[16];
        _snwprintf(scorestr, 16, L"%d", (int)cJSON_GetNumberValue(score));
        _snwprintf(pingstr, 16, L"%d", (int)cJSON_GetNumberValue(ping));

        LV_ITEM li = {0};
        li.mask = LVIF_TEXT;
        li.iItem = i; // row number
        li.iSubItem = 0; // player name
        li.pszText = utf8ToWide(cJSON_GetStringValue(playername));
        ListView_InsertItem(playerlist, &li);
        li.iSubItem = 1; // score
        li.pszText = scorestr;
        ListView_SetItem(playerlist, &li);
        li.iSubItem = 2; // ping
        li.pszText = pingstr;
        ListView_SetItem(playerlist, &li);
    }
}

void ConnectToServer()
{
    // somebody on the internet said to not trust this
    // int selectedServer = ListView_GetSelectionMark(serverlist);
    int selectedServer = ListView_GetNextItem(serverlist, -1, LVNI_SELECTED);
    printf("connectToServer selectedServer = %d\n", selectedServer);
    if(selectedServer < 0) return;

    if(!bf1942_path[0]){
        initBF1942Path();
        if(!bf1942_path[0]) return;
    }
    

    cJSON* server = cJSON_GetArrayItem(serverdata, selectedServer);
    if(!server) return;
    cJSON* query = cJSON_GetObjectItem(server, "query");
    if(!query) return;

    WCHAR exe_args[256];
    cJSON* IP = cJSON_GetObjectItem(server, "IP");
    cJSON* hostport = cJSON_GetObjectItem(query, "hostport");
    cJSON* gameId = cJSON_GetObjectItem(query, "gameId");
    _snwprintf(exe_args, 256, L"BF1942.exe +restart 1 +joinServer %hs:%d +game %hs", cJSON_GetStringValue(IP), (int)cJSON_GetNumberValue(hostport), cJSON_GetStringValue(gameId)); // %hs is MSVC specific
    

    WCHAR bf_dir[1024] = {0};
    WCHAR* dirslash = wcsrchr(bf1942_path, '\\');
    if(dirslash != 0){
        for(int i = 0; &bf1942_path[i] != dirslash; i++){
            bf_dir[i] = bf1942_path[i];
        }
    }
    wprintf(L"executing \"%ls\" %ls\n", bf1942_path, exe_args);
    wprintf(L"in dir \"%s\"\n", bf_dir);

    STARTUPINFO sui = {0};
    sui.cb = sizeof(sui);
    PROCESS_INFORMATION pi = {0};
    bool ok = CreateProcess(bf1942_path, exe_args, 0, 0, false, CREATE_NEW_PROCESS_GROUP, 0, bf_dir, &sui, &pi);
    if(!ok){
        printf("CreateProcess failed: %d\n", GetLastError());
    }
    else {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void __stdcall ListViewNotify(NMHDR* notify)
{
    //printf("WM_NOTIFY: code=%d hwnd=%p id=%d\n", notify->code, notify->hwndFrom, notify->idFrom);
    NMITEMACTIVATE* act = (NMITEMACTIVATE*)notify;
    switch(notify->code){
        case NM_CLICK:
            printf("clicked %d %d\n", act->iItem, act->iSubItem);
            break;
        case NM_DBLCLK:
            ConnectToServer();
            break;
        case LVN_ODSTATECHANGED:{
            NMLVODSTATECHANGE* ch = (NMLVODSTATECHANGE*)notify;
            //printf("statechange %08X -> %08X %d-%d\n", ch->uOldState, ch->uNewState, ch->iFrom, ch->iTo);
            if((ch->uNewState & LVIS_SELECTED) && !(ch->uOldState & LVIS_SELECTED)){
                if(ch->iFrom != ch->iTo)MessageBox(0, L"wat", L"????", MB_ICONQUESTION);
                SelectServer(ch->iFrom);
            }
            break;
        }
        case LVN_ITEMCHANGED:{
            NMLISTVIEW* lv = (NMLISTVIEW*)notify;
            //printf("itemchanged %d %d %08X -> %08X\n", lv->iItem, lv->iSubItem, lv->uOldState, lv->uNewState);
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
            printf("WM_COMMAND: %d\n", wParam);
            if(wParam == IDCLOSE){
                DestroyWindow(hwnd);
            } else if(wParam == ID_REFRESHBTN){
                ReloadServers();
            }
            break;
        case WM_SETFOCUS:
            SetTimer(mainwindow, ID_PULSE_TIMER, 200, 0);
            SetTimer(mainwindow, ID_SECOND_TIMER, 1000, 0);
            break;
        case WM_KILLFOCUS:
            KillTimer(mainwindow, ID_PULSE_TIMER);
            KillTimer(mainwindow, ID_SECOND_TIMER);
            break;
        case WM_NOTIFY:{
            if(wParam == 100){
                ListViewNotify((NMHDR*)lParam);
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
        printf("failed to create winhttp session\n");
        goto cleanup;
    }
    connect = WinHttpConnect(session, L"master.bf1942.org", INTERNET_DEFAULT_PORT, 0);
    if(connect == 0){
        printf("failed to create winhttp connect\n");
        goto cleanup;
    }
    // flag WINHTTP_FLAG_SECURE may be useful later
    // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpopenrequest
    request = WinHttpOpenRequest(connect, L"GET", L"/json/?full", 0 /* HTTP 1.1 */, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if(request == 0){
        printf("failed to create winhttp request\n");
        goto cleanup;
    }
    // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpsendrequest
    result = WinHttpSendRequest(request, 0, 0, 0, 0, 0, 0);
    if(!result){
        printf("request failed!\n");
        goto cleanup;
    }

    // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpreceiveresponse
    result = WinHttpReceiveResponse(request, 0);
    if(!result){
        printf("receive response failed!\n");
        goto cleanup;
    }

    DWORD body_length = 0;
    for(;;){
        DWORD bytes;
        // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpquerydataavailable
        if(!WinHttpQueryDataAvailable(request, &bytes)){
            printf("query data available failed\n");
            result = false; goto cleanup;
        }
        if(bytes == 0)break;
        //printf("receiving %u bytes\n", bytes);
        DWORD new_body_length = body_length + bytes;
        if(new_body_length > 1024*1024*64){
            // a 64MB json file? huh this game was never that popular
            printf("response json data too big!\n");
            result = false; goto cleanup;
        }
        body = realloc(body, new_body_length + 1);
        if(body == 0){
            printf("get_server_list realloc failed\n");
            result = false; goto cleanup;
        }
        DWORD bytes_read;
        WinHttpReadData(request, body + body_length, bytes, &bytes_read);
        body_length += bytes_read;
        //printf("read %u bytes\n", bytes_read);
    }
    body[body_length] = 0;
    printf("got server list json, %u bytes\n", body_length);
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

void LoadServerList(char* json, DWORD length)
{
    cJSON* root = cJSON_ParseWithLength(json, length);
    if(root == 0){
        printf("failed to parse root json object\n");
        return;
    }
    printf("parsed root object of type %d\n", root->type);
    if(!cJSON_IsArray(root)){
        printf("root object is not array!\n");
        goto cleanup;
    }

    ListView_DeleteAllItems(serverlist);
    ListView_DeleteAllItems(playerlist);
    SetWindowText(addresslabel, L"Select a server");

    for(int i = 0; i < cJSON_GetArraySize(root); i++){
        cJSON* server = cJSON_GetArrayItem(root, i);
        cJSON* query = cJSON_GetObjectItem(server, "query");
        if(!query) { printf("malformed server data\n"); continue; }
        cJSON* hostname = cJSON_GetObjectItem(query, "hostname");
        cJSON* maxplayers = cJSON_GetObjectItem(query, "maxplayers");
        cJSON* numplayers = cJSON_GetObjectItem(query, "numplayers");
        cJSON* mapname = cJSON_GetObjectItem(query, "mapname");
        cJSON* gametype = cJSON_GetObjectItem(query, "gametype");
        cJSON* gameId = cJSON_GetObjectItem(query, "gameId");
        if(!(hostname && maxplayers && numplayers && mapname && gametype && gameId)) { printf("malformed query data\n"); continue; }

        WCHAR players[32];
        _snwprintf(players, 32, L"%d / %d", (int)cJSON_GetNumberValue(numplayers), (int)cJSON_GetNumberValue(maxplayers));

        //printf("%-32s %3d / %-3d %s\n", cJSON_GetStringValue(hostname), (int)cJSON_GetNumberValue(numplayers), (int)cJSON_GetNumberValue(maxplayers), cJSON_GetStringValue(mapname));

        LV_ITEM li = {0};
        li.mask = LVIF_TEXT;
        li.iItem = i; // row number
        li.iSubItem = 0; // server name column
        li.pszText = utf8ToWide(cJSON_GetStringValue(hostname));
        ListView_InsertItem(serverlist, &li);
        li.iSubItem = 1; // players
        li.pszText = players;
        ListView_SetItem(serverlist, &li);
        li.iSubItem = 2; // ping
        li.pszText = L"TODO";
        ListView_SetItem(serverlist, &li);
        li.iSubItem = 3; // map
        li.pszText = utf8ToWide(cJSON_GetStringValue(mapname));
        ListView_SetItem(serverlist, &li);
        li.iSubItem = 4; // gamemode
        li.pszText = utf8ToWide(cJSON_GetStringValue(gametype));
        ListView_SetItem(serverlist, &li);
        li.iSubItem = 5; // mod
        li.pszText = utf8ToWide(cJSON_GetStringValue(gameId));
        ListView_SetItem(serverlist, &li);

    }
    serverdata = root;
    return;
cleanup:
    if(root != 0)cJSON_Delete(root);
}

void ReloadServers()
{
    DWORD server_list_json_length;
    char* server_list_json = GetServerList(&server_list_json_length);
    if(server_list_json){
        LoadServerList(server_list_json, server_list_json_length);
        free(server_list_json);
        server_list_json = 0;
    }
    else {
        MessageBox(mainwindow, L"Failed to download server list", L"Error", MB_ICONERROR);
    }
}

void PulseSecond()
{
    printf("second\n");
}

void PulseTimer()
{
    printf("pulse\n");
}

int __stdcall WinMain(HINSTANCE instance, HINSTANCE previnstance, LPSTR commandline, int cmdshow)
{
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
    wndclass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);//CreateSolidBrush(RGB(0xf0, 0xf0, 0xf0)); GetSysColorBrush(COLOR_WINDOW);
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
    // NONCLIENTMETRICS ncm;
    // ncm.cbSize = sizeof(NONCLIENTMETRICS);
    // SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &ncm, 0);
    // HFONT hFont = CreateFontIndirect(&ncm.lfMessageFont);
    // SendMessage(mainwindow, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));

    RECT clientarea;
    GetClientRect(mainwindow, &clientarea);
    int csizex = clientarea.right-clientarea.left, csizey = clientarea.bottom-clientarea.top;

    CreateWindow(L"BUTTON", L"Close", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, csizex - 110, csizey - 40, 100, 30, mainwindow, (HMENU)IDCLOSE, instance, 0);
    CreateWindow(L"BUTTON", L"Refresh", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 680, csizey - 40, 100, 30, mainwindow, (HMENU)ID_REFRESHBTN, instance, 0);

    addresslabel = CreateWindow(WC_STATIC, L"Select a server", WS_CHILD | WS_VISIBLE, 10, csizey-35, 170, 16, mainwindow, (HMENU)ID_ADDRESSLABEL, instance, 0);
    
    // SendMessage(button, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
    // SendMessage(bittom, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
    // SendMessage(addresslabel, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));

    // LVS_LIST means we get a details view with columns
    serverlist = CreateWindow(WC_LISTVIEW,  L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL, 10, 10, 660, csizey-50, mainwindow, (HMENU)ID_SERVERLIST, instance, 0);
    ListView_SetExtendedListViewStyle(serverlist, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_HEADERDRAGDROP);
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
    col.cx = 170;
    ListView_InsertColumn(playerlist, col.iSubItem, &col);
    col.iSubItem = 1;
    col.pszText = L"Score";
    col.cx = 40;
    ListView_InsertColumn(playerlist, col.iSubItem, &col);
    col.iSubItem = 2;
    col.pszText = L"Ping";
    col.cx = 40;
    ListView_InsertColumn(playerlist, col.iSubItem, &col);

    ShowWindow(mainwindow, cmdshow);

    //ReloadServers();

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