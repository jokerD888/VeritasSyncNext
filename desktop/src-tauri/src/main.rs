#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod ai;
mod ipc;

use serde::Serialize;
use std::{
    fs,
    path::PathBuf,
    process::Command,
    sync::{Mutex, OnceLock},
    thread,
    time::Duration,
};
use tauri::{
    menu::{Menu, MenuItem},
    tray::TrayIconBuilder,
    AppHandle, Manager,
};
use tauri_plugin_updater::UpdaterExt;

struct EngineRuntime {
    database: PathBuf,
    pipe: String,
    executable: PathBuf,
}

// UI startup, IPC retries, and the watchdog can all ask for the sidecar at
// once. Serialize the complete probe/spawn/ready sequence so they cannot
// create competing servers for the same named pipe.
static ENGINE_START_LOCK: OnceLock<Mutex<()>> = OnceLock::new();

fn runtime(app: &AppHandle) -> Result<EngineRuntime, String> {
    let data = app
        .path()
        .app_local_data_dir()
        .map_err(|error| error.to_string())?;
    fs::create_dir_all(&data).map_err(|error| error.to_string())?;
    let resource = app
        .path()
        .resource_dir()
        .map_err(|error| error.to_string())?;
    let packaged = resource.join("engine").join("veritassync-engine.exe");
    let development_resource = resource
        .join("resources")
        .join("engine")
        .join("veritassync-engine.exe");
    let resource_sidecar = resource.join("veritassync-engine.exe");
    let executable_sidecar = std::env::current_exe()
        .map_err(|error| error.to_string())?
        .parent()
        .map(|directory| directory.join("veritassync-engine.exe"))
        .ok_or_else(|| "无法定位桌面壳所在目录".to_string())?;
    let executable = [
        packaged,
        development_resource,
        resource_sidecar,
        executable_sidecar,
    ]
    .into_iter()
    .find(|candidate| candidate.exists())
    .ok_or_else(|| {
        "找不到 veritassync-engine sidecar；请先运行 stage-desktop-engine.ps1".to_string()
    })?;
    let user: String = std::env::var("USERNAME")
        .unwrap_or_else(|_| "local".into())
        .chars()
        .map(|character| {
            if character.is_ascii_alphanumeric() {
                character
            } else {
                '_'
            }
        })
        .collect();
    Ok(EngineRuntime {
        database: data.join("state.db"),
        pipe: format!(r"\\.\pipe\veritassync-next-{user}"),
        executable,
    })
}

fn ensure(app: &AppHandle) -> Result<EngineRuntime, String> {
    let _startup = ENGINE_START_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .map_err(|_| "engine 启动锁异常".to_string())?;
    let runtime = runtime(app)?;
    if ipc::request(&runtime.pipe, "ping", &[]).is_ok() {
        return Ok(runtime);
    }
    let mut command = Command::new(&runtime.executable);
    command
        .args(["--ipc-serve", "--db"])
        .arg(&runtime.database)
        .arg("--pipe")
        .arg(&runtime.pipe);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0000_0008 | 0x0000_0200);
    }
    command
        .spawn()
        .map_err(|error| format!("无法启动 engine: {error}"))?;
    for _ in 0..20 {
        thread::sleep(Duration::from_millis(150));
        if ipc::request(&runtime.pipe, "ping", &[]).is_ok() {
            return Ok(runtime);
        }
    }
    Err("engine 未能在 3 秒内启动 IPC server".into())
}

#[tauri::command]
fn ensure_engine(app: AppHandle) -> Result<(), String> {
    ensure(&app).map(|_| ())
}

#[tauri::command]
fn engine_request(app: &AppHandle, command: &str, args: &[String]) -> Result<String, String> {
    let runtime = ensure(&app)?;
    let finish = |response: String| {
        if response.starts_with("ERR\t") {
            Err(response[4..].trim().to_string())
        } else {
            Ok(response)
        }
    };
    match ipc::request(&runtime.pipe, command, args) {
        Ok(response) => finish(response),
        Err(_) => {
            let restarted = ensure(&app)?;
            finish(ipc::request(&restarted.pipe, command, args)?)
        }
    }
}

#[tauri::command]
fn ipc_request(app: AppHandle, command: String, args: Vec<String>) -> Result<String, String> {
    engine_request(&app, &command, &args)
}

fn response_fields(reply: &str) -> Result<Vec<String>, String> {
    reply
        .lines()
        .next()
        .ok_or_else(|| "Engine 返回了空响应".to_string())?
        .split('\t')
        .map(ipc::unescape)
        .collect()
}

fn parse_ignore_context(reply: &str) -> Result<ai::IgnoreContext, String> {
    let fields = response_fields(reply)?;
    if fields.len() != 4 || fields[0] != "OK" {
        return Err("Engine 返回了无效的忽略上下文".into());
    }
    let mut context = ai::IgnoreContext {
        summary: fields[3].clone(),
        truncated: fields[2] == "1",
        ..Default::default()
    };
    for line in reply.lines().skip(1) {
        let mut fields = line.split('\t');
        let kind = fields.next().unwrap_or_default();
        let path = fields
            .next()
            .map(ipc::unescape)
            .transpose()?
            .unwrap_or_default();
        match kind {
            "MATCH" => context.relevant_paths.push(path),
            "COMPARE" => context.comparison_paths.push(path),
            "END" => break,
            _ => return Err("Engine 返回了未知的忽略上下文行".into()),
        }
    }
    Ok(context)
}

#[tauri::command]
fn ai_provider_status(app: AppHandle) -> Result<ai::AiProviderStatus, String> {
    ai::status(&app)
}

#[tauri::command]
fn configure_ai_provider(
    app: AppHandle,
    input: ai::AiProviderInput,
) -> Result<ai::AiProviderStatus, String> {
    ai::configure(&app, input)
}

#[tauri::command]
fn clear_ai_provider_key(app: AppHandle) -> Result<ai::AiProviderStatus, String> {
    ai::clear_key(&app)
}

#[tauri::command]
async fn generate_ignore_rules(
    app: AppHandle,
    task_id: String,
    description: String,
    context_mode: String,
) -> Result<ai::GeneratedIgnoreRules, String> {
    if !matches!(context_mode.as_str(), "private" | "precise") {
        return Err("忽略规则上下文模式无效".into());
    }
    let current = response_fields(&engine_request(&app, "ignore_get", &[task_id.clone()])?)?;
    if current.len() != 5 || current[0] != "OK" {
        return Err("Engine 返回了无效的忽略规则".into());
    }
    let reply = engine_request(
        &app,
        "ignore_context",
        &[task_id, description.clone(), context_mode],
    )?;
    let context = parse_ignore_context(&reply)?;
    ai::generate(&app, &description, &current[4], &context).await
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AvailableUpdate {
    version: String,
    notes: Option<String>,
}

#[tauri::command]
async fn check_for_update(app: AppHandle) -> Result<Option<AvailableUpdate>, String> {
    let updater = app.updater().map_err(|error| error.to_string())?;
    updater
        .check()
        .await
        .map(|update| {
            update.map(|update| AvailableUpdate {
                version: update.version,
                notes: update.body,
            })
        })
        .map_err(|error| error.to_string())
}

#[tauri::command]
async fn install_update(app: AppHandle) -> Result<String, String> {
    let updater = app.updater().map_err(|error| error.to_string())?;
    let update = updater
        .check()
        .await
        .map_err(|error| error.to_string())?
        .ok_or_else(|| "没有可安装的更新".to_string())?;
    let version = update.version.clone();
    update
        .download_and_install(|_, _| {}, || {})
        .await
        .map_err(|error| error.to_string())?;
    Ok(version)
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_log::Builder::default().build())
        .plugin(tauri_plugin_store::Builder::default().build())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_window_state::Builder::default().build())
        .plugin(tauri_plugin_single_instance::init(|app, _, _| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.unminimize();
                let _ = window.show();
                let _ = window.set_focus();
            }
        }))
        .plugin(tauri_plugin_updater::Builder::new().build())
        .setup(|app| {
            log::info!("VeritasSync desktop shell starting");
            let show = MenuItem::with_id(app, "show", "显示 VeritasSync", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "退出桌面壳", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show, &quit])?;
            TrayIconBuilder::with_id("veritassync-tray")
                .tooltip("VeritasSync Next")
                .menu(&menu)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "show" => {
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.show();
                            let _ = window.set_focus();
                        }
                    }
                    "quit" => app.exit(0),
                    _ => {}
                })
                .build(app)?;
            ensure(&app.handle()).map_err(std::io::Error::other)?;
            let watchdog = app.handle().clone();
            thread::spawn(move || loop {
                thread::sleep(Duration::from_secs(2));
                let _ = ensure(&watchdog);
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            ensure_engine,
            ipc_request,
            ai_provider_status,
            configure_ai_provider,
            clear_ai_provider_key,
            generate_ignore_rules,
            check_for_update,
            install_update
        ])
        .run(tauri::generate_context!())
        .expect("error while running VeritasSync desktop");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_ignore_context_rows_and_escaped_paths() {
        let context = parse_ignore_context(
            "OK\t12\t1\tbuild%09summary\nMATCH\tlogs%2Fapp.log\nCOMPARE\tsrc%2Fmain.cpp\nEND\n",
        )
        .unwrap();
        assert_eq!(context.summary, "build\tsummary");
        assert_eq!(context.relevant_paths, ["logs/app.log"]);
        assert_eq!(context.comparison_paths, ["src/main.cpp"]);
        assert!(context.truncated);
    }
}
