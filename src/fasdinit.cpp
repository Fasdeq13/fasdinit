#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <csignal>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

namespace fs = std::filesystem;

class FasdInit {
private:
    pid_t stage2_pid = -1;
    int signal_fd = -1;
    sigset_t signal_mask;
    bool is_rebooting = true;

    void setup_tty() {
        setsid();
        int fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }
        ioctl(STDIN_FILENO, TIOCSCTTY, 1);
        struct termios tty;
        if (tcgetattr(STDIN_FILENO, &tty) == 0) {
            tty.c_lflag |= (ISIG | ICANON | ECHO | ECHOE | ECHOK);
            tcsetattr(STDIN_FILENO, TCSANOW, &tty);
        }
        tcsetpgrp(STDIN_FILENO, getpid());
    }

    void mount_virtual_fs() {
        mount("proc", "/proc", "proc", 0, nullptr);
        mount("sysfs", "/sys", "sysfs", 0, nullptr);
        mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID | MS_NOEXEC, nullptr);
        fs::create_directories("/dev/pts");
        mount("devpts", "/dev/pts", "devpts", 0, nullptr);
        fs::create_directories("/dev/shm");
        mount("tmpfs", "/dev/shm", "tmpfs", 0, nullptr);
        fs::create_directories("/run");
        mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=755");

        fs::create_directories("/sys/fs/cgroup");
        mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, nullptr);
    }

    void init_cgroups() {
        std::string base_group = "/sys/fs/cgroup/system.slice";
        fs::create_directories(base_group);
        std::ofstream sub(base_group + "/cgroup.procs");
        if (sub.is_open()) {
            sub << getpid();
            sub.close();
        }
    }

    void init_signals() {
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, SIGCHLD);
        sigaddset(&signal_mask, SIGINT);
        sigaddset(&signal_mask, SIGTERM);
        sigaddset(&signal_mask, SIGPWR);
        sigprocmask(SIG_BLOCK, &signal_mask, nullptr);

        signal_fd = signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    }

    void run_script(const std::string& script_path, bool wait_for_completion) {
        if (!fs::exists(script_path)) return;
        pid_t pid = fork();
        if (pid == 0) {
            sigset_t empty_mask;
            sigemptyset(&empty_mask);
            sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
            char* argv[] = { const_cast<char*>(script_path.c_str()), nullptr };
            char* envp[] = { const_cast<char*>("TERM=xterm-256color"), const_cast<char*>("PATH=/usr/bin:/usr/sbin:/bin:/sbin"), nullptr };
            execve(script_path.c_str(), argv, envp);
            _exit(127);
        } else if (pid > 0) {
            if (wait_for_completion) {
                int status;
                while (waitpid(pid, &status, 0) < 0) {
                    if (errno != EINTR) break;
                }
            } else {
                stage2_pid = pid;
            }
        }
    }

    void terminate_stage2() {
        if (stage2_pid > 0) {
            kill(stage2_pid, SIGTERM);
            int status;
            for (int i = 0; i < 50; ++i) {
                pid_t r = waitpid(stage2_pid, &status, WNOHANG);
                if (r == stage2_pid) { stage2_pid = -1; return; }
                usleep(10000);
            }
            kill(stage2_pid, SIGKILL);
            while (waitpid(stage2_pid, &status, 0) < 0) {
                if (errno != EINTR) break;
            }
            stage2_pid = -1;
        }
    }

    void handle_sigchld() {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (pid == stage2_pid) stage2_pid = -1;
        }
    }

public:
    FasdInit() {
        if (getpid() != 1) {
            std::cerr << "Must be run as PID 1" << std::endl;
            _exit(1);
        }
    }

    ~FasdInit() {
        if (signal_fd >= 0) close(signal_fd);
    }

    void execute() {
        setup_tty();
        mount_virtual_fs();
        init_cgroups();
        init_signals();

        std::cout << "[fasdinit] Entering Stage 1" << std::endl;
        run_script("/etc/fasdinit/1", true);

        std::cout << "[fasdinit] Entering Stage 2" << std::endl;

        pid_t scan_pid = fork();
        if (scan_pid == 0) {
            sigset_t empty_mask;
            sigemptyset(&empty_mask);
            sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
            int fd = open("/dev/console", O_RDWR);
            if (fd >= 0) {
                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            char* argv[] = { const_cast<char*>("fasdscan"), nullptr };
            char* envp[] = { const_cast<char*>("TERM=xterm-256color"), const_cast<char*>("PATH=/usr/bin:/usr/sbin:/bin:/sbin:."), nullptr };
            execve("fasdscan", argv, envp);
            _exit(127);
        }
        stage2_pid = scan_pid;

        bool event_loop = true;
        struct signalfd_siginfo fdsi;
        fd_set fds;

        while (event_loop) {
            FD_ZERO(&fds);
            FD_SET(signal_fd, &fds);

            int ret = select(signal_fd + 1, &fds, nullptr, nullptr, nullptr);
            if (ret > 0 && FD_ISSET(signal_fd, &fds)) {
                ssize_t s = read(signal_fd, &fdsi, sizeof(struct signalfd_siginfo));
                if (s != sizeof(struct signalfd_siginfo)) continue;

                switch (fdsi.ssi_signo) {
                    case SIGCHLD:
                        handle_sigchld();
                        break;
                    case SIGINT:
                        is_rebooting = true;
                        event_loop = false;
                        break;
                    case SIGTERM:
                    case SIGPWR:
                        is_rebooting = false;
                        event_loop = false;
                        break;
                }
            }

            int status;
            pid_t check_scan = waitpid(stage2_pid, &status, WNOHANG);
            if (check_scan == stage2_pid) {
                stage2_pid = -1;
                event_loop = false;
            }
        }

        std::cout << "[fasdinit] Entering Stage 3" << std::endl;
        terminate_stage2();
        run_script("/etc/fasdinit/3", true);

        sync();
        if (is_rebooting) reboot(RB_AUTOBOOT);
        else reboot(RB_POWER_OFF);
    }
};

int main() {
    FasdInit init;
    init.execute();
    return 0;
}
