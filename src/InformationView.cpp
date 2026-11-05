#include "InformationView.h"
#include "Report.h"
#include <shellapi.h>
#include <fluentui/frame.hpp>
#include <fluentui/icon.hpp>
#include <algorithm>

namespace system_info
{
namespace
{
using namespace fluent;
constexpr std::array<const wchar_t*, 16> kLabels = {
    L"主板型号", L"OS信息", L"CPU型号", L"GPU型号", L"显示器信息", L"内存信息", L"SSD信息",
    L"BIOS版本", L"EC版本", L"主板SN", L"系统SN", L"UUID",
    L"有线网卡", L"无线网卡", L"蓝牙地址", L"TPM型号"};

class DeviceMark final : public Widget
{
public:
    DeviceMark() { width = height = 52; }
    void draw(Painter& painter, const Palette& palette) override
    {
        painter.fill(bounds_, palette.highContrast ? palette.surface : Color::rgb(0xE8F0FE), 13);
        painter.icon(static_cast<wchar_t>(IconSymbol::ThisPC), bounds_, palette.accent, 28);
    }
};

class CardGrid final : public TableLayout
{
public:
    CardGrid() { rowGap = columnGap = 12; }
    Size measure(Size available) override
    {
        const std::size_t desiredColumns = available.width >= 680.f ? 3 : (available.width >= 440.f ? 2 : 1);
        if (desiredColumns != columnCount_)
        {
            columnCount_ = desiredColumns;
            columns.assign(columnCount_, Track::star());
            const auto cards = children();
            rows.assign((cards.size() + columnCount_ - 1) / columnCount_, Track::automatic());
            for (std::size_t index = 0; index < cards.size(); ++index)
                setCell(*cards[index], {index / columnCount_, index % columnCount_});
        }
        return TableLayout::measure(available);
    }
private:
    std::size_t columnCount_{};
};

class ActionToolbar final : public TableLayout
{
public:
    Size measure(Size available) override
    {
        const bool compact = available.width < 620;
        if (rows.empty() || compact != compact_)
        {
            compact_ = compact;
            rows.assign(compact ? 2 : 1, Track::automatic());
            columns = {Track::star()};
            if (!compact) columns.push_back(Track::pixel(338));
            rowGap = 6;
            columnGap = 10;
            const auto widgets = children();
            setCell(*widgets.at(0), {0, 0});
            setCell(*widgets.at(1), compact ? TableCell{1, 0} : TableCell{0, 1});
        }
        return TableLayout::measure(available);
    }
private:
    bool compact_{};
};

std::wstring TimeText()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t text[24]{};
    swprintf_s(text, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    return text;
}
}

InformationView::InformationView()
{
    using namespace fluent;
    padding = {24, 12, 20, 10};
    spacing = 10;

    auto& heading = add<TableLayout>();
    setDock(heading, Dock::Top);
    heading.columns = {Track::pixel(52), Track::star()};
    heading.rows = {Track::automatic()};
    heading.columnGap = 16;
    auto& mark = heading.add<DeviceMark>();
    heading.setCell(mark, {0, 0});
    auto& titleArea = heading.add<StackPanel>();
    titleArea.spacing = 3;
    heading.setCell(titleArea, {0, 1});
    auto& title = titleArea.add<TextBlock>(L"电脑信息", 28.f);
    title.weight = 600;
    auto& subtitle = titleArea.add<TextBlock>(L"这台电脑的硬件、系统与设备标识", 13.f);
    subtitle.secondary = true;
    subtitle.wrap = true;

    auto& toolbar = add<ActionToolbar>();
    setDock(toolbar, Dock::Top);
    status_ = &toolbar.add<TextBlock>(L"正在读取电脑信息…", 13.f);
    status_->secondary = true;
    status_->wrap = true;
    status_->verticalAlignment = TextVerticalAlign::Center;
    auto& actions = toolbar.add<WrapPanel>();
    actions.itemSpacing = actions.lineSpacing = 8;
    collectLogs_ = &actions.add<Button>(L"收集日志");
    collectLogs_->name = L"收集日志";
    collectLogs_->width = 114;
    collectLogs_->height = 36;
    collectLogs_->glyph = std::wstring(1, static_cast<wchar_t>(IconSymbol::ReportDocument));
    collectLogs_->fontFamily = Typography::Family;
    collectLogs_->tooltip = L"收集 Windows 事件日志和故障诊断资料";
    collectLogs_->onClick = [this] { if (onCollectLogs) onCollectLogs(); };
    copy_ = &actions.add<Button>(L"复制全部");
    copy_->width = 114;
    copy_->glyph = std::wstring(1, static_cast<wchar_t>(IconSymbol::Copy));
    copy_->name = L"复制全部电脑信息";
    copy_->fontFamily = Typography::Family;
    copy_->height = 36;
    copy_->tooltip = L"复制全部 16 项信息";
    copy_->enabled = false;
    copy_->onClick = [this] { CopyAll(); };
    refresh_ = &actions.add<Button>(L"刷新");
    refresh_->width = 88;
    refresh_->glyph = std::wstring(1, static_cast<wchar_t>(IconSymbol::Refresh));
    refresh_->fontFamily = Typography::Family;
    refresh_->height = 36;
    refresh_->accent = true;
    refresh_->tooltip = L"重新读取电脑信息（F5）";
    refresh_->onClick = [this] { if (onRefresh) onRefresh(); };

    progress_ = &add<ProgressBar>();
    progress_->height = 3;
    progress_->strokeWidth = 3;
    progress_->indeterminate = true;
    progress_->name = L"正在采集电脑信息";
    setDock(*progress_, Dock::Top);
    AddLogPanel();

    footnote_ = &add<TextBlock>(L"共 16 项  ·  可选择文字或右键复制", 12.f);
    footnote_->secondary = true;
    footnote_->wrap = true;
    setDock(*footnote_, Dock::Bottom);

    auto& scroll = add<ScrollViewer>();
    scroll.name = L"电脑信息列表";
    auto& content = scroll.add<StackPanel>();
    content.spacing = 14;
    content.padding = {0, 0, 6, 4};
    auto& cards = content.add<CardGrid>();
    cards.name = L"九宫格电脑信息";
    AddCard(cards, L"主板与系统", static_cast<wchar_t>(IconSymbol::System), {0, 1});
    AddCard(cards, L"处理器", static_cast<wchar_t>(IconSymbol::Processing), {2});
    AddCard(cards, L"显卡", static_cast<wchar_t>(IconSymbol::TVMonitorSelected), {3});
    AddCard(cards, L"显示器", static_cast<wchar_t>(IconSymbol::TVMonitor), {4});
    AddCard(cards, L"内存", static_cast<wchar_t>(IconSymbol::SDCard), {5});
    AddCard(cards, L"存储", static_cast<wchar_t>(IconSymbol::HardDrive), {6});
    AddCard(cards, L"固件与安全", static_cast<wchar_t>(IconSymbol::Shield), {7, 8, 15});
    AddCard(cards, L"设备标识", static_cast<wchar_t>(IconSymbol::Fingerprint), {9, 10, 11});
    AddCard(cards, L"网络连接", static_cast<wchar_t>(IconSymbol::Network), {12, 13, 14});

    diagnosticPanel_ = &content.add<Border>();
    diagnosticPanel_->visible = false;
    auto& details = diagnosticPanel_->add<StackPanel>();
    auto& note = details.add<TextBlock>(L"采集提示", 14.f);
    note.weight = 600;
    diagnostic_ = &details.add<CopyableText>();
    diagnostic_->wordWrap = true;
    diagnostic_->font = {12, 400};
    diagnostic_->copyLabel = L"复制";
    diagnostic_->selectAllLabel = L"全选";
    diagnostic_->tabStop = true;
}

void InformationView::AddLogPanel()
{
    using namespace fluent;
    logPanel_ = &add<Border>();
    setDock(*logPanel_, Dock::Top);
    logPanel_->visible = false;
    logPanel_->padding = {12, 8, 12, 8};
    logPanel_->radius = 8;
    auto& content = logPanel_->add<StackPanel>();
    content.spacing = 4;
    auto& line = content.add<TableLayout>();
    line.columns = {Track::star(), Track::automatic()};
    line.rows = {Track::automatic()};
    line.columnGap = 8;
    logStatus_ = &line.add<TextBlock>(L"", 13.f);
    logStatus_->wrap = true;
    logStatus_->verticalAlignment = TextVerticalAlign::Center;
    auto& actions = line.add<StackPanel>(Orientation::Horizontal);
    actions.spacing = 6;
    line.setCell(actions, {0, 1});
    cancelLogs_ = &actions.add<Button>(L"取消收集");
    cancelLogs_->onClick = [this] {
        if (onCancelLogs) onCancelLogs();
        cancelLogs_->enabled = false;
        logStatus_->text = L"正在取消，已收集的文件将保留…";
        invalidate(true);
    };
    openLogReport_ = &actions.add<Button>(L"打开报告");
    openLogReport_->onClick = [this] { OpenLogOutput(logReport_); };
    openLogFolder_ = &actions.add<Button>(L"打开目录");
    openLogFolder_->onClick = [this] { OpenLogOutput(logDirectory_); };
    for (auto* button : {cancelLogs_, openLogReport_, openLogFolder_})
    {
        button->height = 30;
        button->horizontalPadding = 9;
        button->name = button->text;
    }
    logProgress_ = &content.add<ProgressBar>();
    logProgress_->height = logProgress_->strokeWidth = 3;
    logProgress_->name = L"日志收集进度";
    logPath_ = &content.add<CopyableText>();
    logPath_->wordWrap = true;
    logPath_->font = {12, 400};
    logPath_->copyLabel = L"复制";
    logPath_->selectAllLabel = L"全选";
}

void InformationView::OpenLogOutput(const std::filesystem::path& path)
{
    if (path.empty()) return;
    const auto opened = ShellExecuteW(host()->hwnd(), L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32)
    {
        logStatus_->text = L"无法打开此位置，请查看下方日志目录。";
        invalidate(true);
    }
}

void InformationView::SetLogUpdate(const LogUpdate& update)
{
    const bool finished = update.Finished();
    logPanel_->visible = true;
    collectLogs_->enabled = finished;
    collectLogs_->text = finished ? L"收集日志" : L"收集中";
    cancelLogs_->visible = !finished;
    cancelLogs_->enabled = update.state == LogState::Running;
    logProgress_->visible = !finished;
    logProgress_->indeterminate = !update.total;
    if (update.total)
    {
        logProgress_->setRange(0, static_cast<double>(update.total));
        logProgress_->setValue(static_cast<double>(update.completed));
    }
    logDirectory_ = update.directory;
    logReport_ = update.report;
    openLogFolder_->visible = finished && !logDirectory_.empty();
    openLogReport_->visible = finished && !logReport_.empty();
    logPath_->visible = openLogFolder_->visible;
    if (logPath_->visible)
    {
        const auto location = L"日志目录  " + logDirectory_.wstring();
        logPath_->setText(location);
        logPath_->name = location;
    }
    switch (update.state)
    {
    case LogState::Running:
        logStatus_->text = update.total ? L"正在收集日志  " + std::to_wstring(update.completed) +
            L" / " + std::to_wstring(update.total) : L"正在准备收集日志…";
        if (!update.item.empty()) logStatus_->text += L"  ·  " + update.item;
        break;
    case LogState::Cancelling: logStatus_->text = L"正在取消，已收集的文件将保留…"; break;
    case LogState::Complete: logStatus_->text = L"日志收集完成"; break;
    case LogState::Partial:
        logStatus_->text = L"日志已部分收集  ·  " + std::to_wstring(update.failed + update.partial) + L" 项未完整取得";
        break;
    case LogState::Failed: logStatus_->text = L"日志收集失败"; break;
    case LogState::Cancelled: logStatus_->text = L"日志收集已取消"; break;
    }
    if (finished && update.total) logStatus_->text += L"  ·  已完整保存 " + std::to_wstring(update.saved) + L" 项";
    if (finished && !update.detail.empty()) logStatus_->text += L"  " + update.detail;
    invalidate(true);
}

void InformationView::AddCard(fluent::TableLayout& parent, const wchar_t* title,
    wchar_t glyph, std::initializer_list<std::size_t> fields)
{
    using namespace fluent;
    auto& card = parent.add<Border>();
    card.name = std::wstring(title) + L"卡片";
    card.padding = {18, 14, 18, 16};
    card.radius = 10;
    card.minHeight = 110;
    auto& section = card.add<StackPanel>();
    section.spacing = 12;
    auto& heading = section.add<StackPanel>(Orientation::Horizontal);
    heading.spacing = 10;
    auto& icon = heading.add<Icon>(static_cast<IconSymbol>(glyph));
    icon.width = 22;
    icon.iconSize = 20;
    auto& label = heading.add<TextBlock>(title, 15.f);
    label.weight = 600;
    auto& list = section.add<StackPanel>();
    list.spacing = 10;
    for (const std::size_t index : fields)
    {
        auto& row = list.add<StackPanel>();
        row.spacing = 3;
        if (fields.size() > 1)
        {
            auto& rowLabel = row.add<TextBlock>(kLabels[index], 12.f);
            rowLabel.secondary = true;
        }
        auto& value = row.add<CopyableText>(L"正在读取…");
        value.name = kLabels[index];
        value.font = fields.size() == 1 ? TextStyle{15, 500} : TextStyle{13, 400};
        value.wordWrap = true;
        value.tabStop = true;
        value.copyLabel = L"复制";
        value.selectAllLabel = L"全选";
        value.onCopied = [this](bool success) {
            SetStatus(success ? L"已复制所选文字" : L"复制失败，请重试");
        };
        values_[index] = &value;
    }
}

void InformationView::SetBusy()
{
    copy_->enabled = false;
    values_[6]->setText(L"正在读取 SSD SMART…");
    values_[6]->name = L"SSD信息：正在读取 SSD SMART…";
    refresh_->enabled = false;
    refresh_->text = L"读取中";
    progress_->visible = true;
    status_->text = L"正在读取电脑信息…";
    invalidate(true);
}

void InformationView::SetSnapshot(const CollectionUpdate& update)
{
    const auto& snapshot = update.snapshot;
    snapshot_ = snapshot;
    std::size_t available = 0;
    for (std::size_t index = 0; index < kLabels.size(); ++index)
    {
        const auto row = std::find_if(snapshot.rows.begin(), snapshot.rows.end(),
            [index](const auto& item) { return item.label == kLabels[index]; });
        const bool waiting = index == 6 ? update.ssdPending : update.computerPending;
        const std::wstring value = row != snapshot.rows.end() ? row->value : (waiting ? L"正在读取…" : L"N/A");
        const auto accessibleName = std::wstring(kLabels[index]) + L"：" + value;
        if (values_[index]->name != accessibleName)
        {
            values_[index]->setText(value);
            values_[index]->name = accessibleName;
            values_[index]->notifyChanged();
        }
        if (!waiting && !value.empty() && value != L"N/A") ++available;
    }
    std::wstring details = snapshot.error;
    if (!snapshot.diagnostic.empty())
    {
        if (!details.empty()) details += L"\r\n";
        details += snapshot.diagnostic;
    }
    diagnostic_->setText(details);
    diagnosticPanel_->visible = !details.empty();
    refresh_->text = update.Complete() ? L"刷新" : L"读取中";
    refresh_->enabled = update.Complete();
    copy_->enabled = !update.computerPending && !snapshot.rows.empty();
    progress_->visible = !update.Complete();
    status_->text = snapshot.error.empty()
        ? L"已更新 " + TimeText() + L"  ·  已获取 " + std::to_wstring(available) + L" / 16 项"
        : L"采集失败，请查看下方提示后重试";
    if (!details.empty() && snapshot.error.empty()) status_->text += L"  ·  有采集提示";
    if (update.ssdPending) status_->text += L"  ·  正在读取 SSD SMART…";
    if (update.computerPending) status_->text = L"正在读取其他电脑信息…";
    footnote_->text = L"共 16 项  ·  N/A 表示未获取到信息  ·  可选择文字或右键复制";
    invalidate(true);
}

void InformationView::SetStatus(std::wstring text)
{
    status_->text = std::move(text);
    invalidate(true);
}

void InformationView::CopyAll()
{
    if (snapshot_.rows.empty() || !host()) return;
    const bool copied = CopyText(host()->hwnd(), FormatReport(snapshot_));
    SetStatus(copied ? L"已复制当前电脑信息" : L"剪贴板暂不可用，请重试");
}
}
