#include "LogCollection.h"

namespace system_info
{
namespace
{
// Enable the privilege used to export Security on this worker only. Closing
// or finishing a collection restores the thread's original security context.
class LogThreadPrivilege final
{
public:
    LogThreadPrivilege()
    {
        impersonating_ = ImpersonateSelf(SecurityImpersonation) != FALSE;
        if (!impersonating_) return;
        HANDLE token{};
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &token)) return;
        TOKEN_PRIVILEGES privilege{};
        privilege.PrivilegeCount = 1;
        if (LookupPrivilegeValueW(nullptr, SE_SECURITY_NAME, &privilege.Privileges[0].Luid))
        {
            privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr);
        }
        CloseHandle(token);
    }
    ~LogThreadPrivilege() { if (impersonating_) RevertToSelf(); }
    LogThreadPrivilege(const LogThreadPrivilege&) = delete;
    LogThreadPrivilege& operator=(const LogThreadPrivilege&) = delete;
private:
    bool impersonating_{};
};

LogUpdate Summarize(const elm::CollectionResult& result)
{
    LogUpdate update;
    update.total = result.total();
    for (const auto* entry : elm::CollectionEntries(result))
    {
        switch (entry->state)
        {
        case elm::CollectionState::Running: update.item = entry->caption; continue;
        case elm::CollectionState::Waiting: continue;
        case elm::CollectionState::Saved: ++update.saved; break;
        case elm::CollectionState::Failed: ++update.failed; break;
        case elm::CollectionState::Partial: ++update.partial; break;
        default: break;
        }
        ++update.completed;
    }
    if (!result.manifestPath.empty()) update.directory = result.directory;
    if (!result.finished) return update;
    if (result.manifestSaved) update.report = result.manifestPath;
    update.detail = result.message;
    if (!result.manifestSaved) update.state = LogState::Failed;
    else if (result.cancelled) update.state = LogState::Cancelled;
    else if (result.success()) update.state = LogState::Complete;
    else if (update.failed == update.total) update.state = LogState::Failed;
    else update.state = LogState::Partial;
    return update;
}
}

bool LogCollection::Start(const std::wstring& outputRoot)
{
    if (active_) return false;
    cancel_.store(false);
    worker_ = std::jthread([this, outputRoot] {
        LogUpdate final;
        try
        {
            LogThreadPrivilege privilege;
            const auto result = elm::CollectWindowsLogs(outputRoot, [this](const elm::CollectionResult& progress) {
                if (!progress.finished) Publish(Summarize(progress));
            }, &cancel_);
            final = Summarize(result);
        }
        catch (const elm::Error& error) { final.state = LogState::Failed; final.detail = error.text; }
        catch (...) { final.state = LogState::Failed; final.detail = L"日志收集发生异常。"; }
        Publish(std::move(final));
    });
    active_ = true;
    return true;
}

void LogCollection::Publish(LogUpdate update)
{
    std::lock_guard guard(mutex_);
    pending_ = std::move(update);
}

std::optional<LogUpdate> LogCollection::Take()
{
    std::lock_guard guard(mutex_);
    if (!pending_) return {};
    auto update = std::move(pending_);
    pending_.reset();
    if (update->Finished()) active_ = false;
    else if (cancel_.load()) update->state = LogState::Cancelling;
    return update;
}
}
