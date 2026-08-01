use std::{
    ffi::OsStr,
    os::windows::ffi::OsStrExt,
    thread,
    time::Duration,
};

type Handle = isize;
const INVALID_HANDLE_VALUE: Handle = -1isize;
const GENERIC_READ: u32 = 0x8000_0000;
const GENERIC_WRITE: u32 = 0x4000_0000;
const OPEN_EXISTING: u32 = 3;
const ERROR_PIPE_BUSY: u32 = 231;

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

pub fn request(pipe: &str, command: &str, args: &[String]) -> Result<String, String> {
    let mut message = format!("VSYNC_IPC/1\t{}", escape(command));
    for argument in args {
        message.push('\t');
        message.push_str(&escape(argument));
    }
    let wide: Vec<u16> = OsStr::new(pipe).encode_wide().chain(Some(0)).collect();
    // The Engine processes one message per pipe instance. Several UI reads may
    // arrive together, so a busy pipe means "healthy but serving another
    // request", not "engine unavailable". Wait briefly before declaring it
    // unavailable; this also prevents the shell from spawning duplicate
    // sidecars during normal dashboard refreshes.
    let mut handle = INVALID_HANDLE_VALUE;
    for _ in 0..12 {
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
        if unsafe { GetLastError() } != ERROR_PIPE_BUSY {
            break;
        }
        let _ = unsafe { WaitNamedPipeW(wide.as_ptr(), 200) };
        thread::sleep(Duration::from_millis(10));
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
