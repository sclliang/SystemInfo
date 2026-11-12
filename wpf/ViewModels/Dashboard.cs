using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using SystemInfo.Models;
using Wpf.Ui.Controls;

namespace SystemInfo.ViewModels
{
    internal abstract class Observable : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;
        protected void Changed([CallerMemberName] string name = null) =>
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        protected bool Set<T>(ref T field, T value, [CallerMemberName] string name = null)
        {
            if (EqualityComparer<T>.Default.Equals(field, value)) return false;
            field = value;
            Changed(name);
            return true;
        }
    }

    internal sealed class Readout : Observable
    {
        public string Key { get; }
        public string Label { get; }
        private string value = "正在读取…";
        public string Value { get => value; set => Set(ref this.value, value); }
        public Readout(string key, string label = null) { Key = key; Label = label ?? key; }
    }

    internal sealed class Section : Observable
    {
        public string Kind { get; }
        public string Title { get; }
        public SymbolRegular Icon { get; }
        public int Column { get; }
        public List<Readout> Fields { get; }
        private Battery battery;
        public Battery Battery { get => battery; private set { Set(ref battery, value); Changed(nameof(HasPercent)); Changed(nameof(PercentText)); } }
        public bool HasPercent => Battery?.Percent != null;
        public string PercentText => Battery?.Percent is int percent ? percent + "%" : "";
        public Section(string kind, string title, SymbolRegular icon, int column, params Readout[] fields)
        { Kind = kind; Title = title; Icon = icon; Column = column; Fields = fields.ToList(); }

        public void Update(Snapshot snapshot)
        {
            foreach (var field in Fields)
            {
                string found = snapshot.Rows?.FirstOrDefault(row => row.Label == field.Key)?.Value;
                bool pending = field.Key == "SSD信息" ? snapshot.SsdPending : snapshot.ComputerPending;
                field.Value = string.IsNullOrEmpty(found) ? (pending ? "正在读取…" : "N/A") : found;
            }
            if (Kind == "power") Battery = snapshot.Battery;
        }
    }

    internal sealed class Dashboard : Observable
    {
        public List<Section> Sections { get; } = new List<Section>
        {
            new Section("hardware", "硬件配置", SymbolRegular.DeveloperBoard24, 0,
                new Readout("主板型号", "主板"), new Readout("CPU型号", "处理器"),
                new Readout("内存信息", "内存"), new Readout("GPU型号", "显卡"),
                new Readout("显示器信息", "显示器"), new Readout("OS信息", "操作系统")),
            new Section("power", "电池与电源", SymbolRegular.Battery624, 1,
                new Readout("电池信息", "供电状态")),
            new Section("network", "网络连接", SymbolRegular.Wifi124, 0,
                new Readout("有线网卡", "以太网"), new Readout("无线网卡", "无线网络"), new Readout("蓝牙地址", "蓝牙")),
            new Section("storage", "SSD 存储", SymbolRegular.Database24, 0,
                new Readout("SSD信息", "固态硬盘")),
            new Section("firmware", "固件与安全", SymbolRegular.ShieldCheckmark24, 1,
                new Readout("BIOS版本", "BIOS 版本"), new Readout("EC版本", "EC 版本"), new Readout("TPM型号", "TPM")),
            new Section("identity", "设备标识", SymbolRegular.Desktop24, 1,
                new Readout("系统SN", "系统 SN"), new Readout("主板SN", "主板 SN"), new Readout("UUID", "UUID")),
            new Section("windows", "Windows 授权信息", SymbolRegular.Key24, 1,
                new Readout("Windows 产品 ID", "产品 ID"), new Readout("Windows OEM 密钥", "OEM 密钥"))
        };

        private Snapshot snapshot;
        public Snapshot Snapshot => snapshot;
        public string Machine => snapshot?.Machine ?? Environment.MachineName;
        public string Updated => string.IsNullOrEmpty(snapshot?.Updated) ? "" : "更新于 " + snapshot.Updated;
        public string Status => snapshot == null || snapshot.ComputerPending ? "正在读取硬件信息"
            : snapshot.SsdPending ? "正在读取 SSD 数据" : string.IsNullOrEmpty(snapshot.Error) ? "信息已更新" : "部分信息读取失败";
        public bool Busy => snapshot == null || !snapshot.Complete;
        public bool CanRefresh => snapshot != null && snapshot.Complete;
        public bool CanCopy => snapshot?.Rows?.Count > 0 && !snapshot.ComputerPending;
        public bool CanCollect => snapshot != null && snapshot.Log?.Active != true;
        public string Diagnostic => string.Join(Environment.NewLine,
            new[] { snapshot?.Error, snapshot?.Diagnostic }.Where(text => !string.IsNullOrWhiteSpace(text)));
        public bool HasDiagnostic => !string.IsNullOrEmpty(Diagnostic);
        public bool HasLog => snapshot?.Log != null;
        public bool LogActive => snapshot?.Log?.Active == true;
        public bool CanCancel => snapshot?.Log?.State == "running";
        public bool CanOpenReport => snapshot?.Log != null && !LogActive && snapshot.Log.HasReport;
        public bool CanOpenFolder => snapshot?.Log != null && !LogActive && !string.IsNullOrEmpty(snapshot.Log.Directory);
        public double LogProgress => snapshot?.Log?.Total > 0 ? 100.0 * snapshot.Log.Completed / snapshot.Log.Total : 0;
        public bool LogIndeterminate => LogActive && snapshot.Log.Total == 0;
        public string LogPath => snapshot?.Log?.Directory ?? "";
        public string LogTitle
        {
            get
            {
                var log = snapshot?.Log;
                if (log == null) return "";
                switch (log.State)
                {
                    case "running": return "正在收集日志";
                    case "cancelling": return "正在取消收集…";
                    case "complete": return "日志收集完成";
                    case "partial": return "日志已收集，部分项目不可用";
                    case "cancelled": return "日志收集已取消";
                    default: return "日志收集失败";
                }
            }
        }
        public string LogDetail
        {
            get
            {
                var log = snapshot?.Log;
                if (log == null) return "";
                if (log.Active) return (log.Total > 0 ? $"{log.Completed} / {log.Total} 项  ·  " : "")
                    + (string.IsNullOrEmpty(log.Item) ? "正在准备采集" : log.Item);
                return $"已保存 {log.Saved} 项  ·  部分完成 {log.Partial} 项  ·  失败 {log.Failed} 项"
                    + (string.IsNullOrEmpty(log.Detail) ? "" : Environment.NewLine + log.Detail);
            }
        }

        public void Update(Snapshot value)
        {
            snapshot = value;
            foreach (var section in Sections) section.Update(value);
            Changed("");
        }
    }
}
