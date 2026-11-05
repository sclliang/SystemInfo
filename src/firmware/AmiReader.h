#pragma once
#include "AmiFirmware.h"
#include <filesystem>

namespace system_info::firmware::detail
{
struct FirmwareRead
{
    AmiResult result;
    bool allProcessesExited{true};
};
// Shared by the embedded-tool runner and subprocess regression tests.
FirmwareRead ReadAmiFirmware(const std::filesystem::path& executable,
    const std::filesystem::path& directory, std::stop_token token = {},
    unsigned long timeoutMilliseconds = 15000);
}
