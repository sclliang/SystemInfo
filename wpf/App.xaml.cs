using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows;
using SystemInfo.Interop;
using SystemInfo.Logic;
using SystemInfo.Models;

namespace SystemInfo
{
    public partial class App : Application
    {
        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);
            Options options;
            try { options = Options.Parse(e.Args); }
            catch (ArgumentException) { Shutdown(2); return; }
            catch (Exception) { Shutdown(2); return; }
            try
            {
                Wpf.Ui.Appearance.ApplicationThemeManager.Apply(Wpf.Ui.Appearance.ApplicationTheme.Light,
                    Wpf.Ui.Controls.WindowBackdropType.Mica, false);
                if (options.Report != null)
                {
                    using (var native = new NativeSession(false)) Shutdown(native.WriteReport(options.Report, options.AmiOnly));
                }
                else if (options.Smoke && options.Snapshot == null)
                    Shutdown(Smoke(options.SmokeLogs));
                else
                {
                    var window = new MainWindow(options);
                    MainWindow = window;
                    window.Show();
                }
            }
            catch (Exception error)
            {
                if (!options.Smoke && options.Report == null)
                    MessageBox.Show("程序启动失败：\n" + error.Message, "SystemInfo", MessageBoxButton.OK, MessageBoxImage.Error);
                WriteFailure(options, error);
                Shutdown(1);
            }
        }

        private static int Smoke(bool logs)
        {
            using (var session = new NativeSession())
            {
                if (logs && !session.Perform(2, IntPtr.Zero)) return 1;
                var clock = Stopwatch.StartNew();
                ulong revision = 0;
                while (clock.Elapsed.TotalSeconds < (logs ? 480 : 120))
                {
                    Snapshot snapshot = session.Poll(revision);
                    if (snapshot != null)
                    {
                        revision = snapshot.Revision;
                        if (snapshot.Complete && (!logs || snapshot.Log != null && !snapshot.Log.Active))
                            return snapshot.Valid && (!logs || snapshot.Log.HasReport) ? 0 : 1;
                    }
                    Thread.Sleep(80);
                }
                return 3;
            }
        }

        internal static void WriteFailure(Options options, Exception error)
        {
            string output = options.Snapshot ?? options.Report;
            if (output == null) return;
            try { File.WriteAllText(output + ".error.txt", error.ToString(), new UTF8Encoding(true)); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }
}
