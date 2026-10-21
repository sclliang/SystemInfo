#include "Collector.h"
#include "Diagnostics.h"
#include <algorithm>
#include <filesystem>

namespace elm {
std::vector<const CollectedLog*> CollectionEntries(const CollectionResult& result) {
    std::vector<const CollectedLog*> items;
    for (const auto& item : result.logs) items.push_back(&item);
    for (const auto& item : result.diagnostics) items.push_back(&item);
    return items;
}
std::vector<CollectedLog*> CollectionEntries(CollectionResult& result) {
    std::vector<CollectedLog*> items;
    for (auto& item : result.logs) items.push_back(&item);
    for (auto& item : result.diagnostics) items.push_back(&item);
    return items;
}
size_t CollectionResult::count(CollectionState state) const {
    const auto items = CollectionEntries(*this);
    return static_cast<size_t>(std::count_if(items.begin(), items.end(), [state](const auto* item) { return item->state == state; }));
}
size_t CollectionResult::saved() const { return count(CollectionState::Saved); }
size_t CollectionResult::completed() const { return total() - count(CollectionState::Waiting) - count(CollectionState::Running); }
bool CollectionResult::success() const {
    if (!finished || cancelled || !manifestSaved) return false;
    const auto items = CollectionEntries(*this);
    return std::all_of(items.begin(), items.end(), [](const auto* item) {
        return item->state == CollectionState::Saved || item->state == CollectionState::NotFound || item->state == CollectionState::Skipped;
    });
}
std::wstring CollectionStateName(CollectionState state) {
    switch (state) {
    case CollectionState::Waiting: return L"waiting";
    case CollectionState::Running: return L"running";
    case CollectionState::Saved: return L"saved";
    case CollectionState::Failed: return L"failed";
    case CollectionState::Cancelled: return L"cancelled";
    case CollectionState::NotFound: return L"not_found";
    case CollectionState::Skipped: return L"skipped";
    case CollectionState::Partial: return L"partial";
    }
    return L"failed";
}
std::wstring CollectedFilesJson(const std::vector<CollectedFile>& files) {
    std::wstring json = L"[";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        if (i) json += L",";
        json += L"\n      {\"source\":" + JsonQuote(file.source) + L",\"path\":" + JsonQuote(file.path) +
            L",\"status\":" + JsonQuote(CollectionStateName(file.state)) + L",\"bytes\":" + std::to_wstring(file.bytes) +
            L",\"modifiedUtc\":" + JsonQuote(file.modifiedUtc) + L",\"errorCode\":" + std::to_wstring(file.error) +
            L",\"message\":" + JsonQuote(file.message) + L"}";
    }
    return json + L"\n    ]";
}
std::wstring DefaultLogDirectory() {
    std::wstring path(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw Error(length ? ERROR_FILENAME_EXCED_RANGE : GetLastError(), L"Locate executable");
    path.resize(length);
    return (std::filesystem::path(path).parent_path() / L"Log").wstring();
}
CollectionResult InitialCollection(const std::wstring& directory) {
    CollectionResult result;
    result.rootDirectory = directory.empty() ? DefaultLogDirectory() : AbsolutePath(directory);
    result.directory = result.rootDirectory;
    const wchar_t* channels[]{L"System", L"Application", L"Security", L"Setup", L"ForwardedEvents"};
    const wchar_t* captions[]{L"系统事件", L"应用程序事件", L"安全事件", L"安装事件", L"转发的事件"};
    for (size_t i = 0; i < result.logs.size(); ++i) {
        result.logs[i].channel = channels[i]; result.logs[i].caption = captions[i];
    }
    const std::pair<const wchar_t*, const wchar_t*> diagnostics[]{
        {L"CrashDumps", L"蓝屏转储"}, {L"LiveKernelReports", L"实时内核转储"}, {L"WerReports", L"Windows 故障报告"},
        {L"SystemInfo", L"系统与硬件"}, {L"Devices", L"设备状态与问题代码"}, {L"Drivers", L"驱动与签名信息"},
        {L"DumpConfiguration", L"转储与分页文件配置"}, {L"DriverInstallation", L"设备驱动安装日志"},
        {L"DiagnosticEvents", L"设备、驱动与更新事件"}, {L"Timeline", L"故障时间线"}, {L"PowerReports", L"电源诊断（可选）"}
    };
    for (const auto& [id, caption] : diagnostics) {
        CollectedLog item; item.channel = id; item.caption = caption; result.diagnostics.push_back(std::move(item));
    }
    return result;
}
CollectionResult CollectWindowsLogs(const std::wstring& directory,
    const std::function<void(const CollectionResult&)>& progress, const std::atomic_bool* cancel, const CollectionOptions& options) {
    if (!options.days || options.days > 365) throw Error(ERROR_INVALID_PARAMETER, L"Collection days must be between 1 and 365");
    auto result = InitialCollection(directory); result.options = options;
    const auto started = GetTickCount64();
    wchar_t computer[256]{}; DWORD computerLength = static_cast<DWORD>(std::size(computer));
    if (GetComputerNameExW(ComputerNameDnsHostname, computer, &computerLength)) result.computerName = computer;
    FILETIME now{}; GetSystemTimeAsFileTime(&now); result.startedUtc = DiagnosticUtc(FileTimeValue(now));
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    const DWORD zoneState = GetDynamicTimeZoneInformation(&zone);
    if (zoneState != TIME_ZONE_ID_INVALID) {
        result.timeZone = zone.TimeZoneKeyName;
        result.utcOffsetMinutes = -static_cast<int>(zone.Bias +
            (zoneState == TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias : zone.StandardBias));
    }
    auto publish = [&] { result.elapsedMs = GetTickCount64() - started; if (progress) progress(result); };
    try {
        std::filesystem::create_directories(result.rootDirectory);
        SYSTEMTIME time{}; GetLocalTime(&time);
        wchar_t stamp[80]{};
        swprintf_s(stamp, L"Collection_%04u%02u%02u_%02u%02u%02u_%03u-%lu", time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId());
        for (unsigned attempt = 0;; ++attempt) {
            const auto candidate = std::filesystem::path(result.rootDirectory) /
                (std::wstring(stamp) + (attempt ? L"-" + std::to_wstring(attempt) : L""));
            if (CreateDirectoryW(candidate.c_str(), nullptr)) { result.directory = candidate.wstring(); break; }
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS || attempt >= 100) throw Error(error, L"Create collection directory");
        }
        result.manifestPath = (std::filesystem::path(result.directory) / L"manifest.json").wstring();
        std::filesystem::create_directory(std::filesystem::path(result.directory) / L"Events");
        auto context = MakeDiagnosticContext(result, cancel);
        publish();
        auto run = [&](CollectedLog& item, const auto& operation) {
            if (cancel && cancel->load()) { result.cancelled = true; item.state = CollectionState::Cancelled; publish(); return; }
            item.state = CollectionState::Running; publish();
            try {
                operation();
                if (item.state == CollectionState::Running) item.state = CollectionState::Saved;
            } catch (const Error& error) {
                item.error = error.code; item.message = error.text;
                if (error.code == ERROR_CANCELLED) { result.cancelled = true; item.state = CollectionState::Cancelled; }
                else item.state = CollectionState::Failed;
            } catch (const std::filesystem::filesystem_error& error) {
                item.state = CollectionState::Failed; item.error = static_cast<DWORD>(error.code().value());
                item.message = L"文件操作失败：" + SystemError(item.error);
            } catch (const std::exception&) {
                item.state = CollectionState::Failed; item.error = ERROR_GEN_FAILURE; item.message = L"采集时发生意外错误。";
            }
            publish();
        };
        for (auto& log : result.logs) {
            log.path = (std::filesystem::path(result.directory) / L"Events" / (log.channel + L".evtx")).wstring();
            run(log, [&] {
                Query query; query.source = log.channel; query.limit = 0;
                Export(query, log.path, Format::Evtx, false, cancel);
                log.bytes = std::filesystem::file_size(log.path);
            });
        }
        for (auto& item : result.diagnostics) run(item, [&] { CollectDiagnostic(item, context, result); });
    } catch (const Error& error) {
        result.message = error.text;
        for (auto* item : CollectionEntries(result)) if (item->state == CollectionState::Waiting || item->state == CollectionState::Running) {
            item->state = CollectionState::Failed; item->error = error.code; item->message = error.text;
        }
    } catch (const std::filesystem::filesystem_error& error) {
        result.message = L"无法创建采集目录：" + SystemError(static_cast<DWORD>(error.code().value()));
        for (auto* item : CollectionEntries(result)) if (item->state == CollectionState::Waiting || item->state == CollectionState::Running) {
            item->state = CollectionState::Failed; item->error = static_cast<DWORD>(error.code().value()); item->message = result.message;
        }
    }
    if (cancel && cancel->load()) result.cancelled = true;
    result.finished = true; result.elapsedMs = GetTickCount64() - started;
    GetSystemTimeAsFileTime(&now); result.finishedUtc = DiagnosticUtc(FileTimeValue(now));
    if (!result.manifestPath.empty()) {
        try {
            result.manifestSaved = true;
            WriteDiagnosticFile(result.manifestPath, CollectionJson(result));
        } catch (const Error& error) {
            result.manifestSaved = false; result.manifestError = error.code; result.message += L"\n无法保存采集清单：" + error.text;
        } catch (const std::exception&) {
            result.manifestSaved = false; result.manifestError = ERROR_WRITE_FAULT; result.message += L"\n无法保存采集清单。";
        }
    }
    if (progress) progress(result);
    return result;
}
std::wstring CollectionJson(const CollectionResult& result) {
    std::wstring json = L"{\n  \"schemaVersion\":2,\n  \"toolVersion\":\"1.1.0.0\",\n  \"rootDirectory\":" + JsonQuote(result.rootDirectory) +
        L",\n  \"directory\":" + JsonQuote(result.directory) + L",\n  \"computerName\":" + JsonQuote(result.computerName) +
        L",\n  \"startedUtc\":" + JsonQuote(result.startedUtc) +
        L",\n  \"finishedUtc\":" + JsonQuote(result.finishedUtc) + L",\n  \"timeZone\":" + JsonQuote(result.timeZone) +
        L",\n  \"utcOffsetMinutes\":" + std::to_wstring(result.utcOffsetMinutes) +
        L",\n  \"success\":" + (result.success() ? L"true" : L"false") +
        L",\n  \"cancelled\":" + (result.cancelled ? L"true" : L"false") +
        L",\n  \"elapsedMs\":" + std::to_wstring(result.elapsedMs) + L",\n  \"error\":" + JsonQuote(result.message) +
        L",\n  \"manifestPath\":" + JsonQuote(result.manifestPath) + L",\n  \"manifestSaved\":" + (result.manifestSaved ? L"true" : L"false") +
        L",\n  \"manifestErrorCode\":" + std::to_wstring(result.manifestError) +
        L",\n  \"options\":{\"days\":" + std::to_wstring(result.options.days) +
        L",\"includeLargeDumps\":" + (result.options.includeLargeDumps ? L"true" : L"false") +
        L",\"powerReports\":" + (result.options.powerReports ? L"true" : L"false") + L"}";
    auto array = [&](const wchar_t* name, const auto& items) {
        json += L",\n  \"" + std::wstring(name) + L"\":[";
        bool first = true;
        for (const auto& item : items) {
            if (!first) json += L",";
            first = false;
            json += L"\n    {\"channel\":" + JsonQuote(item.channel) + L",\"caption\":" + JsonQuote(item.caption) +
                L",\"path\":" + JsonQuote(item.path) + L",\"status\":" + JsonQuote(CollectionStateName(item.state)) +
                L",\"bytes\":" + std::to_wstring(item.bytes) + L",\"errorCode\":" + std::to_wstring(item.error) +
                L",\"error\":" + JsonQuote(item.message) + L",\"files\":" + CollectedFilesJson(item.files) + L"}";
        }
        json += L"\n  ]";
    };
    array(L"logs", result.logs); array(L"diagnostics", result.diagnostics);
    return json + L"\n}\n";
}
}
