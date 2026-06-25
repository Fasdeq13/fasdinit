mod config;

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

use serde::{Deserialize, Serialize};

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

fn print_usage() {
    eprintln!(
        "Usage: fasdctl <command> [service]\n\n\
         Commands:\n  \
         start <service>      start the service now\n  \
         stop <service>       stop the service\n  \
         restart <service>    restart the service\n  \
         enable <service>     enable autostart (rc.conf)\n  \
         disable <service>    disable autostart (rc.conf)\n  \
         status [service]     show status of one service or all\n  \
         list                  list all known services"
    );
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        print_usage();
        std::process::exit(1);
    }

    let command = args[1].as_str();
    let service_arg = args.get(2).cloned();

    let request = match (command, service_arg.clone()) {
        ("start", Some(name)) => Request::Start(name),
        ("stop", Some(name)) => Request::Stop(name),
        ("restart", Some(name)) => Request::Restart(name),
        ("enable", Some(name)) => Request::Enable(name),
        ("disable", Some(name)) => Request::Disable(name),
        ("status", name) => Request::Status(name),
        ("list", _) => Request::ListServices,
        _ => {
            print_usage();
            std::process::exit(1);
        }
    };

    match send_request(&request) {
        Ok(Response::Ok(msg)) => {
            println!("{}", msg);
        }
        Ok(Response::Err(msg)) => {
            eprintln!("error: {}", msg);
            std::process::exit(1);
        }
        Err(e) => {
            eprintln!("fasdctl: failed to reach fasdinit: {}", e);
            eprintln!("(daemon not running or socket {} unavailable)", config::SOCKET_PATH);
            std::process::exit(1);
        }
    }
}

fn send_request(request: &Request) -> std::io::Result<Response> {
    let mut stream = UnixStream::connect(config::SOCKET_PATH)?;
    stream.set_read_timeout(Some(Duration::from_secs(5))).ok();
    stream.set_write_timeout(Some(Duration::from_secs(5))).ok();

    let payload = serde_json::to_vec(request)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
    stream.write_all(&payload)?;
    stream.shutdown(std::net::Shutdown::Write).ok();

    let mut buf = Vec::new();
    stream.read_to_end(&mut buf)?;

    serde_json::from_slice(&buf)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))
}
