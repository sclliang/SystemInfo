#pragma once
#include <windows.h>
#include <string>

namespace elm {
// Diagnostic options are used only by the separately built UI test executable.
struct GuiOptions {
    bool validate = false, dark = false;
    float uiScale = 1;
    std::wstring snapshot, scenario = L"success";
};
int RunGui(HINSTANCE instance, const GuiOptions& options = {});
}
