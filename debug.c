#define UNICODE 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <DbgHelp.h>
#include <io.h>
#include <fcntl.h>
//#include <iostream>
#include <stdbool.h>
#include <stdio.h>


LONG __stdcall ExceptionFilter(LPEXCEPTION_POINTERS x){
    EXCEPTION_RECORD* xr = x->ExceptionRecord;
    //CONTEXT* ctx = x->ContextRecord;
    static WCHAR msg[128];
    int n = wsprintf(msg, L"\nexception %08X at %p", xr->ExceptionCode, xr->ExceptionAddress);
    if(xr->ExceptionCode == EXCEPTION_ACCESS_VIOLATION){
        n += wsprintf(msg + n, xr->ExceptionInformation[0] == 0 ? L" reading %p\n" : L" writing %p\n", (void*)xr->ExceptionInformation[1]);
    }
    MessageBox(0, msg, L"BF1942 Server browser", MB_ICONERROR);
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if(dbghelp != 0){
        typedef BOOL __stdcall MiniDumpWriteDump_t(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        MiniDumpWriteDump_t* pMiniDumpWriteDump = (MiniDumpWriteDump_t*)GetProcAddress(dbghelp, "MiniDumpWriteDump");
        if(pMiniDumpWriteDump != 0){
            HANDLE file = CreateFile(L"browser_crash.dmp", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
            if(file != 0){
                MINIDUMP_EXCEPTION_INFORMATION excinfo;
                excinfo.ThreadId = GetCurrentThreadId();
                excinfo.ExceptionPointers = x;
                excinfo.ClientPointers = FALSE;
                pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal | MiniDumpWithFullMemoryInfo | MiniDumpWithFullMemory, &excinfo, 0, 0);
                CloseHandle(file);
                return EXCEPTION_EXECUTE_HANDLER;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InitCrashHandler()
{
    SetUnhandledExceptionFilter(ExceptionFilter);
}


// from https://stackoverflow.com/questions/311955/redirecting-cout-to-a-console-in-windows
void BindCrtHandlesToStdHandles(bool bindStdIn, bool bindStdOut, bool bindStdErr)
{
    // Re-initialize the C runtime "FILE" handles with clean handles bound to "nul". We do this because it has been
    // observed that the file number of our standard handle file objects can be assigned internally to a value of -2
    // when not bound to a valid target, which represents some kind of unknown internal invalid state. In this state our
    // call to "_dup2" fails, as it specifically tests to ensure that the target file number isn't equal to this value
    // before allowing the operation to continue. We can resolve this issue by first "re-opening" the target files to
    // use the "nul" device, which will place them into a valid state, after which we can redirect them to our target
    // using the "_dup2" function.
    if (bindStdIn)
    {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "r", stdin);
    }
    if (bindStdOut)
    {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "w", stdout);
    }
    if (bindStdErr)
    {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "w", stderr);
    }

    // Redirect unbuffered stdin from the current standard input handle
    if (bindStdIn)
    {
        HANDLE stdHandle = GetStdHandle(STD_INPUT_HANDLE);
        if(stdHandle != INVALID_HANDLE_VALUE)
        {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if(fileDescriptor != -1)
            {
                FILE* file = _fdopen(fileDescriptor, "r");
                if(file != NULL)
                {
                    int dup2Result = _dup2(_fileno(file), _fileno(stdin));
                    if (dup2Result == 0)
                    {
                        setvbuf(stdin, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Redirect unbuffered stdout to the current standard output handle
    if (bindStdOut)
    {
        HANDLE stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if(stdHandle != INVALID_HANDLE_VALUE)
        {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if(fileDescriptor != -1)
            {
                FILE* file = _fdopen(fileDescriptor, "w");
                if(file != NULL)
                {
                    int dup2Result = _dup2(_fileno(file), _fileno(stdout));
                    if (dup2Result == 0)
                    {
                        setvbuf(stdout, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Redirect unbuffered stderr to the current standard error handle
    if (bindStdErr)
    {
        HANDLE stdHandle = GetStdHandle(STD_ERROR_HANDLE);
        if(stdHandle != INVALID_HANDLE_VALUE)
        {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if(fileDescriptor != -1)
            {
                FILE* file = _fdopen(fileDescriptor, "w");
                if(file != NULL)
                {
                    int dup2Result = _dup2(_fileno(file), _fileno(stderr));
                    if (dup2Result == 0)
                    {
                        setvbuf(stderr, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Clear the error state for each of the C++ standard stream objects. We need to do this, as attempts to access the
    // standard streams before they refer to a valid target will cause the iostream objects to enter an error state. In
    // versions of Visual Studio after 2005, this seems to always occur during startup regardless of whether anything
    // has been read from or written to the targets or not.
    if (bindStdIn)
    {
        //std::wcin.clear();
        //std::cin.clear();
    }
    if (bindStdOut)
    {
        //std::wcout.clear();
        //std::cout.clear();
    }
    if (bindStdErr)
    {
        //std::wcerr.clear();
        //std::cerr.clear();
    }
}

void InitConsole()
{
    AllocConsole();
    BindCrtHandlesToStdHandles(true, true, true);
}
