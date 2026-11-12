using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using SystemInfo.Interop;
using SystemInfo.Logic;
using SystemInfo.ViewModels;

namespace SystemInfo
{
    public partial class MainWindow : Wpf.Ui.Controls.FluentWindow
    {
        private readonly Options options;
        private readonly Dashboard dashboard = new Dashboard();
        private readonly DispatcherTimer timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(120) };
        private readonly DispatcherTimer toastTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
        private readonly Stopwatch elapsed = Stopwatch.StartNew();
        private readonly BindingFailures bindingFailures = new BindingFailures();
        private NativeSession session;
        private IntPtr handle;
        private ulong revision;
        private bool closing, closed, smokeLogsStarted, captured;
        private int exitCode;

        internal MainWindow(Options options)
        {
            this.options = options;
            if (options.Smoke) PresentationTraceSources.DataBindingSource.Listeners.Add(bindingFailures);
            InitializeComponent();
            DataContext = dashboard;
            if (options.Scale != 1) ContentRoot.LayoutTransform = new ScaleTransform(options.Scale, options.Scale);
            SourceInitialized += InitializeNative;
            ContentRendered += (_, __) => Poll();
            Viewport.SizeChanged += (_, __) => AdaptHeader();
            PreviewKeyDown += Window_KeyDown;
            Closing += Window_Closing;
            timer.Tick += (_, __) => Poll();
            toastTimer.Tick += (_, __) => { Toast.Visibility = Visibility.Collapsed; toastTimer.Stop(); };
        }

        private void InitializeNative(object sender, EventArgs args)
        {
            handle = new WindowInteropHelper(this).Handle;
            session = new NativeSession();
            if (!session.Position(handle, options.Width, options.Height)) WindowStartupLocation = WindowStartupLocation.CenterScreen;
            timer.Start();
        }

        private void AdaptHeader()
        {
            bool narrow = Viewport.ActualWidth / options.Scale < 930;
            Grid.SetRow(Actions, narrow ? 1 : 0);
            Grid.SetColumn(Actions, narrow ? 0 : 1);
            Grid.SetColumnSpan(Actions, narrow ? 2 : 1);
            Grid.SetColumnSpan(IntroIdentity, narrow ? 2 : 1);
            Actions.HorizontalAlignment = narrow ? HorizontalAlignment.Left : HorizontalAlignment.Right;
            Actions.Margin = narrow ? new Thickness(0, 13, 0, 0) : new Thickness(0);
        }

        private void Poll()
        {
            if (closing || session == null) return;
            try
            {
                var snapshot = session.Poll(revision);
                if (snapshot != null) { revision = snapshot.Revision; dashboard.Update(snapshot); }
                if (options.SmokeLogs && !smokeLogsStarted && dashboard.CanCollect)
                {
                    smokeLogsStarted = true;
                    CollectButton.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                }
                bool finished = dashboard.Snapshot?.Complete == true && (!options.SmokeLogs ||
                    dashboard.Snapshot.Log != null && !dashboard.Snapshot.Log.Active);
                if (finished && options.Snapshot != null && !captured)
                {
                    captured = true;
                    Dispatcher.BeginInvoke(DispatcherPriority.ContextIdle, new Action(() => Capture()));
                }
                if (options.Smoke && elapsed.Elapsed.TotalSeconds > (options.SmokeLogs ? 480 : 120))
                { exitCode = 3; Close(); }
            }
            catch (Exception error)
            {
                if (options.Smoke) { App.WriteFailure(options, error); exitCode = 1; Close(); }
                else { ShowToast(error.Message); timer.Stop(); }
            }
        }

        private void Capture()
        {
            if (closing) return;
            try
            {
                UpdateLayout();
                var bitmap = new RenderTargetBitmap((int)Math.Ceiling(ActualWidth), (int)Math.Ceiling(ActualHeight), 96, 96, PixelFormats.Pbgra32);
                // WPF snapshots exclude DWM's material. Composite the content
                // over the light theme's fallback so transparent areas remain readable.
                var visual = new DrawingVisual();
                using (var drawing = visual.RenderOpen())
                {
                    var bounds = new Rect(0, 0, ActualWidth, ActualHeight);
                    drawing.DrawRectangle((Brush)FindResource("ApplicationBackgroundBrush"), null, bounds);
                    drawing.DrawRectangle(new VisualBrush(this), null, bounds);
                }
                bitmap.Render(visual);
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using (var file = File.Create(options.Snapshot)) encoder.Save(file);
                if (bindingFailures.HasErrors) throw new InvalidOperationException("WPF 数据绑定错误：\n" + bindingFailures);
                if (Sections.Items.Count != 7 || dashboard.Sections.Sum(section => section.Fields.Count) != 19)
                    throw new InvalidOperationException("界面信息分区不完整。");
                if (options.Smoke) VerifyAppearance();
                if (!dashboard.Snapshot.Valid || options.SmokeLogs && !dashboard.Snapshot.Log.HasReport)
                    exitCode = 1;
                if (options.Smoke) Close();
            }
            catch (Exception error) { App.WriteFailure(options, error); exitCode = 3; if (options.Smoke) Close(); else ShowToast("保存界面图失败：" + error.Message); }
        }

        private void Perform(uint operation, string success = null)
        {
            if (session == null || closing) return;
            try
            {
                if (session.Perform(operation, handle))
                {
                    if (success != null) ShowToast(success);
                    Poll();
                }
                else ShowToast("当前操作暂不可用，请稍后重试。");
            }
            catch (Exception error) { ShowToast(error.Message); }
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out int value, int size);

        private void VerifyAppearance()
        {
            if (WindowBackdropType != Wpf.Ui.Controls.WindowBackdropType.Mica ||
                Wpf.Ui.Appearance.ApplicationThemeManager.GetAppTheme() != Wpf.Ui.Appearance.ApplicationTheme.Light)
                throw new InvalidOperationException("窗口未使用浅色云母主题。");
            AppTitleBar.ApplyTemplate();
            var close = AppTitleBar.Template.FindName("PART_CloseButton", AppTitleBar) as Wpf.Ui.Controls.TitleBarButton;
            var red = (close?.MouseOverBackground as SolidColorBrush)?.Color;
            var white = (close?.MouseOverButtonsForeground as SolidColorBrush)?.Color;
            if (!red.HasValue || red.Value.R < 180 || red.Value.G > 130 || red.Value.B > 130 || white != Colors.White)
                throw new InvalidOperationException("关闭按钮未配置红底白色悬停效果。");
            string order = string.Join(",", dashboard.Sections.Where(section => section.Column == 0).Select(section => section.Kind));
            if (order != "hardware,network,storage") throw new InvalidOperationException("网络与 SSD 顺序不正确。");
            int backdrop = 0;
            int status = DwmGetWindowAttribute(handle, 38, out backdrop, sizeof(int));
            if (Wpf.Ui.Controls.WindowBackdrop.IsSupported(Wpf.Ui.Controls.WindowBackdropType.Mica) && (status != 0 || backdrop != 2))
                throw new InvalidOperationException("Windows 未启用云母背景。");
            File.WriteAllText(options.Snapshot + ".checks.txt",
                "Theme=Light\r\nBackdrop=Mica\r\nDwmBackdrop=" + backdrop + "\r\nCloseHover=" + red +
                "\r\nCloseHoverForeground=" + white + "\r\nColumnOrder=" + order + "\r\nFields=19\r\nBindingErrors=0\r\n", new UTF8Encoding(true));
        }

        private void ShowToast(string text)
        {
            ToastText.Text = text;
            Toast.Visibility = Visibility.Visible;
            toastTimer.Stop(); toastTimer.Start();
        }

        private void Window_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.F5) { if (dashboard.CanRefresh) Perform(1); e.Handled = true; }
            if (e.Key == Key.C && Keyboard.Modifiers == (ModifierKeys.Control | ModifierKeys.Shift))
            { if (dashboard.CanCopy) Perform(4, "已复制全部电脑信息"); e.Handled = true; }
        }

        private void Refresh_Click(object sender, RoutedEventArgs e) { if (dashboard.CanRefresh) Perform(1); }
        private void Copy_Click(object sender, RoutedEventArgs e) { if (dashboard.CanCopy) Perform(4, "已复制全部电脑信息"); }
        private void Collect_Click(object sender, RoutedEventArgs e) { if (dashboard.CanCollect) Perform(2); }
        private void Cancel_Click(object sender, RoutedEventArgs e) { if (dashboard.CanCancel) Perform(3); }
        private void OpenReport_Click(object sender, RoutedEventArgs e) => Perform(5);
        private void OpenFolder_Click(object sender, RoutedEventArgs e) => Perform(6);

        private async void Window_Closing(object sender, CancelEventArgs e)
        {
            if (closed) return;
            e.Cancel = true;
            if (closing) return;
            closing = true;
            timer.Stop(); toastTimer.Stop();
            ContentRoot.IsEnabled = false;
            ShowToast("正在结束采集并清理临时文件…");
            var owned = session;
            session = null;
            try { if (owned != null) await Task.Run(() => owned.Dispose()); }
            catch (Exception error) { App.WriteFailure(options, error); exitCode = 1; }
            PresentationTraceSources.DataBindingSource.Listeners.Remove(bindingFailures);
            toastTimer.Stop();
            closed = true;
            Close();
            Application.Current.Shutdown(exitCode);
        }

        private sealed class BindingFailures : TraceListener
        {
            private readonly StringBuilder text = new StringBuilder();
            public bool HasErrors => text.Length > 0;
            public override void Write(string message) => text.Append(message);
            public override void WriteLine(string message) => text.AppendLine(message);
            public override string ToString() => text.ToString();
        }
    }
}
