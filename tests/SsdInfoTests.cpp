#include "../src/Collection.h"
#include "../src/storage/Smart.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace system_info;
using namespace system_info::storage;
using namespace std::chrono_literals;

void Check(bool valid, const char* message)
{
    if (!valid) throw std::runtime_error(message);
}

void CheckStorage()
{
    Check(IsExternalStorage(L"SCSI", LR"(SCSI\DISK&VEN_NVME)", BusTypeUsb), "USB/NVMe bridge leaked");
    Check(IsExternalStorage(L"SCSI", LR"(USB\VID_174C&PID_55AA)", BusTypeUnknown), "USB parent leaked");
    Check(IsExternalStorage(L"SCSI", LR"(uaspstor\disk)", BusTypeScsi), "UASP disk leaked");
    Check(IsExternalStorage(L"USB", L"", BusTypeUnknown), "WMI USB disk leaked");
    Check(!IsExternalStorage(L"SCSI", LR"(SCSI\DISK&VEN_NVME)", BusTypeNvme), "Internal NVMe was rejected");
    Disk internal, bridge;
    internal.model = bridge.model = L"Same SSD model";
    internal.physicalDriveId = 0;
    bridge.physicalDriveId = 1;
    internal.pnpId = L"internal";
    bridge.pnpId = L"bridge";
    std::vector<Disk> matched{internal, bridge};
    RemoveExternalDisk(matched, -1, L"BRIDGE");
    Check(matched.size() == 1 && matched.front().physicalDriveId == 0, "Core USB evidence must also exclude the native row");
    matched.push_back(bridge);
    RemoveExternalDisk(matched, 1, {});
    Check(matched.size() == 1 && matched.front().physicalDriveId == 0, "USB exclusion confused two disks of the same model");
    std::array<std::uint8_t, 512> log{};
    Disk disk;
    disk.model = L"Test NVMe";
    disk.transferMode = L"PCIe 4.0 x4";
    disk.capacityBytes = 1'000'000'000'000ULL;
    Check(!DecodeNvmeHealthLog(false, log, disk) && !disk.powerCounts, "Failed SMART read became zero");
    log[32] = 1;
    log[48] = 2;
    log[128] = 0x23;
    log[129] = 1;
    Check(DecodeNvmeHealthLog(true, log, disk) && disk.powerHours == 291 && disk.powerCounts == 0,
        "Power cycles/hours were swapped, truncated or lost");
    Check(disk.dataUnitsReadGb == 512000.0L / 1073741824.0L &&
        disk.dataUnitsWrittenGb == 1024000.0L / 1073741824.0L, "NVMe transfer units or direction are wrong");
    disk.dataUnitsReadGb = 2048;
    disk.dataUnitsWrittenGb.reset();
    Disk hdd;
    hdd.model = L"Mechanical disk";
    const auto text = FormatDisks({hdd, disk});
    Check(text.find(hdd.model) == text.npos && text.find(L"NVMe · PCIe 4.0 x4") != text.npos &&
        text.find(L"1.00 TB") != text.npos && text.find(L"0 次") != text.npos &&
        text.find(L"2.00 TiB") != text.npos && text.find(L"写入 N/A") != text.npos, "SSD display lost a field or a unit");
}

CollectionUpdate Next(Collection& collection)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do
    {
        if (auto update = collection.Take()) return std::move(*update);
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("A finished worker was blocked by the other worker");
}

void CheckProgress(bool slowSsd)
{
    // Release the slow worker only after the first partial result has been consumed.
    std::mutex mutex;
    std::condition_variable_any changed;
    bool released = false;
    const auto wait = [&](std::stop_token stop) {
        std::unique_lock lock(mutex);
        changed.wait(lock, stop, [&] { return released; });
    };
    const auto basic = [&](std::stop_token stop) {
        if (!slowSsd) wait(stop);
        ftc::hardware_info::Snapshot value;
        value.rows = {{L"CPU型号", L"Test CPU"}, {L"SSD信息", L"N/A"}};
        return value;
    };
    const auto ssd = [&](std::stop_token stop) {
        if (slowSsd) wait(stop);
        return SsdResult{L"Test SSD", {}};
    };
    Collection collection;
    Check(collection.Start(basic, ssd), "Collection did not start");
    const auto partial = Next(collection);
    Check(partial.ssdPending == slowSsd && partial.computerPending != slowSsd && !partial.Complete(),
        "The first worker must publish independently");
    if (slowSsd) Check(partial.snapshot.rows.front().value == L"Test CPU" &&
        partial.snapshot.rows.back().value.find(L"正在读取") != std::wstring::npos, "Missing basic cards while SMART is pending");
    else Check(partial.snapshot.rows.size() == 1 && partial.snapshot.rows.front().value == L"Test SSD", "SSD result lost");
    Check(!collection.Start(basic, ssd), "A refresh overlapped with a running scan");
    { std::lock_guard lock(mutex); released = true; }
    changed.notify_all();
    const auto final = Next(collection);
    Check(final.Complete() && final.snapshot.rows.size() == 2 && final.snapshot.rows.back().value == L"Test SSD",
        "Second worker overwrote or duplicated an existing row");
    Check(collection.Start(basic, [](std::stop_token) -> SsdResult { throw std::runtime_error("device error"); }),
        "Refresh after completion failed");
    auto failed = Next(collection);
    if (!failed.Complete()) failed = Next(collection);
    Check(failed.snapshot.rows.front().value == L"Test CPU" && failed.snapshot.rows.back().value == L"N/A" &&
        !failed.snapshot.diagnostic.empty(), "A SMART failure lost valid data or reused old disk results");
}
}

int main()
{
    try { CheckStorage(); CheckProgress(true); CheckProgress(false); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
