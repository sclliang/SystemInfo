#include <windows.h>
#include "CommandLine.h"
#include "ConsoleUi.h"
#include "EventLogService.h"

int wmain(int argc, wchar_t **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    elm::EnableSecurityPrivilege();
    std::vector<std::wstring> args(argv + 1, argv + argc);
    // "gui" is kept for compatibility with earlier shortcuts and scripts.
    if (args.empty() || (args.size() == 1 && args[0] == L"gui"))
        return elm::RunConsoleCollection(L"", {}, true);
    return elm::RunCommandLine(args);
}
