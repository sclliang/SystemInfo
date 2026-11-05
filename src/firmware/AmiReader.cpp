#include "AmiReader.h"
#include "AmiProcess.h"
#include <array>

namespace system_info::firmware::detail
{
namespace
{
void Append(std::wstring& text, const std::wstring& line)
{
    if (line.empty()) return;
    if (!text.empty()) text += L"\r\n";
    text += line;
}
struct IdentityField
{
    ReadMode mode;
    const wchar_t* command;
    const wchar_t* label;
};
constexpr std::array<IdentityField, 3> kIdentityFields{{
    {ReadMode::SystemUuid, L"SU", L"UUID"},
    {ReadMode::SystemSerial, L"SS", L"系统SN"},
    {ReadMode::BoardSerial, L"BS", L"主板SN"}}};
}

FirmwareRead ReadAmiFirmware(const std::filesystem::path& executable,
    const std::filesystem::path& directory, std::stop_token token, unsigned long timeoutMilliseconds)
{
    FirmwareRead read;
    auto& result = read.result;
    const auto run = [&](ReadMode mode) {
        auto output = RunAmiRead(executable, directory, mode, token, timeoutMilliseconds);
        Append(result.evidence, ReadEvidence(output, mode));
        if (output.started && !output.exited) read.allProcessesExited = false;
        return output;
    };

    // Read each identifier directly. A failed query must not discard identifiers
    // already returned by the other two queries or substitute /ALL's copy.
    for (const auto& field : kIdentityFields)
    {
        if (token.stop_requested() || !read.allProcessesExited) return read;
        const auto output = run(field.mode);
        const auto parsed = ParseAmiStrings(output.output);
        if (const auto found = parsed.commands.find(field.command); found != parsed.commands.end())
            result.values.commands.emplace(found->first, found->second);
        else
            Append(result.diagnostic, std::wstring(L"AMIDEWIN /") + field.command + L" 未读到有效的" +
                field.label + L"，该项使用原采集方案。");
        if (!output.success)
            Append(result.diagnostic, std::wstring(L"/") + field.command + L"：" + output.error);
    }
    if (token.stop_requested() || !read.allProcessesExited) return read;

    // Bulk reads supply the remaining firmware fields only. They cannot overwrite
    // /SU, /SS or /BS, including when a direct query was unavailable.
    const auto strings = run(ReadMode::Strings);
    const auto parsed = ParseAmiStrings(strings.output + L"\r\n" + strings.console);
    for (const auto key : {L"IVN", L"IV", L"ID", L"BP"})
        if (const auto found = parsed.commands.find(key); found != parsed.commands.end())
            result.values.commands.emplace(found->first, found->second);
    if (!strings.success)
        Append(result.diagnostic, L"/ALL：" + strings.error + L" 未读到的其他固件字段使用原采集方案。");
    if (strings.success && !token.stop_requested() && read.allProcessesExited)
    {
        const auto dump = run(ReadMode::AllData);
        result.values.ecVersion = ParseEcDump(dump.output);
        if (!dump.success) Append(result.diagnostic, L"/DUMPALL：" + dump.error + L" EC 缺失时使用原采集方案。");
    }
    if (result.values.commands.empty() && strings.success)
        Append(result.diagnostic, L"AMIDEWIN 未返回有效的固件字段，已使用原采集方案。");
    return read;
}
}
