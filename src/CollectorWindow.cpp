#include "CollectorWindow.h"
#include "Collector.h"
#include "resource.h"
#include <fluentui/app_window.hpp>
#include <fluentui/app_bar.hpp>
#include <fluentui/content_dialog.hpp>
#include <fluentui/icon_symbols.hpp>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace elm {
namespace {
using namespace fluent;
constexpr wchar_t UiFont[] = L"Microsoft YaHei UI";

std::wstring FileSize(uint64_t bytes) {
    wchar_t text[48]{};
    if (bytes >= 1024 * 1024) swprintf_s(text, L"%.1f MB", bytes / (1024. * 1024.));
    else swprintf_s(text, L"%.0f KB", bytes / 1024.);
    return text;
}

class LogRow final : public Widget {
public:
    CollectedLog log;
    Role role() const override { return Role::Text; }
    std::wstring valueText() const override { return log.channel + L" " + stateText(); }
    std::wstring stateText() const {
        switch (log.state) {
        case CollectionState::Running: return L"正在采集…";
        case CollectionState::Saved: return L"已保存 · " + FileSize(log.bytes);
        case CollectionState::Failed: return L"采集失败";
        case CollectionState::Cancelled: return L"已停止";
        case CollectionState::NotFound: return L"未找到";
        case CollectionState::Skipped: return L"已跳过";
        case CollectionState::Partial: return L"部分完成";
        default: return L"等待采集";
        }
    }
    void draw(Painter& painter, const Palette& p) override {
        auto r = bounds(); const auto colors = DesignPalette::get(p);
        painter.fill(r, colors.card, 8); painter.stroke(r.inset(.5f), colors.border, 1, 8);
        Color ink = p.secondary;
        IconSymbol icon = IconSymbol::Document;
        if (log.state == CollectionState::Saved) { ink = p.accent; icon = IconSymbol::CheckMark; }
        if (log.state == CollectionState::Running) { ink = p.accent; icon = IconSymbol::Download; }
        if (log.state == CollectionState::Failed) { ink = Color::rgb(p.dark ? 0xFF99A4 : 0xC42B1C); icon = IconSymbol::ErrorBadge; }
        if (log.state == CollectionState::Partial) { ink = Color::rgb(p.dark ? 0xFCE100 : 0x946800); icon = IconSymbol::Warning; }
        auto tint = ink; tint.a = .09f;
        painter.fill({r.x + 12, r.y + 11, 32, 32}, tint, 7);
        painter.icon(static_cast<wchar_t>(icon), {r.x + 19, r.y + 18, 18, 18}, ink, 18);
        painter.text(log.caption, {r.x + 58, r.y + 7, r.width - 244, 24}, p.text, 14, 600, TextAlign::Leading, false, UiFont);
        painter.text(log.channel, {r.x + 58, r.y + 31, r.width - 244, 17}, p.secondary, 11, 400, TextAlign::Leading, false, L"Segoe UI");
        painter.text(stateText(), {r.right() - 190, r.y + 14, 174, 26}, ink, 12, 400, TextAlign::Trailing, false, UiFont);
    }
};

class CollectorView final : public Panel {
public:
    explicit CollectorView(AppWindow& window) : window_(window), result_(InitialCollection()) {
        title_ = &label(L"正在收集诊断信息", 25); title_->weight = 600;
        subtitle_ = &label(L"准备收集日志、转储和系统信息，请稍候…", 13, true);
        progress_ = &add<ProgressBar>(); progress_->indeterminate = true; progress_->strokeWidth = 4;
        scroll_ = &add<ScrollViewer>();
        auto& list = scroll_->add<StackPanel>(); list.spacing = 8;
        for (size_t i = 0; i < result_.total(); ++i) { auto& row = list.add<LogRow>(); row.height = 54; rows_.push_back(&row); }
        path_ = &label(result_.directory, 12); path_->wrapMode = TextWrapMode::Anywhere;
        summary_ = &label(L"近期证据默认保留 7 天范围；未找到转储不表示没有发生蓝屏。", 12, true);
        details_ = &button(L"查看详情", IconSymbol::Info, [this] { showDetails(); }); details_->subtle = true;
        folder_ = &button(L"打开采集文件夹", IconSymbol::FolderOpen, [this] { openFolder(); });
        close_ = &button(L"完成", IconSymbol::CheckMark, [this] { window_.close(); }); close_->accent = true;
        dialog_ = &add<ContentDialog>(); dialog_->dialogWidth = 550;
        dialog_->buttons = DialogButtons::Positive; dialog_->positiveText = L"关闭";
        dialog_->defaultButton = DialogButtons::Positive; dialog_->cancelButton = DialogButtons::Positive;
        apply(result_);
    }
    ~CollectorView() override { cancel_ = true; if (worker_.joinable()) worker_.join(); }
    void start() {
        try {
            worker_ = std::thread([this, directory = result_.directory] {
                try {
                    CollectWindowsLogs(directory, [this](const CollectionResult& value) {
                        std::lock_guard lock(mutex_); pending_ = value;
                    }, &cancel_);
                } catch (...) {
                    auto failed = InitialCollection(directory); failed.finished = true;
                    failed.message = L"无法完成采集，请关闭后重试。";
                    for (auto* item : CollectionEntries(failed)) { item->state = CollectionState::Failed; item->message = failed.message; }
                    std::lock_guard lock(mutex_); pending_ = std::move(failed);
                }
            });
        } catch (...) {
            auto failed = result_; failed.finished = true; failed.message = L"无法启动后台采集任务。";
            for (auto* item : CollectionEntries(failed)) item->state = CollectionState::Failed;
            apply(failed);
        }
    }
    void poll() {
        std::optional<CollectionResult> next;
        { std::lock_guard lock(mutex_); next.swap(pending_); }
        if (!next) return;
        apply(*next);
        if (result_.finished && worker_.joinable()) worker_.join();
        if (result_.finished && closing_) window_.close();
    }
    bool requestClose() {
        if (result_.finished) return true;
        closing_ = true; cancel_ = true;
        subtitle_->text = L"正在等待当前导出结束，随后关闭…"; subtitle_->invalidate(); return false;
    }
    int exitCode() const { return result_.success() ? 0 : result_.cancelled ? 5 : 4; }
    bool preTranslate(MSG& message) { return dialog_->preTranslate(message); }
    Size measure(Size available) override { return constrain(available, available); }
    void arrange(Rect r) override {
        Widget::arrange(r);
        auto place = [](Widget* widget, Rect at) { widget->measure({at.width, at.height}); widget->arrange(at); };
        place(title_, {r.x + 100, r.y + 20, r.width - 128, 38});
        place(subtitle_, {r.x + 100, r.y + 63, r.width - 128, 28});
        place(progress_, {r.x + 28, r.y + 107, r.width - 56, 5});
        place(scroll_, {r.x + 28, r.y + 132, r.width - 56, r.height - 344});
        place(path_, {r.x + 28, r.bottom() - 164, r.width - 56, 40});
        place(summary_, {r.x + 28, r.bottom() - 112, r.width - 56, 28});
        place(details_, {r.x + 20, r.bottom() - 62, 112, 36});
        place(folder_, {r.right() - 304, r.bottom() - 62, 168, 36});
        place(close_, {r.right() - 124, r.bottom() - 62, 96, 36});
    }
    void draw(Painter& painter, const Palette& p) override {
        auto r = bounds(); auto colors = DesignPalette::get(p);
        painter.fill(r, colors.page);
        auto color = p.accent;
        if (result_.finished && !result_.success()) color = Color::rgb(p.dark ? 0xFCE100 : 0x946800);
        auto tint = color; tint.a = .09f;
        painter.fill({r.x + 28, r.y + 25, 52, 52}, tint, 13);
        IconSymbol icon = !result_.finished ? IconSymbol::Download : result_.success() ? IconSymbol::CheckMark : IconSymbol::Warning;
        painter.icon(static_cast<wchar_t>(icon), {r.x + 41, r.y + 38, 26, 26}, color, 26);
        painter.text(L"保存位置", {r.x + 28, r.bottom() - 188, 100, 20}, p.secondary, 11, 400, TextAlign::Leading, false, UiFont);
        Panel::draw(painter, p);
    }
    void fixture(const std::wstring& scenario) {
        auto value = InitialCollection(); value.elapsedMs = 1800;
        value.directory += L"\\Collection_20261021_103000_001-1234";
        value.manifestPath = value.directory + L"\\manifest.json";
        for (auto* item : CollectionEntries(value)) {
            item->state = CollectionState::Saved; item->bytes = 1024 * 1024; item->path = value.directory + L"\\" + item->channel;
        }
        value.diagnostics[0].state = value.diagnostics[1].state = CollectionState::NotFound;
        value.diagnostics[0].message = L"近期未找到蓝屏转储，配置记录已保存。";
        value.diagnostics.back().state = CollectionState::Skipped;
        value.diagnostics.back().message = L"按需采集：命令行增加 --power。";
        value.finished = true; value.manifestSaved = true;
        if (scenario == L"loading") {
            value.finished = false; value.manifestSaved = false;
            value.diagnostics[3].state = CollectionState::Running;
            for (size_t i = 4; i < value.diagnostics.size(); ++i) value.diagnostics[i].state = CollectionState::Waiting;
        } else if (scenario == L"failure") {
            value.logs[2].state = CollectionState::Failed;
            value.logs[2].message = L"访问被拒绝。请检查安全日志的访问权限。";
        } else if (scenario == L"partial") {
            value.diagnostics[2].state = CollectionState::Partial;
            value.diagnostics[2].message = L"部分故障报告无权限读取，已保存可访问的文件和索引。";
        } else if (scenario == L"cancelled") {
            value.cancelled = true;
            for (size_t i = 3; i < value.diagnostics.size(); ++i) value.diagnostics[i].state = CollectionState::Cancelled;
        } else if (scenario == L"manifest-failure") {
            value.manifestSaved = false; value.manifestError = ERROR_DISK_FULL; value.message = L"无法保存采集清单：磁盘空间不足。";
        } else if (scenario != L"success" && scenario != L"diagnostics") throw std::runtime_error("Unknown UI fixture");
        apply(value);
        if (close_->enabled != value.finished || (value.success() != (title_->text == L"采集完成")))
            throw std::runtime_error("Collection completion UI is not bound to the result");
        if (rows_.size() != value.total()) throw std::runtime_error("Diagnostic rows missing from UI");
    }
    void validateDiagnosticScroll() {
        scroll_->reveal(rows_[5]);
        if (scroll_->offset <= 0 || scroll_->extent() <= scroll_->viewportBounds().height)
            throw std::runtime_error("Diagnostic rows are not reachable by scrolling");
        invalidate(true);
    }
private:
    TextBlock& label(std::wstring text, float size, bool secondary = false) {
        auto& value = add<TextBlock>(std::move(text), size); value.fontFamily = UiFont; value.secondary = secondary; return value;
    }
    Button& button(std::wstring text, IconSymbol icon, std::function<void()> action) {
        auto& value = add<Button>(std::move(text), std::move(action)); value.fontFamily = UiFont;
        value.fontSize = 13; value.glyph = static_cast<wchar_t>(icon); return value;
    }
    void apply(const CollectionResult& value) {
        result_ = value;
        const auto items = CollectionEntries(value);
        for (size_t i = 0; i < rows_.size(); ++i) {
            rows_[i]->log = *items[i]; rows_[i]->tooltip = items[i]->message.empty() ? items[i]->path : items[i]->message;
        }
        path_->text = value.directory; path_->tooltip = value.directory;
        close_->enabled = value.finished; details_->enabled = value.finished;
        folder_->enabled = value.finished && (value.saved() > 0 || value.count(CollectionState::Partial) > 0);
        progress_->indeterminate = !value.finished;
        progress_->setValue(100. * static_cast<double>(value.completed()) / value.total());
        if (!value.finished) {
            title_->text = L"正在收集诊断信息";
            auto current = std::find_if(items.begin(), items.end(), [](const auto* item) { return item->state == CollectionState::Running; });
            subtitle_->text = current == items.end() ? L"准备收集日志、转储和系统信息，请稍候…" :
                L"正在收集" + (*current)->caption + L" · 已处理 " + std::to_wstring(value.completed()) + L" / " + std::to_wstring(value.total());
            if (current != items.end() && active_ != (*current)->channel) {
                active_ = (*current)->channel; scroll_->reveal(rows_[static_cast<size_t>(current - items.begin())]);
            }
            if (closing_) subtitle_->text = L"正在等待当前导出结束，随后关闭…";
        } else {
            title_->text = value.success() ? L"采集完成" : value.cancelled ? L"采集已停止" :
                value.manifestError ? L"采集清单保存失败" : value.saved() ? L"部分信息采集失败" : L"采集未完成";
            subtitle_->text = L"已保存 " + std::to_wstring(value.saved()) + L" 项，未找到 " +
                std::to_wstring(value.count(CollectionState::NotFound)) + L" 项，跳过 " + std::to_wstring(value.count(CollectionState::Skipped)) +
                L" 项；详情已逐项记录。";
            wchar_t duration[48]{}; swprintf_s(duration, L"%.1f 秒", value.elapsedMs / 1000.);
            summary_->text = value.success() ? L"采集完成 · 用时 " + std::wstring(duration) + L" · 采集清单已保存" :
                L"请查看详情了解原因；已保存的资料可以继续用于分析。";
            window_.setTitle(title_->text + L" - 事件日志收集器");
        }
        notifyChanged(); invalidate(true);
    }
    void showDetails() {
        std::wstring text = result_.message;
        for (const auto* item : CollectionEntries(result_)) {
            if (!text.empty()) text += L"\n\n";
            text += item->caption + L"：" + CollectionStateName(item->state);
            if (!item->message.empty()) text += L"\n" + item->message;
            if (!item->path.empty()) text += L"\n" + item->path;
            size_t shown = 0;
            for (const auto& file : item->files) if (file.state == CollectionState::Failed || file.state == CollectionState::Partial) {
                if (++shown > 5) { text += L"\n更多原因见该项索引。"; break; }
                text += L"\n" + file.source + L"：" + file.message;
            }
        }
        if (!result_.manifestPath.empty()) text += L"\n\n采集清单：" + result_.manifestPath;
        dialog_->title = L"采集详情"; dialog_->message.clear();
        dialog_->contentFactory = [text] {
            auto scroll = std::make_unique<ScrollViewer>(); scroll->height = 320;
            auto& block = scroll->add<TextBlock>(text, 13.f); block.fontFamily = UiFont; block.wrapMode = TextWrapMode::Anywhere;
            return scroll;
        };
        dialog_->open();
    }
    void openFolder() {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_.hwnd(), L"open", result_.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            dialog_->title = L"无法打开文件夹"; dialog_->message = L"请手动打开：\n" + result_.directory;
            dialog_->contentFactory = {}; dialog_->open();
        }
    }
    AppWindow& window_;
    CollectionResult result_;
    std::thread worker_;
    std::atomic_bool cancel_{false};
    std::mutex mutex_;
    std::optional<CollectionResult> pending_;
    bool closing_ = false;
    std::vector<LogRow*> rows_;
    ScrollViewer* scroll_{};
    std::wstring active_;
    TextBlock *title_{}, *subtitle_{}, *path_{}, *summary_{};
    ProgressBar* progress_{};
    Button *details_{}, *folder_{}, *close_{};
    ContentDialog* dialog_{};
};
}

int RunGui(HINSTANCE instance, const GuiOptions& options) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 4;
    int result = 0;
    try {
        AppWindow window; AppWindowOptions settings;
        settings.title = L"事件日志收集器"; settings.width = 720 * options.uiScale; settings.height = 700 * options.uiScale;
        settings.minimumWidth = 720; settings.minimumHeight = 700; settings.fixedSize = true;
        settings.showMaximize = false; settings.host.visualStyle = VisualStyle::WinUI3;
        settings.host.theme = options.dark ? ThemeMode::Dark : ThemeMode::Light;
        settings.host.accent = {Color::rgb(0x00443F), Color::rgb(0x00645D), Color::rgb(0x007F78), Color::rgb(0x008F87),
            Color::rgb(0x38B8AE), Color::rgb(0x76D8CC), Color::rgb(0xA7EBE2)};
        settings.host.forceWarp = options.validate; settings.backgroundEffect = WindowEffect::Normal;
        settings.icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP)); settings.autoVisible = false;
        if (!window.create(nullptr, settings)) throw std::runtime_error("Could not create collector window");
        if (options.uiScale != 1) window.host().setUiScale(options.uiScale);
        auto bar = window.createAppBar(); bar->height = 40; bar->showStayTop = false; bar->showMaximize = false;
        bar->minimizeText = L"最小化"; bar->closeText = L"关闭"; bar->fontFamily = UiFont;
        window.setAppBar(std::move(bar));
        auto content = std::make_unique<CollectorView>(window); auto* view = content.get();
        window.setContent(std::move(content));
        window.setCloseHandler([view, &result] { if (!view->requestClose()) return false; result = view->exitCode(); return true; });
        window.setClosedHandler([&] { PostQuitMessage(result); });
        window.host().setErrorHandler([&](auto, std::wstring_view) { result = 4; });
        window.completeInitialization(false); window.show(!options.validate);
        if (options.validate) {
            auto pump = [&] {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message != WM_QUIT && !view->preTranslate(message) && !window.preTranslate(message)) {
                        TranslateMessage(&message); DispatchMessageW(&message);
                    }
                }
            };
            view->fixture(options.scenario);
            for (int i = 0; i < 25; ++i) { pump(); Sleep(10); }
            if (options.scenario == L"diagnostics") view->validateDiagnosticScroll();
            if (!options.snapshot.empty()) {
                window.host().painter().captureNextFrame(options.snapshot); window.host().invalidate(true);
                for (int i = 0; i < 25; ++i) { pump(); Sleep(10); }
                if (!window.host().painter().captureStatus().starts_with(L"Saved D2D surface PNG:"))
                    throw std::runtime_error("Collector frame capture failed");
                std::ofstream report(std::filesystem::path(options.snapshot + L".txt"));
                report << "PASS: collector " << Utf8(options.scenario) << " fixture, completion state and button binding\n";
            }
            window.setClosedHandler({}); window.destroy();
        } else {
            window.host().invalidate(true); UpdateWindow(window.hwnd()); view->start();
            SetTimer(window.hwnd(), 77, 50, nullptr);
            MSG message{}; BOOL next = 0;
            while (window.hwnd() && (next = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                if (message.message == WM_TIMER && message.hwnd == window.hwnd() && message.wParam == 77) { view->poll(); continue; }
                if (!view->preTranslate(message) && !window.preTranslate(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if (next == -1) result = 4;
        }
    } catch (const std::exception& error) {
        result = 4;
        if (options.validate && !options.snapshot.empty()) { std::ofstream report(std::filesystem::path(options.snapshot + L".txt")); report << "FAIL: " << error.what(); }
        else MessageBoxW(nullptr, L"无法初始化采集窗口，请使用命令行 collect 重试。", L"事件日志收集器", MB_OK | MB_ICONERROR);
    }
    CoUninitialize(); return result;
}
}
