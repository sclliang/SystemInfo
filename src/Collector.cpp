#include "Collector.h"
#include <algorithm>
#include <filesystem>

namespace elm {
size_t CollectionResult::saved() const {
    return static_cast<size_t>(std::count_if(logs.begin(), logs.end(), [](const auto& log) { return log.state == CollectionState::Saved; }));
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
    result.directory = directory.empty() ? DefaultLogDirectory() : AbsolutePath(directory);
    const wchar_t* channels[]{L"System", L"Application", L"Security", L"Setup", L"ForwardedEvents"};
    const wchar_t* captions[]{L"系统", L"应用程序", L"安全", L"安装", L"转发的事件"};
    for (size_t i = 0; i < result.logs.size(); ++i) {
        result.logs[i].channel = channels[i]; result.logs[i].caption = captions[i];
    }
    return result;
}

CollectionResult CollectWindowsLogs(const std::wstring& directory,
    const std::function<void(const CollectionResult&)>& progress, const std::atomic_bool* cancel) {
    auto result = InitialCollection(directory);
    const auto started = GetTickCount64();
    auto publish = [&] { result.elapsedMs = GetTickCount64() - started; if (progress) progress(result); };
    SYSTEMTIME time{}; GetLocalTime(&time);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%04u%02u%02u_%02u%02u%02u_%03u-%lu", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId());
    for (auto& log : result.logs)
        log.path = (std::filesystem::path(result.directory) / (log.channel + L"_" + stamp + L".evtx")).wstring();
    publish();
    std::error_code filesystemError;
    std::filesystem::create_directories(result.directory, filesystemError);
    if (filesystemError || !std::filesystem::is_directory(result.directory, filesystemError)) {
        const DWORD code = filesystemError ? static_cast<DWORD>(filesystemError.value()) : ERROR_DIRECTORY;
        result.message = L"无法创建日志目录：" + result.directory + L"\n" + SystemError(code);
        for (auto& log : result.logs) { log.state = CollectionState::Failed; log.error = code; log.message = result.message; }
    } else {
        for (auto& log : result.logs) {
            if (cancel && cancel->load()) { result.cancelled = true; log.state = CollectionState::Cancelled; continue; }
            log.state = CollectionState::Running; publish();
            try {
                Query query; query.source = log.channel; query.limit = 0;
                // Export all events, including an empty channel, without modifying the source.
                Export(query, log.path, Format::Evtx, false, cancel);
                log.state = CollectionState::Saved;
                std::error_code sizeError;
                const auto bytes = std::filesystem::file_size(log.path, sizeError);
                if (!sizeError) log.bytes = bytes;
            } catch (const Error& error) {
                log.error = error.code; log.message = error.text;
                if (error.code == ERROR_CANCELLED) { result.cancelled = true; log.state = CollectionState::Cancelled; }
                else log.state = CollectionState::Failed;
            } catch (const std::exception&) {
                log.state = CollectionState::Failed; log.error = ERROR_GEN_FAILURE;
                log.message = L"采集时发生意外错误。";
            }
            publish();
        }
    }
    result.finished = true; publish();
    return result;
}

std::wstring CollectionJson(const CollectionResult& result) {
    std::wstring json = L"{\n  \"directory\": " + JsonQuote(result.directory) +
        L",\n  \"success\": " + (result.success() ? L"true" : L"false") +
        L",\n  \"cancelled\": " + (result.cancelled ? L"true" : L"false") +
        L",\n  \"elapsedMs\": " + std::to_wstring(result.elapsedMs) +
        L",\n  \"error\": " + JsonQuote(result.message) + L",\n  \"logs\": [\n";
    const wchar_t* states[]{L"waiting", L"running", L"saved", L"failed", L"cancelled"};
    for (size_t i = 0; i < result.logs.size(); ++i) {
        const auto& log = result.logs[i];
        json += L"    {\"channel\": " + JsonQuote(log.channel) + L", \"path\": " + JsonQuote(log.path) +
            L", \"status\": " + JsonQuote(states[static_cast<size_t>(log.state)]) + L", \"bytes\": " + std::to_wstring(log.bytes) +
            L", \"errorCode\": " + std::to_wstring(log.error) + L", \"error\": " + JsonQuote(log.message) + L"}" +
            (i + 1 == result.logs.size() ? L"\n" : L",\n");
    }
    return json + L"  ]\n}\n";
}
}
