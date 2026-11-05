#pragma once

#include <windows.h>

#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace system_info::storage
{
struct WmiQueryResult final
{
    std::vector<std::vector<std::wstring>> rows;
    bool completed{};
    bool cancelled{};
    HRESULT error{S_OK};
};

// Each storage worker owns its COM apartment and WMI connections.
class WmiSession final
{
public:
    WmiSession() noexcept;
    ~WmiSession();

    WmiSession(const WmiSession&) = delete;
    WmiSession& operator=(const WmiSession&) = delete;
    WmiSession(WmiSession&&) = delete;
    WmiSession& operator=(WmiSession&&) = delete;

    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] HRESULT InitializationError() const noexcept;

    [[nodiscard]] WmiQueryResult Query(
        std::wstring_view wmiNamespace,
        std::wstring_view wql,
        const std::vector<std::wstring>& properties,
        std::stop_token stopToken) const;

private:
    bool ready_{};
    bool uninitialize_{};
    HRESULT initializationError_{E_FAIL};
};

[[nodiscard]] std::wstring FormatHresult(HRESULT value);
} // namespace system_info::storage
