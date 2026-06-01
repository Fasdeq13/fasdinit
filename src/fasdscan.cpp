#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <filesystem>
#include <csignal>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

namespace fs = std::filesystem;

class FasdScan {
private:
    const std::string service_dir = "/var/service";
    const std::string supervisor_bin = "fasdsupervisor";
    std::map<std::string, pid_t> active_services;
    int signal_fd = -1;
    sigset_t signal_mask;
    bool keep_scanning = true;

    void init_signals() {
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, SIGCHLD);
        sigaddset(&signal_mask, SIGINT);
        sigaddset(&signal_mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &signal_mask, nullptr);
        signal_fd = signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    }

    void start_supervisor(const std::string& name, const std::string& path) {
        pid_t pid = fork();
        if (pid == 0) {
            sigset_t empty_mask;
            sigemptyset(&empty_mask);
            sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
            char* argv[] = {
                const_cast<char*>(supervisor_bin.c_str()),
                const_cast<char*>(name.c_str()),
                const_cast<char*>(path.c_str()),
                nullptr
            };
            execvp(supervisor_bin.c_str(), argv);
            _exit(127);
        } else if (pid > 0) {
            active_services[name] = pid;
        }
    }

    void stop_supervisor(const std::string& name, pid_t pid) {
        kill(pid, SIGTERM);
        int status;
        for (int i = 0; i < 20; ++i) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                active_services.erase(name);
                return;
            }
            usleep(10000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        active_services.erase(name);
    }

    void scan_directory() {
        if (!fs::exists(service_dir)) return;
        std::set<std::string> current_dirs;
        for (const auto& entry : fs::directory_iterator(service_dir)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            std::string path = entry.path().string();
            if (name.rfind(".", 0) == 0) continue;
            current_dirs.insert(name);
            if (active_services.find(name) == active_services.end()) {
                start_supervisor(name, path);
            }
        }
        auto it = active_services.begin();
        while (it != active_services.end()) {
            if (current_dirs.find(it->first) == current_dirs.end()) {
                std::string name = it->first;
                pid_t pid = it->second;
                it = active_services.erase(it);
                stop_supervisor(name, pid);
            } else {
                ++it;
            }
        }
    }

    void handle_sigchld() {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (auto const& [name, active_pid] : active_services) {
                if (active_pid == pid) {
                    active_services.erase(name);
                    break;
                }
            }
        }
    }

public:
    FasdScan() { init_signals(); }
    ~FasdScan() { if (signal_fd >= 0) close(signal_fd); }

    void run() {
        struct signalfd_siginfo fdsi;
        while (keep_scanning) {
            scan_directory();
            for (int i = 0; i < 100; ++i) {
                ssize_t s = read(signal_fd, &fdsi, sizeof(struct signalfd_siginfo));
                if (s == sizeof(struct signalfd_siginfo)) {
                    if (fdsi.ssi_signo == SIGCHLD) handle_sigchld();
                    else if (fdsi.ssi_signo == SIGINT || fdsi.ssi_signo == SIGTERM) {
                        keep_scanning = false;
                        break;
                    }
                }
                usleep(10000);
            }
        }
        for (auto const& [name, pid] : active_services) kill(pid, SIGTERM);
        int status;
        while (waitpid(-1, &status, 0) > 0);
    }
};

int main() {
    FasdScan scanner;
    scanner.run();
    return 0;
}
