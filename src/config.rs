use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

pub const RC_CONF_PATH: &str = "/etc/fasdinit/rc.conf";
pub const SERVICES_DIR: &str = "/etc/fasdinit/services";
pub const SOCKET_PATH: &str = "/run/fasdinit.sock";
pub const LOG_DIR: &str = "/var/log/fasdinit";

#[derive(Debug, Clone, Default)]
pub struct ServiceUnit {
    pub name: String,
    pub provide: String,
    pub require: Vec<String>,
    pub keyword: Vec<String>,
    pub command: String,
    pub command_args: String,
    pub pidfile: Option<String>,
    pub user: Option<String>,
    pub respawn: bool,
    pub path: PathBuf,
}

pub fn load_services(dir: &str) -> io::Result<HashMap<String, ServiceUnit>> {
    let mut units = HashMap::new();
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(units),
        Err(e) => return Err(e),
    };

    for entry in entries {
        let entry = entry?;
        let path = entry.path();
        if path.is_file() {
            if let Some(unit) = parse_service_file(&path)? {
                units.insert(unit.name.clone(), unit);
            }
        }
    }
    Ok(units)
}

pub fn parse_service_file(path: &Path) -> io::Result<Option<ServiceUnit>> {
    let content = fs::read_to_string(path)?;
    let default_name = path
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();

    let mut unit = ServiceUnit {
        name: default_name,
        path: path.to_path_buf(),
        ..Default::default()
    };

    for line in content.lines() {
        let line = line.trim();

        if let Some(rest) = line.strip_prefix("# PROVIDE:") {
            unit.provide = rest.trim().to_string();
            if !unit.provide.is_empty() {
                unit.name = unit.provide.clone();
            }
            continue;
        }
        if let Some(rest) = line.strip_prefix("# REQUIRE:") {
            unit.require = rest.split_whitespace().map(|s| s.to_string()).collect();
            continue;
        }
        if let Some(rest) = line.strip_prefix("# KEYWORD:") {
            unit.keyword = rest.split_whitespace().map(|s| s.to_string()).collect();
            continue;
        }
        if line.starts_with('#') || line.is_empty() {
            continue;
        }

        if let Some((key, value)) = parse_kv_line(line) {
            match key.as_str() {
                "command" => unit.command = value,
                "command_args" => unit.command_args = value,
                "pidfile" => unit.pidfile = Some(value),
                "user" => unit.user = Some(value),
                "respawn" => unit.respawn = value == "YES" || value == "yes" || value == "1",
                _ => {}
            }
        }
    }

    if unit.command.is_empty() {
        return Ok(None);
    }
    Ok(Some(unit))
}

fn parse_kv_line(line: &str) -> Option<(String, String)> {
    let idx = line.find('=')?;
    let key = line[..idx].trim().to_string();
    let mut value = line[idx + 1..].trim().to_string();
    if value.starts_with('"') && value.ends_with('"') && value.len() >= 2 {
        value = value[1..value.len() - 1].to_string();
    }
    Some((key, value))
}

pub fn load_rc_conf(path: &str) -> io::Result<HashMap<String, String>> {
    let mut map = HashMap::new();
    let content = match fs::read_to_string(path) {
        Ok(c) => c,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(map),
        Err(e) => return Err(e),
    };

    for line in content.lines() {
        let line = line.trim();
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        if let Some((key, value)) = parse_kv_line(line) {
            map.insert(key, value);
        }
    }
    Ok(map)
}

pub fn set_rc_conf_value(path: &str, key: &str, value: &str) -> io::Result<()> {
    let existing = match fs::read_to_string(path) {
        Ok(c) => c,
        Err(e) if e.kind() == io::ErrorKind::NotFound => String::new(),
        Err(e) => return Err(e),
    };

    let mut found = false;
    let mut out_lines: Vec<String> = Vec::new();

    for line in existing.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with('#') || trimmed.is_empty() {
            out_lines.push(line.to_string());
            continue;
        }
        if let Some((k, _)) = parse_kv_line(trimmed) {
            if k == key {
                out_lines.push(format!("{}=\"{}\"", key, value));
                found = true;
                continue;
            }
        }
        out_lines.push(line.to_string());
    }

    if !found {
        out_lines.push(format!("{}=\"{}\"", key, value));
    }

    if let Some(parent) = Path::new(path).parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(path, out_lines.join("\n") + "\n")
}

pub fn is_enabled(rc_conf: &HashMap<String, String>, service: &str) -> bool {
    let key = format!("{}_enable", service);
    matches!(
        rc_conf.get(&key).map(|s| s.as_str()),
        Some("YES") | Some("yes") | Some("1")
    )
}
