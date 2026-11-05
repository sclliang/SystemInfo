#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod capture;
mod native;
mod options;

use native::Native;
use serde_json::Value;
use std::{
    path::PathBuf,
    sync::{
        Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};
use tauri::{Manager, State, WebviewUrl, WebviewWindow, WebviewWindowBuilder};

struct Shared(Mutex<Option<Native>>);
impl Shared {
    fn access<T>(&self, call: impl FnOnce(&mut Native) -> Result<T, String>) -> Result<T, String> {
        let mut guard = self.0.lock().map_err(|_| "采集会话异常")?;
        call(guard.as_mut().ok_or("采集会话已结束")?)
    }
}
struct Diagnostic {
    path: Option<PathBuf>,
    smoke: bool,
    logs: bool,
    captured: AtomicBool,
}

#[tauri::command]
fn snapshot(
    since: u64,
    state: State<'_, Shared>,
    diagnostic: State<'_, Diagnostic>,
) -> Result<Option<Value>, String> {
    let mut result = state.access(|session| session.poll(since))?;
    if let Some(value) = result.as_mut() {
        value["collectOnReady"] = diagnostic.logs.into();
    }
    Ok(result)
}

#[tauri::command]
fn perform(
    action: String,
    window: WebviewWindow,
    state: State<'_, Shared>,
) -> Result<bool, String> {
    let hwnd = window.hwnd().map_err(|e| e.to_string())?.0;
    state.access(|session| session.action(&action, hwnd))
}

fn finished(value: &Value, logs: bool) -> bool {
    let hardware = value["computerPending"] == false && value["ssdPending"] == false;
    hardware
        && (!logs
            || matches!(
                value["log"]["state"].as_str(),
                Some("complete" | "partial" | "failed" | "cancelled")
            ))
}

#[tauri::command]
fn frame_ready(
    window: WebviewWindow,
    state: State<'_, Shared>,
    diagnostic: State<'_, Diagnostic>,
) -> Result<(), String> {
    let Some(path) = diagnostic.path.as_ref() else {
        return Ok(());
    };
    if diagnostic.captured.load(Ordering::Relaxed) {
        return Ok(());
    }
    let ready = state
        .access(|session| session.poll(0))?
        .is_some_and(|v| finished(&v, diagnostic.logs));
    if ready && !diagnostic.captured.swap(true, Ordering::Relaxed) {
        capture::capture(window, path.clone(), diagnostic.smoke)?;
    }
    Ok(())
}

// Exercise the same serialized payload consumed by the dashboard, without a window.
fn smoke_data(mut session: Native, logs: bool) -> Result<i32, String> {
    if logs && !session.action("collect_logs", std::ptr::null_mut())? {
        return Ok(1);
    }
    let deadline = Instant::now() + Duration::from_secs(if logs { 480 } else { 120 });
    let mut since = 0;
    while Instant::now() < deadline {
        if let Some(v) = session.poll(since)? {
            since = v["revision"].as_u64().ok_or("缺少采集版本")?;
            if finished(&v, logs) {
                let ok = v["error"] == ""
                    && v["rows"].as_array().is_some_and(|rows| {
                        rows.len() == 17 && rows.iter().any(|row| row["label"] == "电池信息")
                    })
                    && v["battery"]["packs"].is_array()
                    && (!logs || v["log"]["hasReport"] == true);
                return Ok(if ok { 0 } else { 1 });
            }
        }
        std::thread::sleep(Duration::from_millis(80));
    }
    Ok(3)
}

fn desktop(options: options::Options, session: Native) -> Result<i32, String> {
    let size = options.size;
    let scale = options.scale;
    let timeout = if options.smoke_logs { 480 } else { 120 };
    let watchdog = options.smoke;
    let app = tauri::Builder::default()
        .manage(Shared(Mutex::new(Some(session))))
        .manage(Diagnostic {
            path: options.snapshot,
            smoke: options.smoke,
            logs: options.smoke_logs,
            captured: AtomicBool::new(false),
        })
        .invoke_handler(tauri::generate_handler![snapshot, perform, frame_ready])
        .setup(move |app| {
            let window =
                WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
                    .title("SystemInfo · 电脑信息")
                    .decorations(false)
                    .resizable(true)
                    .visible(false)
                    .min_inner_size(360., 260.)
                    .theme(Some(tauri::Theme::Light))
                    .disable_drag_drop_handler()
                    .on_navigation(|url| {
                        matches!(
                            (url.scheme(), url.host_str()),
                            ("http" | "https", Some("tauri.localhost"))
                                | ("tauri", Some("localhost"))
                        )
                    })
                    .on_new_window(|_, _| tauri::webview::NewWindowResponse::Deny)
                    .build()?;
            if !native::position(window.hwnd()?.0, size) {
                return Err("无法设置启动窗口位置".into());
            }
            if let Some(zoom) = scale {
                window.set_zoom(zoom)?;
            }
            window.show()?;
            if watchdog {
                let handle = app.handle().clone();
                std::thread::spawn(move || {
                    std::thread::sleep(Duration::from_secs(timeout));
                    handle.exit(3);
                });
            }
            Ok(())
        })
        .build(tauri::generate_context!())
        .map_err(|e| e.to_string())?;
    Ok(app.run_return(|handle, event| {
        if matches!(event, tauri::RunEvent::Exit) {
            let state = handle.state::<Shared>();
            if let Ok(mut guard) = state.0.lock() {
                guard.take();
            }
        }
    }))
}

fn run() -> Result<i32, String> {
    let Ok(options) = options::parse() else {
        return Ok(2);
    };
    if let Some((path, ami)) = &options.report {
        return Ok(native::write_report(path.as_os_str(), *ami));
    }
    let session = Native::new()?;
    if options.smoke && options.snapshot.is_none() {
        return smoke_data(session, options.smoke_logs);
    }
    desktop(options, session)
}

fn main() {
    match run() {
        Ok(code) => std::process::exit(code),
        Err(error) => {
            use windows::{
                Win32::UI::WindowsAndMessaging::{MB_ICONERROR, MB_OK, MessageBoxW},
                core::HSTRING,
            };
            unsafe {
                MessageBoxW(
                    None,
                    &HSTRING::from(format!("程序启动失败\n{error}")),
                    &HSTRING::from("SystemInfo"),
                    MB_ICONERROR | MB_OK,
                );
            }
            std::process::exit(1);
        }
    }
}
