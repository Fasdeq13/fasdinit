mod config;
mod executor;
mod manager;

use std::io::{Read, Write};
use std::os::unix::io::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use signal_hook::consts::{SIGCHLD, SIGINT, SIGTERM};

use manager::Manager;

#[derive(Serialize, Deserialize, Debug)]
enum Request {
    Start(String),
    Stop(String),
    Restart(String),
    Enable(String),
    Disable(String),
    Status(Option<String>),
    ListServices,
}

#[derive(Serialize, Deserialize, Debug)]
enum Response {
    Ok(String),
    Err(String),
}

static GOT_SIGCHLD: AtomicBool = AtomicBool::new(false);
static SHUTDOWN_SIGNAL: AtomicI32 = AtomicI32::new(0);

fn main() {
    let is_pid1 = unsafe { libc::getpid() } == 1;

    if is_pid1 {
        early_boot_setup();
    } else {
        eprintln!("fasdinit: warning — not running as PID 1, continuing in test mode");
    }

    let mut manager = match Manager::load() {
        Ok(m) => m,
        Err(e) => {
            eprintln!("fasdinit: failed to load service configuration: {}", e);
            Manager {
                services: Default::default(),
                rc_conf: Default::default(),
            }
        }
    };

    register_signal_handlers();

    manager.start_all_enabled();

    let listener = setup_control_socket();

    main_loop(&mut manager, listener.as_ref());

    if is_pid1 {
        finalize_shutdown(&manager);
    }
}

fn early_boot_setup() {
    mount_if_missing("proc", "/proc", "proc");
    mount_if_missing("sysfs", "/sys", "sysfs");
    mount_if_missing("devtmpfs", "/dev", "devtmpfs");
    std::fs::create_dir_all(config::LOG_DIR).ok();
}

fn mount_if_missing(fstype: &str, target: &str, source: &str) {
    std::fs::create_dir_all(target).ok();
    let fstype_c = std::ffi::CString::new(fstype).unwrap();
    let target_c = std::ffi::CString::new(target).unwrap();
    let source_c = std::ffi::CString::new(source).unwrap();
    unsafe {
        libc::mount(
            source_c.as_ptr(),
            target_c.as_ptr(),
            fstype_c.as_ptr(),
            0,
            std::ptr::null(),
        );
    }
}

fn register_signal_handlers() {
    unsafe {
        signal_hook::low_level::register(SIGCHLD, || GOT_SIGCHLD.store(true, Ordering::SeqCst))
            .ok();
        signal_hook::low_level::register(SIGTERM, || {
            SHUTDOWN_SIGNAL.store(SIGTERM, Ordering::SeqCst)
        })
        .ok();
        signal_hook::low_level::register(SIGINT, || {
            SHUTDOWN_SIGNAL.store(SIGINT, Ordering::SeqCst)
        })
        .ok();
    }
}

fn setup_control_socket() -> Option<UnixListener> {
    std::fs::remove_file(config::SOCKET_PATH).ok();
    if let Some(parent) = std::path::Path::new(config::SOCKET_PATH).parent() {
        std::fs::create_dir_all(parent).ok();
    }
    match UnixListener::bind(config::SOCKET_PATH) {
        Ok(l) => {
            l.set_nonblocking(true).ok();
            Some(l)
        }
        Err(e) => {
            eprintln!("fasdinit: failed to create control socket: {}", e);
            None
        }
    }
}

fn main_loop(manager: &mut Manager, listener: Option<&UnixListener>) {
    loop {
        let sig = SHUTDOWN_SIGNAL.load(Ordering::SeqCst);
        if sig != 0 {
            eprintln!("fasdinit: shutdown signal received, stopping services");
            manager.stop_all();
            wait_for_drain(manager, Duration::from_secs(10));
            break;
        }

        manager.handle_reaped();
        GOT_SIGCHLD.store(false, Ordering::SeqCst);

        if let Some(listener) = listener {
            let mut pfd = libc::pollfd {
                fd: listener.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            };

            let ret = unsafe { libc::poll(&mut pfd, 1, 500) };

            if ret > 0 && (pfd.revents & libc::POLLIN) != 0 {
                if let Ok((stream, _)) = listener.accept() {
                    handle_client(manager, stream);
                }
            }
        } else {
            std::thread::sleep(Duration::from_millis(500));
        }
    }
}

fn wait_for_drain(manager: &mut Manager, timeout: Duration) {
    let start = std::time::Instant::now();
    loop {
        manager.handle_reaped();
        let any_running = manager
            .services
            .values()
            .any(|rt| rt.state == manager::ServiceState::Stopping);
        if !any_running || start.elapsed() > timeout {
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn finalize_shutdown(manager: &Manager) {
    let _ = manager;
    executor::sync_disks();
    let sig = SHUTDOWN_SIGNAL.load(Ordering::SeqCst);
    if sig == SIGTERM {
        eprintln!("fasdinit: shutting down (SIGTERM)");
    } else {
        eprintln!("fasdinit: shutting down (SIGINT)");
    }
}

fn handle_client(manager: &mut Manager, mut stream: UnixStream) {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    stream.set_nonblocking(false).ok();
    loop {
        match stream.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if n < chunk.len() {
                    break;
                }
            }
            Err(_) => break,
        }
    }

    let request: Result<Request, _> = serde_json::from_slice(&buf);
    let response = match request {
        Ok(req) => process_request(manager, req),
        Err(e) => Response::Err(format!("invalid request: {}", e)),
    };

    if let Ok(data) = serde_json::to_vec(&response) {
        stream.write_all(&data).ok();
    }
}

fn process_request(manager: &mut Manager, req: Request) -> Response {
    match req {
        Request::Start(name) => match manager.start_service(&name) {
            Ok(()) => Response::Ok(format!("service {} started", name)),
            Err(e) => Response::Err(e),
        },
        Request::Stop(name) => match manager.stop_service(&name) {
            Ok(()) => Response::Ok(format!("service {} stopping", name)),
            Err(e) => Response::Err(e),
        },
        Request::Restart(name) => {
            manager.stop_service(&name).ok();
            std::thread::sleep(Duration::from_millis(300));
            manager.handle_reaped();
            match manager.start_service(&name) {
                Ok(()) => Response::Ok(format!("service {} restarted", name)),
                Err(e) => Response::Err(e),
            }
        }
        Request::Enable(name) => {
            let key = format!("{}_enable", name);
            match config::set_rc_conf_value(config::RC_CONF_PATH, &key, "YES") {
                Ok(()) => {
                    manager.rc_conf.insert(key, "YES".to_string());
                    Response::Ok(format!("service {} enabled", name))
                }
                Err(e) => Response::Err(format!("failed to update rc.conf: {}", e)),
            }
        }
        Request::Disable(name) => {
            let key = format!("{}_enable", name);
            match config::set_rc_conf_value(config::RC_CONF_PATH, &key, "NO") {
                Ok(()) => {
                    manager.rc_conf.insert(key, "NO".to_string());
                    Response::Ok(format!("service {} disabled", name))
                }
                Err(e) => Response::Err(format!("failed to update rc.conf: {}", e)),
            }
        }
        Request::Status(Some(name)) => Response::Ok(manager.status_line(&name)),
        Request::Status(None) | Request::ListServices => {
            let mut names: Vec<&String> = manager.services.keys().collect();
            names.sort();
            let lines: Vec<String> = names
                .into_iter()
                .map(|n| manager.status_line(n))
                .collect();
            Response::Ok(lines.join("\n"))
        }
    }
}
