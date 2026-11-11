using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SystemInfo.Logic;
using SystemInfo.Models;
using SystemInfo.ViewModels;

internal static class Program
{
    private static int passed;
    [STAThread]
    private static int Main()
    {
        try
        {
            Run("CLI Unicode paths and layout options", () =>
            {
                var options = Options.Parse(new[] { "--report", "中文 空格.txt" });
                Check(options.Report == Path.GetFullPath("中文 空格.txt"), "Unicode path changed");
                options = Options.Parse(new[] { "--smoke-logs", "--snapshot", "界面.png", "--size", "640", "520", "--scale", "1.5" });
                Check(options.Smoke && options.SmokeLogs && options.Width == 640 && options.Height == 520 && options.Scale == 1.5, "Layout options changed");
            });
            Run("CLI rejects invalid modes and nonfinite numbers", () =>
            {
                foreach (var args in new[] { new[] { "--report" }, new[] { "--report", "a", "--ami-report", "b" },
                    new[] { "--report", "a", "--smoke" }, new[] { "--scale", "NaN" }, new[] { "--scale", "Infinity" },
                    new[] { "--size", "0", "520" }, new[] { "--unknown" } })
                {
                    bool rejected = false;
                    try { Options.Parse(args); } catch (ArgumentException) { rejected = true; }
                    Check(rejected, "Accepted: " + string.Join(" ", args));
                }
            });
            Run("Native JSON preserves zero battery and escaped fields", () =>
            {
                var snapshot = Snapshot.Parse("{\"revision\":42,\"machine\":\"电脑\",\"computerPending\":false,\"ssdPending\":true,\"rows\":[{\"label\":\"CPU型号\",\"value\":\"CPU\\r\\n第二行\"}],\"battery\":{\"percent\":0,\"present\":true,\"state\":\"放电\",\"packs\":[{\"name\":\"电池\",\"fields\":[{\"label\":\"循环次数\",\"value\":\"0\"}]}]}}");
                Check(snapshot.Revision == 42 && snapshot.Rows[0].Value == "CPU\r\n第二行", "JSON text changed");
                Check(snapshot.Battery.Percent == 0 && snapshot.Battery.Packs[0].Fields[0].Value == "0", "Zero battery lost");
            });
            Run("All 17 fields appear once across six sections", () =>
            {
                var model = new Dashboard();
                var fields = model.Sections.SelectMany(section => section.Fields).ToList();
                Check(model.Sections.Count == 6 && fields.Count == 17 && fields.Select(field => field.Key).Distinct().Count() == 17, "Missing or duplicate field");
                Check(!model.CanRefresh && !model.CanCopy && !model.CanCollect, "Actions enabled before native session");
            });
            Run("SSD completes independently from basic hardware", () =>
            {
                var snapshot = Basic(); snapshot.ComputerPending = true; snapshot.SsdPending = false;
                snapshot.Rows = new List<Field> { new Field { Label = "SSD信息", Value = DiskText } };
                var model = new Dashboard(); model.Update(snapshot);
                Check(Storage(model).HasDrives && model.Sections[0].Fields[1].Value == "正在读取…", "SSD waits for hardware");
                snapshot = Basic(); snapshot.SsdPending = true; snapshot.Rows.RemoveAll(row => row.Label == "SSD信息"); model.Update(snapshot);
                Check(model.CanCopy && !model.CanRefresh && model.Status.Contains("SSD") && !Storage(model).HasDrives, "Hardware waits for SSD");
            });
            Run("Refresh preserves previous values while pending", () =>
            {
                var model = new Dashboard(); var snapshot = Basic(); model.Update(snapshot);
                string cpu = model.Sections[0].Fields[1].Value;
                snapshot.ComputerPending = snapshot.SsdPending = true; model.Update(snapshot);
                Check(model.Sections[0].Fields[1].Value == cpu && Storage(model).HasDrives && !model.CanCopy && !model.CanRefresh, "Refresh erased usable values");
            });
            Run("Multi-drive parsing distinguishes zero and unavailable", () =>
            {
                var drives = Drive.Parse(DiskText + "\r\n\r\nSSD-B\r\n2.00 TB  ·  SATA/600\r\n上电 N/A  ·  N/A 次\r\n累计读取 N/A  ·  写入 0.00 GiB\r\n设备说明");
                Check(drives.Count == 2 && drives[0].Metrics[0].Value == "0 小时" && drives[0].Metrics[1].Value == "0 次", "Zero values changed");
                Check(drives[1].Metrics[0].Value == "N/A" && drives[1].Metrics[3].Value == "0.00 GiB" && drives[1].Notes == "设备说明", "Unknown values or notes lost");
            });
            Run("Unknown battery differs from zero charge", () =>
            {
                var model = new Dashboard(); var snapshot = Basic(); model.Update(snapshot);
                var power = model.Sections.Single(section => section.Kind == "power");
                Check(!power.HasPercent, "Unknown charge rendered as zero");
                snapshot.Battery.Percent = 0; model.Update(snapshot);
                Check(power.HasPercent && power.PercentText == "0%", "Zero charge hidden");
            });
            Run("Log cancellation, completion and retry actions", () =>
            {
                var model = new Dashboard(); var snapshot = Basic();
                snapshot.Log = new LogStatus { State = "running", Completed = 5, Total = 20 }; model.Update(snapshot);
                Check(!model.CanCollect && model.CanCancel && model.LogProgress == 25 && !model.CanOpenReport, "Running actions incorrect");
                snapshot.Log.State = "cancelling"; model.Update(snapshot);
                Check(!model.CanCancel && !model.CanCollect && model.LogActive, "Cancellation allows duplicate collect");
                snapshot.Log.State = "cancelled"; snapshot.Log.HasReport = true; snapshot.Log.Directory = "C:\\日志"; model.Update(snapshot);
                Check(model.CanCollect && model.CanOpenReport && model.CanOpenFolder && !model.LogActive, "Cancelled report cannot be inspected or retried");
                snapshot.Log.State = "failed"; snapshot.Log.HasReport = false; snapshot.Log.Directory = ""; model.Update(snapshot);
                Check(model.CanCollect && !model.CanOpenReport && !model.CanOpenFolder, "Failed output exposed");
            });
            Run("Smoke rejects duplicate, incomplete and failed snapshots", () =>
            {
                var snapshot = Basic(); Check(snapshot.Valid, "Complete payload rejected");
                snapshot.Rows[0].Label = snapshot.Rows[1].Label; Check(!snapshot.Valid, "Duplicate field accepted");
                snapshot = Basic(); snapshot.SsdPending = true; Check(!snapshot.Valid, "Pending data accepted");
                snapshot = Basic(); snapshot.Error = "失败"; Check(!snapshot.Valid, "Failed data accepted");
            });
            Console.WriteLine($"Passed {passed} WPF data and behavior checks.");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
    private const string DiskText = "SSD-A\r\n1.00 TB  ·  NVMe · PCIe 4.0 x4\r\n上电 0 小时  ·  0 次\r\n累计读取 0.00 GiB  ·  写入 N/A";
    private static Section Storage(Dashboard model) => model.Sections.Single(section => section.Kind == "storage");
    private static Snapshot Basic()
    {
        var model = new Dashboard();
        return new Snapshot { Revision = 1, Machine = "测试机", Updated = "12:30:00", Error = "", Diagnostic = "",
            Rows = model.Sections.SelectMany(section => section.Fields).Select(field =>
                new Field { Label = field.Key, Value = field.Key == "SSD信息" ? DiskText : "有效值" }).ToList(),
            Battery = new Battery { State = "外部电源", Packs = new List<BatteryPack>() } };
    }
    private static void Run(string name, Action test) { test(); ++passed; Console.WriteLine("PASS " + name); }
    private static void Check(bool condition, string error) { if (!condition) throw new InvalidOperationException(error); }
}
