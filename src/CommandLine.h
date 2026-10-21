#pragma once
#include <string>
#include <vector>

namespace elm
{
    int RunCommandLine(const std::vector<std::wstring> &arguments);
    std::wstring CommandLineHelp();
}
