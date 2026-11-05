#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace system_info::firmware::detail
{
std::optional<std::filesystem::path> OwnedDriverDirectory(
    std::wstring imagePath, const std::filesystem::path& temporaryRoot);
bool AnotherAmiProcessRunning();
// Empty directory means recover old SystemInfo-owned services. Foreign paths
// are never stopped/deleted. Returns cleared directories for bounded file cleanup.
bool CleanupOwnedDrivers(const std::filesystem::path& onlyDirectory,
    std::vector<std::filesystem::path>& cleared, std::wstring& error);
std::wstring DeleteOwnedFiles(const std::filesystem::path& directory);
}
