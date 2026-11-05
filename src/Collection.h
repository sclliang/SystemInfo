#pragma once
#include "storage/SsdInfo.h"
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace system_info
{
struct CollectionUpdate
{
    ftc::hardware_info::Snapshot snapshot;
    bool computerPending{}, ssdPending{};
    bool Complete() const { return !computerPending && !ssdPending; }
};

// Start, Take and Stop belong to the UI thread. Workers publish data only.
class Collection final
{
public:
    using ComputerReader = std::function<ftc::hardware_info::Snapshot(std::stop_token)>;
    using SsdReader = std::function<storage::SsdResult(std::stop_token)>;
    ~Collection();
    bool Start(ComputerReader computer, SsdReader ssd);
    std::optional<CollectionUpdate> Take();
    void Stop();
private:
    std::mutex mutex_;
    std::optional<ftc::hardware_info::Snapshot> computer_;
    std::optional<storage::SsdResult> ssd_;
    bool changed_{}, busy_{};
    std::jthread computerWorker_, ssdWorker_;
};
}
