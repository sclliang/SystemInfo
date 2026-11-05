use std::{ffi::OsString, path::PathBuf};

#[derive(Default)]
pub struct Options {
    pub report: Option<(PathBuf, bool)>,
    pub snapshot: Option<PathBuf>,
    pub smoke: bool,
    pub smoke_logs: bool,
    pub size: Option<(f64, f64)>,
    pub scale: Option<f64>,
}

pub fn parse() -> Result<Options, String> {
    let mut args = std::env::args_os().skip(1);
    let mut options = Options::default();
    let number = |arg: Option<OsString>, low: f64, high: f64| {
        arg.and_then(|a| a.to_str()?.parse::<f64>().ok())
            .filter(|n| (low..=high).contains(n))
            .ok_or_else(|| "无效的窗口尺寸或缩放参数".to_owned())
    };
    while let Some(arg) = args.next() {
        let flag = arg.to_str().ok_or("无效参数")?;
        match flag {
            "--report" | "--ami-report" | "--snapshot" => {
                let path = args
                    .next()
                    .filter(|p| !p.is_empty())
                    .ok_or("缺少输出路径")?;
                let path = std::path::absolute(path).map_err(|e| e.to_string())?;
                if flag == "--snapshot" {
                    options.snapshot = Some(path);
                } else if options
                    .report
                    .replace((path, flag == "--ami-report"))
                    .is_some()
                {
                    return Err("报告参数重复".into());
                }
            }
            "--size" => {
                options.size = Some((
                    number(args.next(), 360., 2000.)?,
                    number(args.next(), 240., 1600.)?,
                ))
            }
            "--scale" => options.scale = Some(number(args.next(), 1., 2.)?),
            "--smoke" => options.smoke = true,
            "--smoke-logs" => {
                options.smoke = true;
                options.smoke_logs = true;
            }
            _ => return Err(format!("未知参数 {flag}")),
        }
    }
    if options.report.is_some() && (options.smoke || options.snapshot.is_some()) {
        return Err("报告模式不能与界面诊断同时使用".into());
    }
    Ok(options)
}
