#pragma once
#include <fluentui/controls.hpp>
#include <fluentui/layout.hpp>
#include "Collection.h"
#include "LogCollection.h"
#include <array>
#include <functional>

namespace system_info
{
class InformationView final : public fluent::DockPanel
{
public:
    InformationView();
    void SetBusy();
    void SetSnapshot(const CollectionUpdate& update);
    void SetStatus(std::wstring text);
    void SetLogUpdate(const LogUpdate& update);
    void ActivateLogButton() { collectLogs_->activate(); }
    void CopyAll();
    std::function<void()> onRefresh;
    std::function<void()> onCollectLogs, onCancelLogs;

private:
    void AddCard(fluent::TableLayout& parent, const wchar_t* title,
        wchar_t glyph, std::initializer_list<std::size_t> fields);
    void AddLogPanel();
    void OpenLogOutput(const std::filesystem::path& path);
    std::array<fluent::CopyableText*, 16> values_{};
    fluent::Button* refresh_{};
    fluent::Button* copy_{};
    fluent::Button *collectLogs_{}, *cancelLogs_{}, *openLogReport_{}, *openLogFolder_{};
    fluent::Border* logPanel_{};
    fluent::TextBlock* logStatus_{};
    fluent::CopyableText* logPath_{};
    fluent::ProgressBar* logProgress_{};
    std::filesystem::path logDirectory_, logReport_;
    fluent::TextBlock* status_{};
    fluent::TextBlock* footnote_{};
    fluent::ProgressBar* progress_{};
    fluent::Border* diagnosticPanel_{};
    fluent::CopyableText* diagnostic_{};
    ftc::hardware_info::Snapshot snapshot_;
};
}
