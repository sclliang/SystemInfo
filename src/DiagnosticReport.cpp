#include "DiagnosticReport.h"
#include "Report.h"
#include "WindowsLicense.h"
#include "firmware/AmiFirmware.h"
#include "storage/SsdInfo.h"

namespace system_info
{
int WriteDiagnosticReport(const std::filesystem::path& path, bool amiOnly)
{
    if (amiOnly)
    {
        try
        {
            const auto ami = system_info::firmware::ReadEmbeddedAmi({});
            std::wstring text = L"AMIDEWIN 内嵌固件读取验证\r\n";
            text += L"版本：" SYSTEMINFO_VERSION L"\r\n";
            text += L"已解析字符串字段：" + std::to_wstring(ami.values.commands.size()) + L" / 7\r\n";
            for (const auto& [command, label] : {std::pair{L"SU", L"UUID"},
                std::pair{L"SS", L"系统SN"}, std::pair{L"BS", L"主板SN"}})
            {
                const auto found = ami.values.commands.find(command);
                text += std::wstring(label) + L"（AMIDEWIN /" + command + L"）：" +
                    (found == ami.values.commands.end() ? L"读取失败，界面使用原采集方案" : found->second) + L"\r\n";
            }
            text += L"EC版本：" + (ami.values.ecVersion.empty() ? L"N/A" : ami.values.ecVersion) + L"\r\n";
            text += L"诊断：" + (ami.diagnostic.empty() ? L"无" : ami.diagnostic) + L"\r\n\r\n" + ami.evidence;
            if (!system_info::WriteText(path, text)) return 3;
            return ami.values.commands.empty() ? 1 : 0;
        }
        catch (...) { return 1; }
    }
    {
        try
        {
            auto snapshot = system_info::CollectSystemInfo();
            try { system_info::storage::ApplySsdInfo(snapshot, system_info::storage::CollectSsdInfo()); }
            catch (...) { system_info::storage::ApplySsdInfo(snapshot, {L"N/A", L"SSD 名称与容量读取失败。"}); }
            if (!system_info::WriteReport(path, snapshot)) return 3;
            return snapshot.error.empty() ? 0 : 1;
        }
        catch (...) { return 1; }
    }
}
}
