#pragma once

#include "Disk.h"

#include <cstdint>
#include <span>

namespace system_info::storage
{
// AtaSmartCore HostReads/HostWrites are already normalized GB, not raw units.
[[nodiscard]] std::optional<long double> AtaSmartHostGb(int normalizedGb) noexcept;
[[nodiscard]] std::wstring DecodeAtaTransferMode(unsigned int word77, unsigned int word88);

// Decodes one NVMe SMART / Health Information log page. The caller must pass
// an explicit success flag for this exact I/O operation; a zero-filled buffer
// is never treated as proof that the read succeeded.
[[nodiscard]] bool DecodeNvmeHealthLog(
    bool snapshotValid,
    std::span<const std::uint8_t> bytes,
    Disk& disk) noexcept;
} // namespace system_info::storage
