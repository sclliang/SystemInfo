#include "Collection.h"

namespace system_info
{
Collection::~Collection() { Stop(); }

bool Collection::Start(ComputerReader computer, SsdReader ssd)
{
    if (busy_) return false;
    busy_ = true;
    computer_.reset();
    ssd_.reset();
    changed_ = false;
    try
    {
        computerWorker_ = std::jthread([this, read = std::move(computer)](std::stop_token token) {
            ftc::hardware_info::Snapshot result;
            try { result = read(token); }
            catch (...) { result.error = L"电脑信息采集发生异常，请刷新重试。"; }
            if (token.stop_requested()) return;
            std::lock_guard guard(mutex_);
            computer_ = std::move(result);
            changed_ = true;
        });
    }
    catch (...)
    {
        std::lock_guard guard(mutex_);
        computer_.emplace().error = L"无法启动电脑信息采集。";
        changed_ = true;
    }
    try
    {
        ssdWorker_ = std::jthread([this, read = std::move(ssd)](std::stop_token token) {
            storage::SsdResult result;
            try { result = read(token); }
            catch (...) { result = {L"N/A", L"SSD SMART 读取失败，请刷新重试。"}; }
            if (token.stop_requested()) return;
            std::lock_guard guard(mutex_);
            ssd_ = std::move(result);
            changed_ = true;
        });
    }
    catch (...)
    {
        std::lock_guard guard(mutex_);
        ssd_ = storage::SsdResult{L"N/A", L"无法启动 SSD 采集。"};
        changed_ = true;
    }
    return true;
}

std::optional<CollectionUpdate> Collection::Take()
{
    std::lock_guard guard(mutex_);
    if (!changed_) return {};
    changed_ = false;
    CollectionUpdate update;
    update.computerPending = !computer_;
    update.ssdPending = !ssd_;
    if (computer_) update.snapshot = *computer_;
    if (ssd_) storage::ApplySsdInfo(update.snapshot, *ssd_);
    else storage::ApplySsdInfo(update.snapshot, {L"正在读取 SSD SMART…", {}});
    busy_ = !update.Complete();
    return update;
}

void Collection::Stop()
{
    computerWorker_.request_stop();
    ssdWorker_.request_stop();
}
}
