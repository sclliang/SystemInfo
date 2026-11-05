#pragma once
#include "eventlog/Collector.h"
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace system_info
{
enum class LogState { Running, Cancelling, Complete, Partial, Failed, Cancelled };

struct LogUpdate
{
    LogState state{LogState::Running};
    size_t completed{}, total{}, saved{}, partial{}, failed{};
    std::wstring item, detail;
    std::filesystem::path directory, report;
    bool Finished() const { return state != LogState::Running && state != LogState::Cancelling; }
};

class LogCollection final
{
public:
    ~LogCollection() { Cancel(); }
    bool Start(const std::wstring& outputRoot = {});
    void Cancel() { cancel_.store(true); }
    std::optional<LogUpdate> Take();
private:
    void Publish(LogUpdate update);
    std::atomic_bool cancel_{};
    std::mutex mutex_;
    std::optional<LogUpdate> pending_;
    bool active_{}; // Owned by the UI thread.
    std::jthread worker_; // Joined before destroying the mailbox or the cancellation flag.
};
}
