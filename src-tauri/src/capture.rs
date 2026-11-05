//! Opt-in developer capture. Only a path supplied on the native command line may be written.
use std::{os::windows::ffi::OsStrExt, path::PathBuf};
use tauri::{Manager, WebviewWindow};
use webview2_com::{
    CapturePreviewCompletedHandler,
    Microsoft::Web::WebView2::Win32::COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
};
use windows::{
    Win32::{
        System::Com::{STGC_DEFAULT, STGM_CREATE, STGM_SHARE_EXCLUSIVE, STGM_WRITE},
        UI::Shell::SHCreateStreamOnFileEx,
    },
    core::PCWSTR,
};

pub fn capture(window: WebviewWindow, path: PathBuf, exit: bool) -> Result<(), String> {
    let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    let handle = window.app_handle().clone();
    window
        .with_webview(move |view| {
            let result: windows::core::Result<()> = (|| unsafe {
                let stream = SHCreateStreamOnFileEx(
                    PCWSTR(wide.as_ptr()),
                    (STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE).0,
                    0x80,
                    true,
                    None,
                )?;
                let output = stream.clone();
                let done = handle.clone();
                let callback = CapturePreviewCompletedHandler::create(Box::new(move |status| {
                    let saved = status.and_then(|_| output.Commit(STGC_DEFAULT));
                    drop(output);
                    if exit {
                        done.exit(if saved.is_ok() { 0 } else { 3 });
                    }
                    saved
                }));
                view.controller().CoreWebView2()?.CapturePreview(
                    COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                    &stream,
                    &callback,
                )
            })();
            if result.is_err() && exit {
                handle.exit(3);
            }
        })
        .map_err(|e| e.to_string())
}
