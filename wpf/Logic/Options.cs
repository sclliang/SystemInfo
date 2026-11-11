using System;
using System.Globalization;
using System.IO;

namespace SystemInfo.Logic
{
    internal sealed class Options
    {
        public string Report { get; private set; }
        public bool AmiOnly { get; private set; }
        public string Snapshot { get; private set; }
        public bool Smoke { get; private set; }
        public bool SmokeLogs { get; private set; }
        public double Width { get; private set; }
        public double Height { get; private set; }
        public double Scale { get; private set; } = 1;

        public static Options Parse(string[] args)
        {
            var options = new Options();
            for (int i = 0; i < args.Length; ++i)
            {
                string flag = args[i];
                switch (flag)
                {
                    case "--report":
                    case "--ami-report":
                        if (options.Report != null) throw new ArgumentException("报告参数重复。");
                        options.Report = Path.GetFullPath(Next(args, ref i));
                        options.AmiOnly = flag == "--ami-report";
                        break;
                    case "--snapshot":
                        options.Snapshot = Path.GetFullPath(Next(args, ref i));
                        break;
                    case "--smoke": options.Smoke = true; break;
                    case "--smoke-logs": options.Smoke = options.SmokeLogs = true; break;
                    case "--size":
                        options.Width = Number(Next(args, ref i), 360, 3000);
                        options.Height = Number(Next(args, ref i), 260, 2000);
                        break;
                    case "--scale": options.Scale = Number(Next(args, ref i), 1, 2); break;
                    default: throw new ArgumentException("未知参数：" + flag);
                }
            }
            if (options.Report != null && (options.Smoke || options.Snapshot != null))
                throw new ArgumentException("报告模式不能与界面诊断同时使用。");
            return options;
        }

        private static string Next(string[] args, ref int index)
        {
            if (++index >= args.Length || string.IsNullOrWhiteSpace(args[index]))
                throw new ArgumentException("参数缺少数值或路径。");
            return args[index];
        }

        private static double Number(string text, double min, double max)
        {
            if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out double value)
                || double.IsNaN(value) || value < min || value > max)
                throw new ArgumentException("窗口尺寸或缩放参数无效。");
            return value;
        }
    }
}
