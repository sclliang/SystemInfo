#include "Collection.h"
#include "DiagnosticReport.h"
#include "LogCollection.h"
#include "Report.h"
#include "WindowPlacement.h"
#include "firmware/AmiFirmware.h"
#include <shellapi.h>
#include <cstdint>
#include <memory>

// The WPF owner serializes these entry points. Slow reads stay on the
// existing native workers. No STL object or exception crosses the C ABI.
namespace
{
using namespace system_info;
using elm::JsonQuote;
const wchar_t* JsonBool(bool value) { return value ? L"true" : L"false"; }
std::wstring BatteryJson(const std::optional<power::Reading>& battery)
{
    if (!battery) return L"null";
    std::wstring json = L"{\"state\":" + JsonQuote(battery->state)
        + L",\"source\":" + JsonQuote(battery->source) + L",\"remaining\":" + JsonQuote(battery->remaining)
        + L",\"present\":" + (battery->present ? JsonBool(*battery->present) : L"null")
        + L",\"percent\":" + (battery->percent ? std::to_wstring(*battery->percent) : L"null") + L",\"packs\":[";
    bool first = true;
    for (const auto& pack : battery->packs)
    {
        if (!first) json += L",";
        first = false;
        json += L"{\"name\":" + JsonQuote(pack.name) + L",\"fields\":[";
        for (std::size_t i = 0; i < pack.fields.size(); ++i)
        {
            if (i) json += L",";
            const auto& [label, value] = pack.fields[i];
            json += L"{\"label\":" + JsonQuote(label) + L",\"value\":" + JsonQuote(value) + L"}";
        }
        json += L"]}";
    }
    return json + L"]}";
}

std::wstring ComputerName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(std::size(name));
    return GetComputerNameW(name, &size) ? std::wstring(name, size) : L"本机";
}

struct Session
{
    Collection hardware;
    LogCollection logs;
    CollectionUpdate info{{}, true, true};
    std::optional<LogUpdate> log;
    std::wstring updated;
    std::wstring machine = ComputerName();
    std::string json;
    std::uint64_t revision{1};

    bool Refresh()
    {
        if (!hardware.Start(firmware::CollectComputerInfo, storage::CollectSsdInfo)) return false;
        info.computerPending = info.ssdPending = true;
        ++revision;
        return true;
    }
    void Advance()
    {
        if (auto next = hardware.Take())
        {
            info = std::move(*next);
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t time[12]{};
            swprintf_s(time, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
            updated = time;
            ++revision;
        }
        if (auto next = logs.Take()) { log = std::move(next); ++revision; }
    }
    std::wstring LogJson() const
    {
        if (!log) return L"null";
        const wchar_t* state = L"running";
        switch (log->state)
        {
        case LogState::Running: break;
        case LogState::Cancelling: state = L"cancelling"; break;
        case LogState::Complete: state = L"complete"; break;
        case LogState::Partial: state = L"partial"; break;
        case LogState::Failed: state = L"failed"; break;
        case LogState::Cancelled: state = L"cancelled"; break;
        }
        return L"{\"state\":" + JsonQuote(state) + L",\"item\":" + JsonQuote(log->item) +
            L",\"completed\":" + std::to_wstring(log->completed) + L",\"total\":" + std::to_wstring(log->total) +
            L",\"saved\":" + std::to_wstring(log->saved) + L",\"partial\":" + std::to_wstring(log->partial) +
            L",\"failed\":" + std::to_wstring(log->failed) + L",\"detail\":" + JsonQuote(log->detail) +
            L",\"directory\":" + JsonQuote(log->directory.wstring()) +
            L",\"hasReport\":" + JsonBool(!log->report.empty()) + L"}";
    }
    std::string Serialize() const
    {
        std::wstring rows = L"[";
        for (const auto& row : info.snapshot.rows)
        {
            if (rows.size() > 1) rows += L",";
            rows += L"{\"label\":" + JsonQuote(row.label) + L",\"value\":" + JsonQuote(row.value) + L"}";
        }
        return elm::Utf8(L"{\"revision\":" + std::to_wstring(revision) +
            L",\"machine\":" + JsonQuote(machine) +
            L",\"computerPending\":" + JsonBool(info.computerPending) + L",\"ssdPending\":" + JsonBool(info.ssdPending) +
            L",\"updated\":" + JsonQuote(updated) + L",\"diagnostic\":" + JsonQuote(info.snapshot.diagnostic) +
            L",\"error\":" + JsonQuote(info.snapshot.error) + L",\"rows\":" + rows + L"],\"battery\":" +
            BatteryJson(info.snapshot.battery) + L",\"log\":" + LogJson() + L"}");
    }
    ~Session() { hardware.Stop(); logs.Cancel(); }
};
}

extern "C" void* si_new() noexcept
{
    try { auto session = std::make_unique<Session>(); session->Refresh(); return session.release(); }
    catch (...) { return nullptr; }
}
extern "C" void si_drop(void* ptr) noexcept { delete static_cast<Session*>(ptr); }

// 0 = busy/unavailable, 1 = accepted, -1 = unexpected failure.
extern "C" int si_action(void* ptr, unsigned action, HWND owner) noexcept
{
    try
    {
        auto& session = *static_cast<Session*>(ptr);
        switch (action)
        {
        case 1: return session.Refresh();
        case 2:
            if (!session.logs.Start()) return 0;
            session.log = LogUpdate{};
            ++session.revision;
            return 1;
        case 3:
            if (!session.log || session.log->Finished()) return 0;
            session.logs.Cancel();
            session.log->state = LogState::Cancelling;
            ++session.revision;
            return 1;
        case 4:
            if (session.info.computerPending || session.info.snapshot.rows.empty()) return 0;
            return CopyText(owner, FormatReport(session.info.snapshot));
        case 5:
        case 6:
        {
            if (!session.log || !session.log->Finished()) return 0;
            const auto& path = action == 5 ? session.log->report : session.log->directory;
            if (path.empty() || !std::filesystem::exists(path)) return 0;
            return reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        }
        default: return 0;
        }
    }
    catch (...) { return -1; }
}

// Borrowed UTF-8 until the next poll. The owner copies it before another call.
extern "C" const char* si_poll(void* ptr, std::uint64_t since) noexcept
{
    try
    {
        auto& session = *static_cast<Session*>(ptr);
        session.Advance();
        if (since == session.revision) return "";
        session.json = session.Serialize();
        return session.json.c_str();
    }
    catch (...) { return nullptr; }
}
extern "C" int si_export(const wchar_t* path, int amiOnly) noexcept
{
    try { return WriteDiagnosticReport(path, amiOnly != 0); }
    catch (...) { return 1; }
}
extern "C" int si_position(HWND hwnd, double widthDip, double heightDip) noexcept
{
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info)) return 0;
    const auto place = [&](UINT dpi) {
        const SIZE requested{static_cast<LONG>(widthDip * dpi / 96.0), static_cast<LONG>(heightDip * dpi / 96.0)};
        const RECT bounds = system_info::InitialWindowBounds(info.rcWork, requested);
        return SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
            bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    };
    const UINT initialDpi = GetDpiForWindow(hwnd);
    if (!place(initialDpi)) return 0;
    // Moving to another monitor can resize the hidden window during WM_DPICHANGED.
    const UINT targetDpi = GetDpiForWindow(hwnd);
    return initialDpi == targetDpi || place(targetDpi);
}
