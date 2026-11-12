#include "../src/Collection.h"
#include "../src/storage/Disk.h"
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
    Check(!IsExternalStorage(L"SCSI", LR"(SCSI\DISK&VEN_ATA)", BusTypeSata), "Internal SATA was rejected");
    Check(IsExternalStorage(L"SCSI", LR"(USB4\DISK)", BusTypeNvme), "USB4 tunnel leaked");
    Check(IsExternalStorage(L"SCSI", LR"(SBP2\DISK)", BusType1394), "FireWire bridge leaked");
    Disk disk;
    disk.model = L"Test NVMe";
    disk.solidState = true;
    disk.capacityBytes = 1'000'000'000'000ULL;
    Disk hdd;
    hdd.model = L"Mechanical disk";
    const auto text = FormatDisks({hdd, disk});
    Check(text == L"Test NVMe · 1 TB", "SSD readout must contain only its name and nominal capacity and exclude HDDs");
}

void CheckNominalCapacities()
{
    Disk disk;
    disk.solidState = true;
    disk.model = L"Test SSD";
    for (const auto& [bytes, expected] : {
        std::pair{120'034'123'776ULL, L"128 GB"}, std::pair{128'035'676'160ULL, L"128 GB"},
        std::pair{240'057'409'536ULL, L"256 GB"}, std::pair{256'060'514'304ULL, L"256 GB"},
        std::pair{500'107'862'016ULL, L"512 GB"}, std::pair{512'110'190'592ULL, L"512 GB"},
        std::pair{1'000'204'886'016ULL, L"1 TB"}, std::pair{1'024'209'543'168ULL, L"1 TB"},
        std::pair{2'000'398'934'016ULL, L"2 TB"}, std::pair{2'048'408'248'320ULL, L"2 TB"},
        std::pair{3'840'000'000'000ULL, L"4 TB"}, std::pair{4'000'787'030'016ULL, L"4 TB"},
        std::pair{8'001'563'222'016ULL, L"8 TB"}, std::pair{16'000'000'000'000ULL, L"16 TB"}})
    {
        disk.capacityBytes = bytes;
        Check(FormatDisks({disk}) == std::wstring(L"Test SSD · ") + expected, "SSD name or nominal capacity changed");
    }
    disk.capacityBytes.reset();
    Check(FormatDisks({disk}) == L"Test SSD · N/A", "Unknown capacity became an invented capacity");
    disk.capacityBytes = 0;
    Check(FormatDisks({disk}) == L"Test SSD · N/A", "Zero capacity became an invented capacity");
    disk.capacityBytes = 500'107'862'016ULL;
    Disk second = disk;
    second.model = L"Second SSD";
    second.capacityBytes = 1'024'209'543'168ULL;
    Disk hdd;
    hdd.capacityBytes = 2'000'398'934'016ULL;
    Check(FormatDisks({disk, hdd, second}) == L"Test SSD · 512 GB\r\nSecond SSD · 1 TB", "Each SSD must have its own line with its name and nominal capacity");
    Check(FormatDisks({second, second}) == L"Second SSD · 1 TB\r\nSecond SSD · 1 TB", "Two identical SSDs must remain on separate lines");
    second.capacityBytes.reset();
    Check(FormatDisks({disk, second}) == L"Test SSD · 512 GB\r\nSecond SSD · N/A", "One unknown capacity hid another disk");
    second.model.clear();
    Check(FormatDisks({second}) == L"未提供名称的 SSD · N/A", "Missing name or capacity was invented");
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
        partial.snapshot.rows.back().value.find(L"正在读取") != std::wstring::npos, "Missing basic information while SSD is pending");
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
        !failed.snapshot.diagnostic.empty(), "An SSD query failure lost valid data or reused old disk results");
}
}

int main()
{
    try { CheckStorage(); CheckNominalCapacities(); CheckProgress(true); CheckProgress(false); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
