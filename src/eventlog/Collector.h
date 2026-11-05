#pragma once
#include "EventLogService.h"
#include <array>

namespace elm
{
    enum class CollectionState
    {
        Waiting,
        Running,
        Saved,
        Failed,
        Cancelled,
        NotFound,
        Skipped,
        Partial
    };
    struct CollectionOptions
    {
        unsigned days = 7;
        bool includeLargeDumps = false;
        bool powerReports = true;
        bool energyReport = false;
    };
    struct CollectedFile
    {
        std::wstring source, path, modifiedUtc, message;
        CollectionState state = CollectionState::Waiting;
        uint64_t bytes = 0;
        DWORD error = 0;
    };
    struct CollectedLog
    {
        std::wstring channel, caption, path;
        CollectionState state = CollectionState::Waiting;
        uint64_t bytes = 0;
        DWORD error = 0;
        std::wstring message;
        std::vector<CollectedFile> files;
    };
    struct CollectionResult
    {
        std::wstring rootDirectory, directory, manifestPath, computerName, startedUtc, finishedUtc, timeZone, message;
        std::array<CollectedLog, 5> logs;
        std::vector<CollectedLog> diagnostics;
        CollectionOptions options;
        int utcOffsetMinutes = 0;
        uint64_t elapsedMs = 0;
        DWORD manifestError = 0;
        bool finished = false, cancelled = false, manifestSaved = false;
        size_t saved() const;
        size_t completed() const;
        size_t count(CollectionState state) const;
        size_t total() const { return logs.size() + diagnostics.size(); }
        bool success() const;
    };
    std::wstring CollectionStateName(CollectionState state);
    std::wstring CollectedFilesJson(const std::vector<CollectedFile> &files);
    std::vector<const CollectedLog *> CollectionEntries(const CollectionResult &result);
    std::vector<CollectedLog *> CollectionEntries(CollectionResult &result);
    std::wstring DefaultLogDirectory();
    CollectionResult InitialCollection(const std::wstring &directory = L"");
    CollectionResult CollectWindowsLogs(const std::wstring &directory = L"",
                                        const std::function<void(const CollectionResult &)> &progress = {}, const std::atomic_bool *cancel = nullptr,
                                        const CollectionOptions &options = {});
    std::wstring CollectionJson(const CollectionResult &result);
}
