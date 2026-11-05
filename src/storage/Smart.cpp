#include "Smart.h"

#include <algorithm>
#include <optional>

namespace system_info::storage
{
namespace
{
constexpr std::size_t kHealthLogSize = 512U;
constexpr std::size_t kDataReadOffset = 32U;
constexpr std::size_t kDataWrittenOffset = 48U;
constexpr std::size_t kPowerCountOffset = 112U;
constexpr std::size_t kPowerHoursOffset = 128U;

[[nodiscard]] std::uint64_t LittleEndian64(
    const std::uint8_t* const data,
    const std::size_t count) noexcept
{
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < (std::min)(count, sizeof(value)); ++index)
    {
        value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::optional<std::uint64_t> ExactLittleEndian64(
    const std::uint8_t* const data,
    const std::size_t count) noexcept
{
    if (count > sizeof(std::uint64_t))
    {
        for (std::size_t index = sizeof(std::uint64_t); index < count; ++index)
        {
            if (data[index] != 0U)
            {
                return std::nullopt;
            }
        }
    }
    return LittleEndian64(data, count);
}

[[nodiscard]] long double LittleEndianLongDouble(
    const std::uint8_t* const data,
    const std::size_t count) noexcept
{
    long double value = 0.0L;
    long double factor = 1.0L;
    for (std::size_t index = 0U;
         index < (std::min)(count, static_cast<std::size_t>(16U));
         ++index)
    {
        value += static_cast<long double>(data[index]) * factor;
        factor *= 256.0L;
    }
    return value;
}

[[nodiscard]] long double DataUnitsToGb(const std::uint8_t* const data) noexcept
{
    return LittleEndianLongDouble(data, 16U) * 512'000.0L /
        (1024.0L * 1024.0L * 1024.0L);
}
} // namespace

std::optional<long double> AtaSmartHostGb(const int normalizedGb) noexcept
{
    return normalizedGb < 0 ? std::nullopt :
        std::optional<long double>{static_cast<long double>(normalizedGb)};
}

std::wstring DecodeAtaTransferMode(const unsigned int word77, const unsigned int word88)
{
    // ATA word 77 bits 3:1 encode one negotiated speed; they are not flags.
    switch ((word77 >> 1U) & 0x07U)
    {
    case 1U: return L"SATA/150";
    case 2U: return L"SATA/300";
    case 3U: return L"SATA/600";
    default: break;
    }
    const unsigned int udma = (word88 >> 8U) & 0xFFU;
    for (int bit = 7; bit >= 0; --bit)
    {
        if ((udma & (1U << static_cast<unsigned int>(bit))) != 0U)
            return L"UDMA/" + std::to_wstring(bit);
    }
    return L"ATA";
}

bool DecodeNvmeHealthLog(
    const bool snapshotValid,
    const std::span<const std::uint8_t> bytes,
    Disk& disk) noexcept
{
    if (!snapshotValid || bytes.size() < kHealthLogSize)
    {
        return false;
    }

    disk.isNvme = true;
    disk.solidState = true;
    disk.dataUnitsReadGb = DataUnitsToGb(bytes.data() + kDataReadOffset);
    disk.dataUnitsWrittenGb = DataUnitsToGb(bytes.data() + kDataWrittenOffset);
    disk.powerCounts = ExactLittleEndian64(
        bytes.data() + kPowerCountOffset, 16U);
    disk.powerHours = ExactLittleEndian64(
        bytes.data() + kPowerHoursOffset, 16U);
    return true;
}
} // namespace system_info::storage
