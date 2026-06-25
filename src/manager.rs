use std::collections::{HashMap, HashSet, VecDeque};

use libc::pid_t;

use crate::config::{self, ServiceUnit};
use crate::executor;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServiceState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Failed,
}

#[derive(Debug)]
pub struct ServiceRuntime {
    pub unit: ServiceUnit,
    pub state: ServiceState,
    pub pid: Option<pid_t>,
    pub restart_count: u32,
}

pub struct Manager {
    pub services: HashMap<String, ServiceRuntime>,
    pub rc_conf: HashMap<String, String>,
}

impl Manager {
    pub fn load() -> std::io::Result<Manager> {
        let units = config::load_services(config::SERVICES_DIR)?;
        let rc_conf = config::load_rc_conf(config::RC_CONF_PATH)?;

        let services = units
            .into_iter()
            .map(|(name, unit)| {
                (
                    name,
                    ServiceRuntime {
                        unit,
                        state: ServiceState::Stopped,
                        pid: None,
                        restart_count: 0,
                    },
                )
            })
            .collect();

        Ok(Manager { services, rc_conf })
    }

    pub fn is_enabled(&self, name: &str) -> bool {
        config::is_enabled(&self.rc_conf, name)
    }

    pub fn boot_order(&self) -> Vec<String> {
        let enabled: Vec<String> = self
            .services
            .keys()
            .filter(|name| self.is_enabled(name))
            .cloned()
            .collect();
        self.topo_sort(&enabled)
    }

    fn topo_sort(&self, names: &[String]) -> Vec<String> {
        let name_set: HashSet<&str> = names.iter().map(|s| s.as_str()).collect();
        let mut in_degree: HashMap<&str, usize> = HashMap::new();
        let mut dependents: HashMap<&str, Vec<&str>> = HashMap::new();

        let mut provide_to_name: HashMap<&str, &str> = HashMap::new();
        for (name, rt) in &self.services {
            if !rt.unit.provide.is_empty() {
                provide_to_name.insert(rt.unit.provide.as_str(), name.as_str());
            }
            provide_to_name.entry(name.as_str()).or_insert(name.as_str());
        }

        for name in names {
            in_degree.entry(name.as_str()).or_insert(0);
        }

        for name in names {
            if let Some(rt) = self.services.get(name) {
                for dep_provide in &rt.unit.require {
                    if let Some(&dep_file_name) = provide_to_name.get(dep_provide.as_str()) {
                        if name_set.contains(dep_file_name) {
                            *in_degree.get_mut(name.as_str()).unwrap() += 1;
                            dependents.entry(dep_file_name).or_default().push(name.as_str());
                        }
                    }
                }
            }
        }

        let mut queue: VecDeque<&str> = in_degree
            .iter()
            .filter(|(_, &deg)| deg == 0)
            .map(|(&n, _)| n)
            .collect();
        let mut queue_vec: Vec<&str> = queue.drain(..).collect();
        queue_vec.sort();
        let mut queue: VecDeque<&str> = queue_vec.into();

        let mut order = Vec::new();
        while let Some(name) = queue.pop_front() {
            order.push(name.to_string());
            if let Some(deps) = dependents.get(name) {
                let mut next_ready = Vec::new();
                for &dep_name in deps {
                    let deg = in_degree.get_mut(dep_name).unwrap();
                    *deg -= 1;
                    if *deg == 0 {
                        next_ready.push(dep_name);
                    }
                }
                next_ready.sort();
                for n in next_ready {
                    queue.push_back(n);
                }
            }
        }

        if order.len() < names.len() {
            for name in names {
                if !order.contains(name) {
                    order.push(name.clone());
                }
            }
        }

        order
    }

    pub fn start_service(&mut self, name: &str) -> Result<(), String> {
        let rt = self
            .services
            .get_mut(name)
            .ok_or_else(|| format!("service not found: {}", name))?;

        if rt.state == ServiceState::Running {
            return Ok(());
        }

        if !executor::command_exists(&rt.unit.command) {
            rt.state = ServiceState::Failed;
            return Err(format!("command not found: {}", rt.unit.command));
        }

        rt.state = ServiceState::Starting;
        match executor::spawn_service(&rt.unit) {
            Ok(pid) => {
                rt.pid = Some(pid);
                rt.state = ServiceState::Running;
                Ok(())
            }
            Err(e) => {
                rt.state = ServiceState::Failed;
                Err(format!("failed to start {}: {}", name, e))
            }
        }
    }

    pub fn stop_service(&mut self, name: &str) -> Result<(), String> {
        let rt = self
            .services
            .get_mut(name)
            .ok_or_else(|| format!("service not found: {}", name))?;

        if let Some(pid) = rt.pid {
            rt.state = ServiceState::Stopping;
            executor::send_signal(pid, libc::SIGTERM).ok();
        } else {
            rt.state = ServiceState::Stopped;
        }
        Ok(())
    }

    pub fn start_all_enabled(&mut self) {
        let order = self.boot_order();
        for name in order {
            if let Err(e) = self.start_service(&name) {
                eprintln!("fasdinit: {}", e);
            }
        }
    }

    pub fn stop_all(&mut self) {
        let mut order = self.boot_order();
        order.reverse();
        for name in order {
            if let Some(rt) = self.services.get(&name) {
                if rt.state == ServiceState::Running {
                    self.stop_service(&name).ok();
                }
            }
        }
    }

    pub fn handle_reaped(&mut self) {
        for (pid, _status) in executor::reap_children() {
            if let Some((name, _)) = self
                .services
                .iter()
                .find(|(_, rt)| rt.pid == Some(pid))
                .map(|(n, rt)| (n.clone(), rt.state))
            {
                let should_respawn = {
                    let rt = self.services.get_mut(&name).unwrap();
                    rt.pid = None;
                    let was_stopping = rt.state == ServiceState::Stopping;
                    rt.state = ServiceState::Stopped;
                    rt.unit.respawn && !was_stopping
                };

                if should_respawn {
                    let rt = self.services.get_mut(&name).unwrap();
                    rt.restart_count += 1;
                    if rt.restart_count <= 10 {
                        self.start_service(&name).ok();
                    } else {
                        let rt = self.services.get_mut(&name).unwrap();
                        rt.state = ServiceState::Failed;
                        eprintln!(
                            "fasdinit: service {} exceeded restart limit, stopped",
                            name
                        );
                    }
                }
            }
        }
    }

    pub fn status_line(&self, name: &str) -> String {
        match self.services.get(name) {
            Some(rt) => format!(
                "{:<20} {:<10} {}",
                name,
                format!("{:?}", rt.state),
                rt.pid.map(|p| p.to_string()).unwrap_or_else(|| "-".to_string())
            ),
            None => format!("{:<20} unknown", name),
        }
    }
}
