#pragma once
#include <filesystem>
namespace system_info
{
int WriteDiagnosticReport(const std::filesystem::path& path, bool amiOnly);
}
