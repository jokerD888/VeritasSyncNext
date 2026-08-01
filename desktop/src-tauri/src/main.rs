#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod ipc;

use std::{fs, path::PathBuf, process::Command, thread, time::Duration};
use tauri::{menu::{Menu, MenuItem}, tray::TrayIconBuilder, AppHandle, Manager};

struct EngineRuntime { database: PathBuf, pipe: String, executable: PathBuf }

fn runtime(app: &AppHandle) -> Result<EngineRuntime, String> {
    let data = app.path().app_local_data_dir().map_err(|error| error.to_string())?;
    fs::create_dir_all(&data).map_err(|error| error.to_string())?;
    let resource = app.path().resource_dir().map_err(|error| error.to_string())?;
    let packaged = resource.join("engine").join("veritassync-engine.exe");
    let development = resource.join("veritassync-engine.exe");
    let executable = if packaged.exists() { packaged } else { development };
    if !executable.exists() { return Err("找不到 veritassync-engine sidecar；请先运行 stage-desktop-engine.ps1".into()); }
    let user: String = std::env::var("USERNAME").unwrap_or_else(|_| "local".into()).chars()
        .map(|character| if character.is_ascii_alphanumeric() { character } else { '_' }).collect();
    Ok(EngineRuntime { database: data.join("state.db"), pipe: format!(r"\\.\pipe\veritassync-next-{user}"), executable })
}

fn ensure(app: &AppHandle) -> Result<EngineRuntime, String> {
    let runtime = runtime(app)?;
    if ipc::request(&runtime.pipe, "ping", &[]).is_ok() { return Ok(runtime); }
    let mut command = Command::new(&runtime.executable);
    command.args(["--ipc-serve", "--db"]).arg(&runtime.database).arg("--pipe").arg(&runtime.pipe);
    #[cfg(windows)] { use std::os::windows::process::CommandExt; command.creation_flags(0x0000_0008 | 0x0000_0200); }
    command.spawn().map_err(|error| format!("无法启动 engine: {error}"))?;
    for _ in 0..20 { thread::sleep(Duration::from_millis(150)); if ipc::request(&runtime.pipe, "ping", &[]).is_ok() { return Ok(runtime); } }
    Err("engine 未能在 3 秒内启动 IPC server".into())
}

#[tauri::command]
fn ensure_engine(app: AppHandle) -> Result<(), String> { ensure(&app).map(|_| ()) }

#[tauri::command]
fn ipc_request(app: AppHandle, command: String, args: Vec<String>) -> Result<String, String> {
    let runtime = ensure(&app)?;
    match ipc::request(&runtime.pipe, &command, &args) {
        Ok(response) if response.starts_with("ERR\t") => Err(response[4..].trim().to_string()),
        Ok(response) => Ok(response),
        Err(_) => { let restarted = ensure(&app)?; ipc::request(&restarted.pipe, &command, &args) }
    }
}

#[tauri::command]
fn select_folder() -> Option<String> { rfd::FileDialog::new().pick_folder().map(|path| path.to_string_lossy().to_string()) }

fn main() {
    tauri::Builder::default().plugin(tauri_plugin_updater::Builder::new().build()).setup(|app| {
        let show = MenuItem::with_id(app, "show", "显示 VeritasSync", true, None::<&str>)?;
        let quit = MenuItem::with_id(app, "quit", "退出桌面壳", true, None::<&str>)?;
        let menu = Menu::with_items(app, &[&show, &quit])?;
        TrayIconBuilder::with_id("veritassync-tray").tooltip("VeritasSync Next").menu(&menu)
            .on_menu_event(|app, event| match event.id.as_ref() {
                "show" => if let Some(window) = app.get_webview_window("main") { let _ = window.show(); let _ = window.set_focus(); },
                "quit" => app.exit(0), _ => {}
            }).build(app)?;
        Ok(())
    }).invoke_handler(tauri::generate_handler![ensure_engine, ipc_request, select_folder])
      .run(tauri::generate_context!()).expect("error while running VeritasSync desktop");
}
