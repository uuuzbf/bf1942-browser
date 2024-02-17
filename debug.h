
#ifdef DEBUG
#define dbgprintf   printf
#else
#define dbgprintf
#endif

void InitConsole();
void InitCrashHandler();