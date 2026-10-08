use std::{ffi::OsStr, os::windows::ffi::OsStrExt};
use windows_sys::Win32::{
    Foundation::{ERROR_FILE_NOT_FOUND, ERROR_SUCCESS},
    System::Registry::{
        RegCloseKey, RegCreateKeyExW, RegDeleteValueW, RegGetValueW, RegSetValueExW,
        HKEY_CURRENT_USER, KEY_SET_VALUE, REG_OPTION_NON_VOLATILE, REG_SZ, RRF_RT_REG_SZ,
    },
};

fn wide(value: &OsStr) -> Vec<u16> {
    value.encode_wide().chain(Some(0)).collect()
}
fn command() -> Result<String, String> {
    let executable = std::env::current_exe().map_err(|error| error.to_string())?;
    Ok(format!("\"{}\" --background", executable.display()))
}
const RUN_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const VALUE: &str = "VeritasSyncNext";

fn value_name(identifier: &str) -> String {
    if identifier == "io.veritassync.next" { VALUE.into() }
    else { format!("{VALUE}-{identifier}") }
}

fn read_status(path: &str, value: &str, expected: &str) -> Result<bool, String> {
    let key = wide(OsStr::new(path));
    let name = wide(OsStr::new(value));
    let mut buffer = vec![0u16; 32768];
    let mut size = (buffer.len() * 2) as u32;
    let result = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER, key.as_ptr(), name.as_ptr(), RRF_RT_REG_SZ,
            std::ptr::null_mut(), buffer.as_mut_ptr().cast(), &mut size,
        )
    };
    if result == ERROR_FILE_NOT_FOUND {
        return Ok(false);
    }
    if result != ERROR_SUCCESS {
        return Err(format!("无法读取登录启动配置 ({result})"));
    }
    let length = buffer.iter().position(|&value| value == 0).unwrap_or(buffer.len());
    Ok(String::from_utf16_lossy(&buffer[..length]) == expected)
}
fn write_configuration(path: &str, value: &str, command: Option<&str>) -> Result<(), String> {
    let text = command.map(|text| wide(OsStr::new(text)));
    let path = wide(OsStr::new(path));
    let name = wide(OsStr::new(value));
    let mut key = std::ptr::null_mut();
    let result = unsafe {
        RegCreateKeyExW(
            HKEY_CURRENT_USER, path.as_ptr(), 0, std::ptr::null(),
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, std::ptr::null(), &mut key,
            std::ptr::null_mut(),
        )
    };
    if result != ERROR_SUCCESS {
        return Err(format!("无法打开登录启动配置 ({result})"));
    }
    let result = if let Some(text) = text {
        unsafe {
            RegSetValueExW(key, name.as_ptr(), 0, REG_SZ, text.as_ptr().cast(), (text.len() * 2) as u32)
        }
    } else {
        unsafe { RegDeleteValueW(key, name.as_ptr()) }
    };
    unsafe { RegCloseKey(key); }
    if result == ERROR_SUCCESS || (command.is_none() && result == ERROR_FILE_NOT_FOUND) {
        Ok(())
    } else {
        Err(format!("无法保存登录启动配置 ({result})"))
    }
}

#[tauri::command]
pub fn autostart_status(app: tauri::AppHandle) -> Result<bool, String> {
    read_status(RUN_KEY, &value_name(&app.config().identifier), &command()?)
}

#[tauri::command]
pub fn configure_autostart(app: tauri::AppHandle, enabled: bool) -> Result<(), String> {
    let text = if enabled { Some(command()?) } else { None };
    write_configuration(RUN_KEY, &value_name(&app.config().identifier), text.as_deref())
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows_sys::Win32::System::Registry::RegDeleteKeyW;

    #[test]
    fn registry_round_trip_preserves_unicode_and_disable_is_idempotent() {
        // Never touch the real Windows Run key in automated tests.
        let suffix = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let path = format!(r"Software\VeritasSyncNext\Tests\autostart-{}-{suffix}", std::process::id());
        struct Cleanup(String);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let path = wide(OsStr::new(&self.0));
                unsafe { RegDeleteKeyW(HKEY_CURRENT_USER, path.as_ptr()); }
            }
        }
        let _cleanup = Cleanup(path.clone());
        let expected = r#""C:\测试 space🙂\app.exe" --background"#;
        assert!(!read_status(&path, VALUE, expected).unwrap());
        write_configuration(&path, VALUE, Some(expected)).unwrap();
        assert!(read_status(&path, VALUE, expected).unwrap());
        assert!(!read_status(&path, VALUE, "another app").unwrap());
        write_configuration(&path, VALUE, None).unwrap();
        write_configuration(&path, VALUE, None).unwrap();
        assert!(!read_status(&path, VALUE, expected).unwrap());
    }

    #[test]
    fn test_packages_do_not_overwrite_production_startup() {
        assert_eq!(value_name("io.veritassync.next"), VALUE);
        assert_ne!(value_name("io.veritassync.next.audit"), VALUE);
    }
}
