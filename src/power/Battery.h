#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace system_info::power
{
struct Pack
{
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> fields;
};

struct Reading
{
    std::optional<bool> present;
    std::optional<unsigned> percent;
    std::wstring source;
    std::wstring state;
    std::wstring remaining;
    std::vector<Pack> packs;
};

Reading Read();
std::wstring FormatReport(const Reading& reading);
}
