#include <Windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <fluentui/app_window.hpp>
#include "Collection.h"
#include "InformationView.h"
#include "DiagnosticReport.h"
#include "WindowPlacement.h"
#include "firmware/AmiFirmware.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
constexpr UINT_PTR kPollTimer = 1;
struct Arguments
{
    std::filesystem::path report, snapshot, amiReport;
    bool smoke{}, smokeLogs{};
    float width{}, height{}, scale{1};
};

Arguments ReadArguments()
{
    Arguments options;
    int count = 0;
    auto raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!raw) throw std::runtime_error("CommandLineToArgvW failed");
    const auto release = [](wchar_t** value) { LocalFree(value); };
    const std::unique_ptr<wchar_t*, decltype(release)> arguments(raw, release);
    for (int i = 1; i < count; ++i)
    {
        const std::wstring_view argument(raw[i]);
        if ((argument == L"--report" || argument == L"--snapshot" || argument == L"--ami-report") && i + 1 < count)
        {
            auto& path = argument == L"--report" ? options.report : (argument == L"--ami-report" ? options.amiReport : options.snapshot);
            path = std::filesystem::absolute(raw[++i]);
        }
        else if (argument == L"--smoke") options.smoke = true;
        else if (argument == L"--smoke-logs") { options.smoke = true; options.smokeLogs = true; }
        else if (argument == L"--size" && i + 2 < count)
        {
            options.width = std::stof(raw[++i]);
            options.height = std::stof(raw[++i]);
            if (!(options.width >= 360 && options.width <= 2000 && options.height >= 240 && options.height <= 1600))
                throw std::runtime_error("Invalid window size");
        }
        else if (argument == L"--scale" && i + 1 < count)
        {
            options.scale = std::stof(raw[++i]);
            if (!(options.scale >= 1 && options.scale <= 2)) throw std::runtime_error("Invalid scale");
        }
        else throw std::runtime_error("Invalid command line");
    }
    if ((!options.report.empty() && !options.amiReport.empty()) ||
        ((!options.report.empty() || !options.amiReport.empty()) && (options.smoke || !options.snapshot.empty())))
        throw std::runtime_error("Report mode cannot be combined with UI capture");
    return options;
}

int RunWindow(const Arguments& arguments, int show)
{
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor))
        throw std::runtime_error("Monitor information unavailable");
    const RECT& work = monitor.rcWork;
    const RECT bounds = system_info::InitialWindowBounds(work);
    const int initialWidth = bounds.right - bounds.left;
    const int initialHeight = bounds.bottom - bounds.top;
    const float initialScale = static_cast<float>(GetDpiForSystem()) / 96.f * arguments.scale;
    fluent::AppWindow window;
    system_info::Collection collection;
    system_info::LogCollection logs;
    fluent::AppWindowOptions options;
    options.title = L"SystemInfo · 电脑信息";
    options.systemTitleBar = true;
    options.width = arguments.width > 0 ? arguments.width : static_cast<float>(initialWidth) / initialScale;
    options.height = arguments.height > 0 ? arguments.height : static_cast<float>(initialHeight) / initialScale;
    options.minimumWidth = (std::min)(440.f, options.width);
    options.minimumHeight = (std::min)(300.f, options.height);
    options.autoCenter = false;
    options.initialPosition = POINT{bounds.left, bounds.top};
    options.autoVisible = false;
    options.backgroundEffect = fluent::WindowEffect::Normal;
    options.host.theme = fluent::ThemeMode::Light;
    options.host.uiScale = arguments.scale;
    options.host.visualStyle = fluent::VisualStyle::WinUI3;
    options.icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    if (!window.create(nullptr, options)) throw std::runtime_error("Window creation failed");
    if (arguments.width == 0)
        SetWindowPos(window.hwnd(), nullptr, bounds.left, bounds.top,
            initialWidth, initialHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    else
        window.centerOnDesktop();
    int exitCode = 0;
    window.host().setErrorHandler([&](std::exception_ptr, std::wstring_view context) {
        OutputDebugStringW((L"SystemInfo UI: " + std::wstring(context) + L"\n").c_str());
        exitCode = 1;
        window.close();
    });
    auto view = std::make_unique<system_info::InformationView>();
    auto* information = view.get();
    auto refresh = [&] {
        try
        {
            if (collection.Start(system_info::firmware::CollectComputerInfo, system_info::storage::CollectSsdInfo))
                information->SetBusy();
        }
        catch (...) { information->SetStatus(L"无法启动采集线程，请重试"); }
    };
    information->onRefresh = refresh;
    information->onCollectLogs = [&] {
        try { if (logs.Start()) information->SetLogUpdate({}); }
        catch (...)
        {
            system_info::LogUpdate failed;
            failed.state = system_info::LogState::Failed;
            failed.detail = L"无法启动日志收集，请重试。";
            information->SetLogUpdate(failed);
        }
    };
    information->onCancelLogs = [&] { logs.Cancel(); };
    window.setContent(std::move(view));
    window.setCloseHandler([&] { logs.Cancel(); collection.Stop(); return true; });
    window.setClosedHandler([] { PostQuitMessage(0); });
    if (!SetTimer(window.hwnd(), kPollTimer, 100, nullptr)) throw std::runtime_error("SetTimer failed");
    if (!arguments.smoke) ShowWindow(window.hwnd(), show);
    refresh();
    if (arguments.smokeLogs) information->ActivateLogButton();

    bool ready = false;
    bool logsReady = !arguments.smokeLogs;
    bool logProgressSeen = false;
    bool captureRequested = false;
    const ULONGLONG started = GetTickCount64();
    ULONGLONG computerReadyMs{}, ssdReadyMs{};
    bool displayedWhileSsdPending{};
    MSG message{};
    int result = 0;
    while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0)
    {
        if (message.message == WM_TIMER && message.hwnd == window.hwnd() && message.wParam == kPollTimer)
        {
            if (auto logUpdate = logs.Take())
            {
                information->SetLogUpdate(*logUpdate);
                if (arguments.smokeLogs)
                {
                    logProgressSeen |= !logUpdate->Finished();
                    logsReady = logUpdate->Finished();
                    if (logsReady && logUpdate->report.empty()) exitCode = 1;
                }
            }
            if (auto update = collection.Take())
            {
                information->SetSnapshot(*update);
                ready = update->Complete();
                if (arguments.smoke)
                {
                    const auto elapsed = GetTickCount64() - started;
                    if (!update->computerPending && !computerReadyMs) computerReadyMs = elapsed;
                    if (!update->ssdPending && !ssdReadyMs) ssdReadyMs = elapsed;
                    if (!update->computerPending && update->ssdPending) displayedWhileSsdPending = true;
                }
                if (!update->snapshot.error.empty()) exitCode = 1;
            }
            if (ready && logsReady && !arguments.snapshot.empty() && !captureRequested)
            {
                window.host().painter().captureNextFrame(arguments.snapshot.wstring());
                if (arguments.smoke)
                    ShowWindow(window.hwnd(), arguments.width == 0 ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
                window.host().invalidate(true);
                captureRequested = true;
            }
            const auto status = window.host().painter().captureStatus();
            const bool captured = status.starts_with(L"Saved D2D surface PNG:");
            if (arguments.smoke && ready && logsReady && (arguments.snapshot.empty() || captured))
            {
                if (!arguments.snapshot.empty())
                {
                    RECT actual{};
                    GetWindowRect(window.hwnd(), &actual);
                    RECT frame{};
                    const bool hasFrame = SUCCEEDED(DwmGetWindowAttribute(window.hwnd(), DWMWA_EXTENDED_FRAME_BOUNDS,
                        &frame, sizeof(frame)));
                    auto layoutPath = arguments.snapshot;
                    layoutPath += L".layout.txt";
                    std::ofstream layout(layoutPath);
                    layout << "work=" << work.left << ',' << work.top << ',' << work.right - work.left << ',' << work.bottom - work.top << '\n';
                    layout << "window=" << actual.left << ',' << actual.top << ',' << actual.right - actual.left << ',' << actual.bottom - actual.top << '\n';
                    if (hasFrame) layout << "visible_frame=" << frame.left << ',' << frame.top << ','
                        << frame.right - frame.left << ',' << frame.bottom - frame.top << '\n';
                    layout << "maximized=" << (IsZoomed(window.hwnd()) != FALSE) << '\n';
                    layout << "dpi=" << GetDpiForWindow(window.hwnd()) << '\n';
                    layout << "computer_ready_ms=" << computerReadyMs << "\nssd_ready_ms=" << ssdReadyMs
                        << "\ncomputer_shown_while_ssd_pending=" << displayedWhileSsdPending << '\n';
                    if (arguments.smokeLogs) layout << "log_progress_seen=" << logProgressSeen << '\n';
                }
                window.close();
            }
            else if (arguments.smoke && GetTickCount64() - started > (arguments.smokeLogs ? 480000ULL : 120000ULL))
            {
                exitCode = 3;
                window.close();
            }
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_F5)
        {
            refresh();
            continue;
        }
        if (!window.preTranslate(message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    collection.Stop();
    return result == -1 ? 1 : exitCode;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show)
{
    Arguments arguments;
    try { arguments = ReadArguments(); }
    catch (...) { return 2; }
    if (!arguments.amiReport.empty()) return system_info::WriteDiagnosticReport(arguments.amiReport, true);
    if (!arguments.report.empty()) return system_info::WriteDiagnosticReport(arguments.report, false);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    int result = 1;
    try { result = RunWindow(arguments, show); }
    catch (...)
    {
        if (!arguments.smoke)
            MessageBoxW(nullptr, L"程序启动失败，请检查 FluentUI 图形环境后重试。", L"SystemInfo", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
