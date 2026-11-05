#include <Windows.h>
#include "Report.h"
#include <cstring>
#include <fstream>
#include <limits>

namespace system_info
{
std::wstring FormatReport(const ftc::hardware_info::Snapshot& snapshot)
{
    std::wstring text = L"电脑信息\r\n\r\n";
    for (const auto& row : snapshot.rows)
        text += row.label + L"：\r\n" + row.value + L"\r\n\r\n";
    if (!snapshot.error.empty()) text += L"采集失败：\r\n" + snapshot.error + L"\r\n";
    if (!snapshot.diagnostic.empty()) text += L"采集提示：\r\n" + snapshot.diagnostic + L"\r\n";
    return text;
}

bool WriteReport(const std::filesystem::path& path, const ftc::hardware_info::Snapshot& snapshot)
{
    return WriteText(path, FormatReport(snapshot));
}

bool WriteText(const std::filesystem::path& path, const std::wstring& text)
{
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    const int length = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    std::string bytes(static_cast<std::size_t>(required), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
        bytes.data(), required, nullptr, nullptr)) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write("\xEF\xBB\xBF", 3);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return !file.fail();
}

bool CopyText(HWND owner, const std::wstring& text)
{
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL buffer = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!buffer) return false;
    void* memory = GlobalLock(buffer);
    if (!memory) { GlobalFree(buffer); return false; }
    std::memcpy(memory, text.c_str(), bytes);
    GlobalUnlock(buffer);
    if (!OpenClipboard(owner)) { GlobalFree(buffer); return false; }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, buffer);
    CloseClipboard();
    if (!copied) GlobalFree(buffer); // Clipboard owns the memory on success.
    return copied;
}
}
