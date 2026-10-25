#pragma once
#include "Collector.h"
#include <filesystem>

namespace elm
{
    // Every report carries its data and viewer, so it works offline without sidecar assets.
    std::wstring ReportHtml(const std::wstring &title, const std::wstring &json,
                            const std::filesystem::path &root, const std::filesystem::path &path,
                            const std::wstring &kind, const std::wstring &status = L"",
                            const std::wstring &message = L"");
}
