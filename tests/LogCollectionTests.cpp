#include "../src/LogCollection.h"
#include <chrono>
#include <fstream>
#include <iostream>

namespace
{
using namespace system_info;
namespace fs = std::filesystem;

void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

LogUpdate Finish(LogCollection& job)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do
    {
        if (auto state = job.Take(); state && state->Finished()) return std::move(*state);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < end);
    throw std::runtime_error("Log worker did not publish its terminal state");
}

void CheckRetry(const fs::path& root)
{
    LogCollection job;
    const auto file = root / L"not-a-directory.txt";
    std::ofstream(file) << "fixture";
    Check(job.Start(file.wstring()), "Failed to start test");
    const auto failed = Finish(job);
    Check(failed.state == LogState::Failed && failed.directory.empty() && failed.report.empty() &&
        !failed.detail.empty(), "Output failure showed a nonexistent report");
    fs::path previous;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        Check(job.Start(root.wstring()), "Cannot retry the log collection");
        Check(!job.Start(root.wstring()), "Overlapping collection was accepted");
        job.Cancel();
        const auto result = Finish(job);
        Check(result.state == LogState::Cancelled && result.total == 20 &&
            result.directory.parent_path() == root && result.report == result.directory / L"manifest.html" &&
            fs::is_regular_file(result.report), "Cancellation lost the manifest or the original collection scope");
        Check(previous != result.directory && (previous.empty() || fs::exists(previous / L"manifest.html")),
            "A later collection replaced the earlier output");
        std::ifstream stream(result.report, std::ios::binary);
        const std::string html{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        Check(html.find("\"cancelled\":true") != html.npos && html.find("@@DATA@@") == html.npos &&
            html.find("PowerReports") != html.npos, "The offline report did not embed the collection result");
        previous = result.directory;
    }
}
}

int main()
{
    try
    {
        const auto base = fs::canonical(fs::temp_directory_path());
        const auto root = base / (L"SystemInfo-LogTest-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
        Check(fs::create_directory(root), "Test requires a fresh output root");
        CheckRetry(root);
        Check(fs::canonical(root).parent_path() == base, "Unexpected cleanup location");
        fs::remove_all(root);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
