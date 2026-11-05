use serde_json::Value;
use std::{
    ffi::{CStr, OsStr, c_char, c_void},
    os::windows::ffi::OsStrExt,
    ptr::NonNull,
};

unsafe extern "C" {
    fn si_new() -> *mut c_void;
    fn si_drop(session: *mut c_void);
    fn si_action(session: *mut c_void, op: u32, owner: *mut c_void) -> i32;
    fn si_poll(session: *mut c_void, since: u64) -> *const c_char;
    fn si_export(path: *const u16, ami: i32) -> i32;
    fn si_position(window: *mut c_void, width: f64, height: f64) -> i32;
}

pub struct Native(NonNull<c_void>);
// The owner mutex serializes all calls. Each C++ worker owns its COM apartment.
unsafe impl Send for Native {}

impl Native {
    pub fn new() -> Result<Self, String> {
        NonNull::new(unsafe { si_new() })
            .map(Self)
            .ok_or_else(|| "无法启动硬件采集".into())
    }
    pub fn action(&mut self, action: &str, hwnd: *mut c_void) -> Result<bool, String> {
        let op = match action {
            "refresh" => 1,
            "collect_logs" => 2,
            "cancel_logs" => 3,
            "copy_all" => 4,
            "open_report" => 5,
            "open_folder" => 6,
            _ => return Err("未知操作".into()),
        };
        match unsafe { si_action(self.0.as_ptr(), op, hwnd) } {
            1 => Ok(true),
            0 => Ok(false),
            _ => Err("操作失败，请重试".into()),
        }
    }
    pub fn poll(&mut self, since: u64) -> Result<Option<Value>, String> {
        let ptr = unsafe { si_poll(self.0.as_ptr(), since) };
        if ptr.is_null() {
            return Err("无法读取采集结果".into());
        }
        // Parse the borrowed buffer before releasing the mutex or calling native code again.
        let bytes = unsafe { CStr::from_ptr(ptr) }.to_bytes();
        if bytes.is_empty() {
            Ok(None)
        } else {
            serde_json::from_slice(bytes)
                .map(Some)
                .map_err(|e| e.to_string())
        }
    }
}

impl Drop for Native {
    fn drop(&mut self) {
        unsafe {
            si_drop(self.0.as_ptr());
        }
    }
}
pub fn write_report(path: &OsStr, ami: bool) -> i32 {
    let wide: Vec<u16> = path.encode_wide().chain(Some(0)).collect();
    unsafe { si_export(wide.as_ptr(), ami as i32) }
}
pub fn position(hwnd: *mut c_void, size: Option<(f64, f64)>) -> bool {
    let (w, h) = size.unwrap_or_default();
    unsafe { si_position(hwnd, w, h) != 0 }
}
