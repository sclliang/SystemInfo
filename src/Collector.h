#pragma once
#include "EventLogService.h"
#include <array>

namespace elm {
enum class CollectionState { Waiting, Running, Saved, Failed, Cancelled };
struct CollectedLog {
    std::wstring channel, caption, path;
    CollectionState state = CollectionState::Waiting;
    uint64_t bytes = 0;
    DWORD error = 0;
    std::wstring message;
};
struct CollectionResult {
    std::wstring directory, message;
    std::array<CollectedLog, 5> logs;
    uint64_t elapsedMs = 0;
    bool finished = false, cancelled = false;
    size_t saved() const;
    bool success() const { return finished && saved() == logs.size(); }
};
std::wstring DefaultLogDirectory();
CollectionResult InitialCollection(const std::wstring& directory = L"");
CollectionResult CollectWindowsLogs(const std::wstring& directory = L"",
    const std::function<void(const CollectionResult&)>& progress = {}, const std::atomic_bool* cancel = nullptr);
std::wstring CollectionJson(const CollectionResult& result);
}
