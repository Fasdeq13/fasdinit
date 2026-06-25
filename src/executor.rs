use std::ffi::CString;
use std::fs::OpenOptions;
use std::io;
use std::os::unix::io::AsRawFd;
use std::path::Path;

use libc::{c_char, pid_t};

use crate::config::ServiceUnit;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExitStatus {
    Exited(i32),
    Signaled(i32),
}

pub fn split_args(input: &str) -> Vec<String> {
    let mut args = Vec::new();
    let mut current = String::new();
    let mut in_single = false;
    let mut in_double = false;

    for ch in input.chars() {
        match ch {
            '\'' if !in_double => in_single = !in_single,
            '"' if !in_single => in_double = !in_double,
            c if c.is_whitespace() && !in_single && !in_double => {
                if !current.is_empty() {
                    args.push(std::mem::take(&mut current));
                }
            }
            c => current.push(c),
        }
    }
    if !current.is_empty() {
        args.push(current);
    }
    args
}

fn to_cstring(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new("").unwrap())
}

pub fn spawn_service(unit: &ServiceUnit) -> io::Result<pid_t> {
    let mut argv: Vec<String> = vec![unit.command.clone()];
    argv.extend(split_args(&unit.command_args));

    let log_path = format!("{}/{}.log", crate::config::LOG_DIR, unit.name);
    std::fs::create_dir_all(crate::config::LOG_DIR).ok();
    let log_file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)?;
    let log_fd = log_file.as_raw_fd();

    let c_argv: Vec<CString> = argv.iter().map(|s| to_cstring(s)).collect();
    let mut c_argv_ptrs: Vec<*const c_char> = c_argv.iter().map(|s| s.as_ptr()).collect();
    c_argv_ptrs.push(std::ptr::null());

    let pid = unsafe { libc::fork() };
    if pid < 0 {
        return Err(io::Error::last_os_error());
    }

    if pid == 0 {
        unsafe {
            libc::setsid();

            libc::dup2(log_fd, libc::STDOUT_FILENO);
            libc::dup2(log_fd, libc::STDERR_FILENO);

            if let Some(user) = &unit.user {
                drop_privileges(user);
            }

            libc::execvp(c_argv_ptrs[0], c_argv_ptrs.as_ptr());
            libc::_exit(127);
        }
    }

    Ok(pid)
}

fn drop_privileges(username: &str) {
    let c_user = to_cstring(username);
    unsafe {
        let pw = libc::getpwnam(c_user.as_ptr());
        if pw.is_null() {
            return;
        }
        let uid = (*pw).pw_uid;
        let gid = (*pw).pw_gid;
        libc::setgid(gid);
        libc::setuid(uid);
    }
}

pub fn send_signal(pid: pid_t, sig: i32) -> io::Result<()> {
    let ret = unsafe { libc::kill(pid, sig) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

pub fn reap_children() -> Vec<(pid_t, ExitStatus)> {
    let mut results = Vec::new();
    loop {
        let mut status: i32 = 0;
        let pid = unsafe { libc::waitpid(-1, &mut status, libc::WNOHANG) };
        if pid <= 0 {
            break;
        }
        let exit_status = if libc::WIFEXITED(status) {
            ExitStatus::Exited(libc::WEXITSTATUS(status))
        } else if libc::WIFSIGNALED(status) {
            ExitStatus::Signaled(libc::WTERMSIG(status))
        } else {
            continue;
        };
        results.push((pid, exit_status));
    }
    results
}

pub fn is_alive(pid: pid_t) -> bool {
    unsafe { libc::kill(pid, 0) == 0 }
}

pub fn sync_disks() {
    unsafe { libc::sync() };
}

pub fn reboot(cmd: i32) -> io::Result<()> {
    let ret = unsafe { libc::reboot(cmd) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

pub fn command_exists(path: &str) -> bool {
    Path::new(path).exists()
}
