use std::{ffi::OsStr, os::windows::ffi::OsStrExt, thread, time::Duration};

type Handle = isize;
const INVALID_HANDLE_VALUE: Handle = -1isize;
const GENERIC_READ: u32 = 0x8000_0000;
const GENERIC_WRITE: u32 = 0x4000_0000;
const OPEN_EXISTING: u32 = 3;
const ERROR_FILE_NOT_FOUND: u32 = 2;
const ERROR_PIPE_BUSY: u32 = 231;
const ERROR_PIPE_NOT_CONNECTED: u32 = 233;
const MAX_CONNECT_ATTEMPTS: usize = 24;

fn is_transient_connect_error(error: u32) -> bool {
    matches!(
        error,
        ERROR_FILE_NOT_FOUND | ERROR_PIPE_BUSY | ERROR_PIPE_NOT_CONNECTED
    )
}

#[link(name = "kernel32")]
extern "system" {
    fn CreateFileW(
        name: *const u16,
        access: u32,
        share: u32,
        security: *const (),
        disposition: u32,
        flags: u32,
        template: Handle,
    ) -> Handle;
    fn WriteFile(
        handle: Handle,
        buffer: *const u8,
        bytes: u32,
        written: *mut u32,
        overlapped: *mut (),
    ) -> i32;
    fn ReadFile(
        handle: Handle,
        buffer: *mut u8,
        bytes: u32,
        read: *mut u32,
        overlapped: *mut (),
    ) -> i32;
    fn CloseHandle(handle: Handle) -> i32;
    fn GetLastError() -> u32;
    fn WaitNamedPipeW(name: *const u16, timeout: u32) -> i32;
}

fn escape(value: &str) -> String {
    value.bytes().fold(String::new(), |mut output, byte| {
        if matches!(byte, b'%' | b'\t' | b'\r' | b'\n') {
            output.push_str(&format!("%{byte:02X}"));
        } else {
            output.push(byte as char);
        }
        output
    })
}

pub fn unescape(value: &str) -> Result<String, String> {
    let bytes = value.as_bytes();
    let mut output = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] != b'%' {
            output.push(bytes[index]);
            index += 1;
            continue;
        }
        if index + 2 >= bytes.len() {
            return Err("IPC escape is truncated".into());
        }
        let decode = |byte: u8| match byte {
            b'0'..=b'9' => Some(byte - b'0'),
            b'a'..=b'f' => Some(byte - b'a' + 10),
            b'A'..=b'F' => Some(byte - b'A' + 10),
            _ => None,
        };
        let high = decode(bytes[index + 1]).ok_or_else(|| "IPC escape is invalid".to_string())?;
        let low = decode(bytes[index + 2]).ok_or_else(|| "IPC escape is invalid".to_string())?;
        output.push((high << 4) | low);
        index += 3;
    }
    String::from_utf8(output).map_err(|_| "IPC field is not UTF-8".into())
}

pub fn request(pipe: &str, command: &str, args: &[String]) -> Result<String, String> {
    let mut message = format!("VSYNC_IPC/1\t{}", escape(command));
    for argument in args {
        message.push('\t');
        message.push_str(&escape(argument));
    }
    let wide: Vec<u16> = OsStr::new(pipe).encode_wide().chain(Some(0)).collect();
    // The Engine processes one message per pipe instance. Between accepting
    // clients it briefly destroys and recreates that instance, so concurrent
    // dashboard reads can observe FILE_NOT_FOUND as well as PIPE_BUSY. Both
    // mean "the healthy Engine is rotating its IPC endpoint", not unavailable.
    let mut handle = INVALID_HANDLE_VALUE;
    for attempt in 0..MAX_CONNECT_ATTEMPTS {
        handle = unsafe {
            CreateFileW(
                wide.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                std::ptr::null(),
                OPEN_EXISTING,
                0,
                0,
            )
        };
        if handle != INVALID_HANDLE_VALUE {
            break;
        }
        let error = unsafe { GetLastError() };
        if !is_transient_connect_error(error) || attempt + 1 == MAX_CONNECT_ATTEMPTS {
            break;
        }
        if error == ERROR_PIPE_BUSY {
            let _ = unsafe { WaitNamedPipeW(wide.as_ptr(), 125) };
        } else {
            thread::sleep(Duration::from_millis(25));
        }
    }
    if handle == INVALID_HANDLE_VALUE {
        return Err("IPC engine is unavailable".to_string());
    }
    let result = (|| {
        let mut written = 0;
        if unsafe {
            WriteFile(
                handle,
                message.as_ptr(),
                message.len() as u32,
                &mut written,
                std::ptr::null_mut(),
            )
        } == 0
            || written != message.len() as u32
        {
            return Err("cannot send IPC request".to_string());
        }
        let mut buffer = vec![0_u8; 64 * 1024];
        let mut read = 0;
        if unsafe {
            ReadFile(
                handle,
                buffer.as_mut_ptr(),
                buffer.len() as u32,
                &mut read,
                std::ptr::null_mut(),
            )
        } == 0
        {
            return Err("cannot read IPC response".to_string());
        }
        String::from_utf8(buffer[..read as usize].to_vec())
            .map_err(|_| "IPC response is not UTF-8".to_string())
    })();
    unsafe {
        CloseHandle(handle);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn retries_when_the_single_instance_server_rotates() {
        assert!(is_transient_connect_error(ERROR_FILE_NOT_FOUND));
        assert!(is_transient_connect_error(ERROR_PIPE_BUSY));
        assert!(is_transient_connect_error(ERROR_PIPE_NOT_CONNECTED));
        assert!(!is_transient_connect_error(5)); // ERROR_ACCESS_DENIED
    }

    #[test]
    fn decodes_engine_fields_without_accepting_malformed_escapes() {
        assert_eq!(unescape("line%0Avalue%09x").unwrap(), "line\nvalue\tx");
        assert!(unescape("bad%0").is_err());
        assert!(unescape("bad%XZ").is_err());
    }
}
