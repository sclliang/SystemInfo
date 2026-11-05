#pragma once
#include <filesystem>
#include <stop_token>
#include <string>

namespace system_info::firmware::detail
{
enum class ReadMode { SystemUuid, SystemSerial, BoardSerial, Strings, AllData };
struct ReadResult
{
    bool success{}, started{}, exited{};
    unsigned long exitCode{}, elapsedMilliseconds{};
    std::wstring output, console, error;
};
// Identity queries have exactly one switch and no value argument (read only).
// Bulk reads use their dedicated output-file argument. No write mode exists.
ReadResult RunAmiRead(const std::filesystem::path& executable,
    const std::filesystem::path& directory, ReadMode mode,
    std::stop_token token = {}, unsigned long timeoutMilliseconds = 15000);
std::wstring ReadEvidence(const ReadResult& result, ReadMode mode);
std::wstring ReadSmallTextFile(const std::filesystem::path& path);
}
