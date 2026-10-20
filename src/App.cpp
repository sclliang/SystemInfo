#include <windows.h>
#include <shellapi.h>
#include "CommandLine.h"
#include "EventLogService.h"
#include "CollectorWindow.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    elm::EnableSecurityPrivilege();
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return 4;
    std::vector<std::wstring> args;
    for (int i = 1; i < count; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    if (args.empty() || (args.size() == 1 && args[0] == L"gui")) return elm::RunGui(instance);
    return elm::RunCommandLine(args);
}
