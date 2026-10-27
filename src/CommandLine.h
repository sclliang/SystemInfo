#pragma once
#include <string>
#include <vector>

namespace elm
{
    int RunCommandLine(const std::vector<std::wstring> &arguments);
    std::wstring CommandLineHelp();
    // Writes UTF-16 to a console, or UTF-8 when the stream is redirected.
    void WriteOutput(const std::wstring &value, bool error = false);
}
