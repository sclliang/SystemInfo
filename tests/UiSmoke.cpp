#include "../src/CollectorWindow.h"
#include "../src/EventLogService.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    elm::EnableSecurityPrivilege();
    elm::GuiOptions options; options.validate = true;
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; args && i < count; ++i) {
        std::wstring value = args[i];
        if (value == L"--snapshot" && i + 1 < count) options.snapshot = args[++i];
        else if (value == L"--dark") options.dark = true;
        else if (value == L"--scenario" && i + 1 < count) options.scenario = args[++i];
        else if (value == L"--scale" && i + 1 < count) options.uiScale = static_cast<float>(_wtof(args[++i]));
    }
    if (args) LocalFree(args);
    return elm::RunGui(instance, options);
}
