using System;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using SystemInfo.Models;

namespace SystemInfo.Interop
{
    // One owner calls this object serially. Native workers keep slow WMI/SMART
    // reads off the dispatcher. No borrowed buffer survives a native call.
    internal sealed class NativeSession : IDisposable
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr NewSession();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void DropSession(IntPtr session);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr PollSession(IntPtr session, ulong revision);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ActionSession(IntPtr session, uint operation, IntPtr owner);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)] private delegate int ExportReport([MarshalAs(UnmanagedType.LPWStr)] string path, int ami);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int PositionWindow(IntPtr window, double width, double height);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string file, IntPtr reserved, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] private static extern bool FreeLibrary(IntPtr module);

        private IntPtr module, session;
        private readonly string directory;
        private readonly DropSession drop;
        private readonly PollSession poll;
        private readonly ActionSession action;
        private readonly ExportReport export;
        private readonly PositionWindow position;

        public NativeSession(bool start = true)
        {
            directory = Path.Combine(Path.GetTempPath(), "SystemInfo-Core-" + Guid.NewGuid().ToString("N"));
            try
            {
                // Elevated processes never load native code from an unprotected
                // shared cache or from the current working directory.
                var security = new DirectorySecurity();
                security.SetAccessRuleProtection(true, false);
                foreach (var sid in new[] { WindowsIdentity.GetCurrent().User,
                    new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
                    new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null) })
                    security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                        InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
                        PropagationFlags.None, AccessControlType.Allow));
                Directory.CreateDirectory(directory, security);
                string path = Path.Combine(directory, "SystemInfo.Native.dll");
                using (var source = Assembly.GetExecutingAssembly().GetManifestResourceStream("SystemInfo.Native.dll"))
                using (var destination = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    if (source == null) throw new InvalidOperationException("缺少内嵌采集模块。");
                    source.CopyTo(destination);
                }
                module = LoadLibraryEx(path, IntPtr.Zero, 0x00000100 | 0x00000800);
                if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "无法加载采集模块");
                drop = Load<DropSession>("si_drop");
                poll = Load<PollSession>("si_poll");
                action = Load<ActionSession>("si_action");
                export = Load<ExportReport>("si_export");
                position = Load<PositionWindow>("si_position");
                if (start)
                {
                    session = Load<NewSession>("si_new")();
                    if (session == IntPtr.Zero) throw new InvalidOperationException("无法启动硬件采集。");
                }
            }
            catch { Dispose(); throw; }
        }

        private T Load<T>(string name) where T : Delegate
        {
            IntPtr address = GetProcAddress(module, name);
            if (address == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "采集接口缺失：" + name);
            return Marshal.GetDelegateForFunctionPointer<T>(address);
        }

        public Snapshot Poll(ulong revision)
        {
            IntPtr address = poll(session, revision);
            if (address == IntPtr.Zero) throw new InvalidOperationException("无法读取采集结果。");
            int length = 0;
            while (Marshal.ReadByte(address, length) != 0)
            {
                if (++length > 16 * 1024 * 1024) throw new InvalidOperationException("采集结果超出大小限制。");
            }
            if (length == 0) return null;
            var bytes = new byte[length];
            Marshal.Copy(address, bytes, 0, length);
            return Snapshot.Parse(Encoding.UTF8.GetString(bytes));
        }

        public bool Perform(uint operation, IntPtr owner)
        {
            int result = action(session, operation, owner);
            if (result < 0) throw new InvalidOperationException("操作失败，请重试。");
            return result == 1;
        }

        public int WriteReport(string path, bool amiOnly) => export(path, amiOnly ? 1 : 0);
        public bool Position(IntPtr window, double width, double height) => position(window, width, height) == 1;

        public void Dispose()
        {
            if (session != IntPtr.Zero) { drop(session); session = IntPtr.Zero; }
            if (module != IntPtr.Zero) { FreeLibrary(module); module = IntPtr.Zero; }
            try
            {
                string file = Path.Combine(directory, "SystemInfo.Native.dll");
                if (File.Exists(file)) File.Delete(file);
                if (Directory.Exists(directory)) Directory.Delete(directory, false);
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }
}
