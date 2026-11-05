use std::{
    env,
    path::{Path, PathBuf},
    process::Command,
};

fn run(command: &mut Command) {
    let status = command.status().expect("cannot launch CMake");
    assert!(status.success(), "native collector build failed: {status}");
}

fn main() {
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest_dir.parent().unwrap();
    let version = format!("{}.0", env!("CARGO_PKG_VERSION"));
    for path in [
        "../resources",
        "../src",
        "../ui",
        "../CMakeLists.txt",
        "../third_party",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
    println!("cargo:rerun-if-env-changed=SYSTEMINFO_NATIVE_LIB_DIR");
    let libs = env::var_os("SYSTEMINFO_NATIVE_LIB_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            let build = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("native");
            run(Command::new("cmake")
                .arg("-S")
                .arg(root)
                .arg("-B")
                .arg(&build)
                .args([
                    "-A",
                    "x64",
                    "-DBUILD_TESTING=OFF",
                    "-DSYSTEMINFO_BUILD_CLASSIC=OFF",
                ]));
            run(Command::new("cmake").arg("--build").arg(&build).args([
                "--config",
                "Release",
                "--target",
                "SystemInfoBridge",
                "--parallel",
            ]));
            build.join("Release")
        });
    println!("cargo:rustc-link-search=native={}", libs.display());
    for lib in [
        "SystemInfoBridge",
        "AmiFirmware",
        "HardwareInfo",
        "SsdInfo",
        "AtaSmartCore",
        "EventLogCollectorCore",
    ] {
        println!(
            "cargo:rerun-if-changed={}",
            libs.join(format!("{lib}.lib")).display()
        );
        println!("cargo:rustc-link-lib=static={lib}");
    }
    for lib in [
        "advapi32", "ole32", "oleaut32", "wbemuuid", "setupapi", "cfgmgr32", "wevtapi", "powrprof",
        "shell32", "shlwapi", "user32", "comsuppw", "wintrust", "crypt32", "uuid",
    ] {
        println!("cargo:rustc-link-lib={lib}");
    }
    let manifest = include_str!("../resources/app.manifest.in")
        .replace("@PROJECT_VERSION@", &version)
        .replace("@SYSTEMINFO_EXECUTION_LEVEL@", "requireAdministrator");
    let mut windows = tauri_build::WindowsAttributes::new()
        .app_manifest(manifest)
        .static_vc_runtime(true)
        .window_icon_path(root.join("resources/six.ico"));
    for (id, relative) in [
        (103, "resources/eventlog/report.html"),
        (201, "resources/ami/AMIDEWINx64.EXE"),
        (202, "resources/ami/amifldrv64.sys"),
        (203, "resources/ami/amigendrv64.sys"),
        (204, "resources/ami/amifldrv32.sys"),
    ] {
        let path = root
            .join(Path::new(relative))
            .display()
            .to_string()
            .replace('\\', "/");
        windows = windows.append_rc_content(format!("{id} RCDATA \"{path}\"\n"));
    }
    let commands =
        tauri_build::AppManifest::new().commands(&["snapshot", "perform", "frame_ready"]);
    tauri_build::try_build(
        tauri_build::Attributes::new()
            .windows_attributes(windows)
            .app_manifest(commands),
    )
    .expect("Tauri assets, permissions and Windows resources");
    println!("cargo:rustc-link-arg=/MANIFEST:NO");
}
