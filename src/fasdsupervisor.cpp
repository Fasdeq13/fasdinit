#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <csignal>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

namespace fs = std::filesystem;

class FasdSupervisor {
private:
    std::string service_name;
    std::string service_path;
    std::string run_script;
    std::string finish_script;
    pid_t pid = -1;
    int signal_fd = -1;
    int fifo_fd = -1;
    sigset_t signal_mask;
    bool keep_running = true;
    bool wants_down = false;
    bool paused = false;
    bool fallback_mode = false;
    struct timeval start_time;

    enum ServiceState { DOWN = 0, RUNNING = 1, FINISH = 2 } state = DOWN;

    const std::vector<std::string> display_managers = {
        "/usr/bin/sddm",
        "/usr/sbin/lightdm",
        "/usr/bin/lightdm",
        "/usr/sbin/gdm",
        "/usr/bin/gdm",
        "/usr/bin/lxdm",
        "/usr/sbin/lxdm"
    };

    void get_tai64(unsigned char* pack) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        uint64_t secs = 4611686018427387914ULL + tv.tv_sec;
        uint32_t nanos = tv.tv_usec * 1000;
        pack[0] = (secs >> 56) & 0xff; pack[1] = (secs >> 48) & 0xff;
        pack[2] = (secs >> 40) & 0xff; pack[3] = (secs >> 32) & 0xff;
        pack[4] = (secs >> 24) & 0xff; pack[5] = (secs >> 16) & 0xff;
        pack[6] = (secs >> 8) & 0xff;  pack[7] = secs & 0xff;
        pack[8] = (nanos >> 24) & 0xff; pack[9] = (nanos >> 16) & 0xff;
        pack[10] = (nanos >> 8) & 0xff; pack[11] = nanos & 0xff;
    }

    void write_runit_status() {
        unsigned char status_bytes[20];
        std::memset(status_bytes, 0, sizeof(status_bytes));
        get_tai64(status_bytes);
        if (pid > 0) {
            status_bytes[12] = pid & 0xff;
            status_bytes[13] = (pid >> 8) & 0xff;
            status_bytes[14] = (pid >> 16) & 0xff;
            status_bytes[15] = (pid >> 24) & 0xff;
        }
        status_bytes[16] = paused ? 1 : 0;
        status_bytes[17] = wants_down ? 1 : 0;
        status_bytes[18] = (state == RUNNING) ? 1 : ((state == FINISH) ? 2 : 0);

        std::string status_path = service_path + "/status";
        int fd = open(status_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, status_bytes, sizeof(status_bytes));
            close(fd);
        }
    }

    void open_control_pipe() {
        std::string fifo_path = service_path + "/control";
        mkfifo(fifo_path.c_str(), 0600);
        fifo_fd = open(fifo_path.c_str(), O_RDONLY | O_NONBLOCK);
    }

    void setup_log_redirection() {
        std::string log_dir = "/var/log/fasdinit";
        fs::create_directories(log_dir);
        std::string log_file = log_dir + "/" + service_name + ".log";
        int fd = open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
    }

    bool try_execute_display_manager() {
        for (const auto& dm : display_managers) {
            if (fs::exists(dm)) {
                char* argv[] = { const_cast<char*>(dm.c_str()), nullptr };
                execv(dm.c_str(), argv);
                return true;
            }
        }
        return false;
    }

    void execute_fallback_console() {
        int fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        } else {
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                if (isatty(STDIN_FILENO) == 0) dup2(null_fd, STDIN_FILENO);
                if (null_fd > 2) close(null_fd);
            }
        }

        char* envp[] = { const_cast<char*>("TERM=xterm-256color"), const_cast<char*>("PATH=/usr/bin:/usr/sbin:/bin:/sbin:."), nullptr };

        if (fs::exists("/usr/sbin/agetty")) {
            char* argv[] = { const_cast<char*>("/usr/sbin/agetty"), const_cast<char*>("--noclear"), const_cast<char*>("tty1"), const_cast<char*>("38400"), const_cast<char*>("linux"), nullptr };
            execve("/usr/sbin/agetty", argv, envp);
        } else if (fs::exists("/sbin/agetty")) {
            char* argv[] = { const_cast<char*>("/sbin/agetty"), const_cast<char*>("--noclear"), const_cast<char*>("tty1"), const_cast<char*>("38400"), const_cast<char*>("linux"), nullptr };
            execve("/sbin/agetty", argv, envp);
        } else {
            char* argv[] = { const_cast<char*>("/bin/bash"), const_cast<char*>("-i"), nullptr };
            execve("/bin/bash", argv, envp);
        }
    }

    void handle_control_command(char cmd) {
        switch (cmd) {
            case 'u': wants_down = false; if (state == DOWN) start_service(); break;
            case 'd': wants_down = true; if (state == RUNNING) stop_service(SIGTERM); break;
            case 'o': wants_down = true; if (state == DOWN) start_service(); break;
            case 'a': if (state == RUNNING) kill(pid, SIGALRM); break;
            case 'h': if (state == RUNNING) kill(pid, SIGHUP); break;
            case 'i': if (state == RUNNING) kill(pid, SIGINT); break;
            case 'q': if (state == RUNNING) kill(pid, SIGQUIT); break;
            case 't': if (state == RUNNING) kill(pid, SIGTERM); break;
            case 'k': if (state == RUNNING) kill(pid, SIGKILL); break;
            case 'p': if (state == RUNNING && !paused) { kill(pid, SIGSTOP); paused = true; write_runit_status(); } break;
            case 'c': if (state == RUNNING && paused) { kill(pid, SIGCONT); paused = false; write_runit_status(); } break;
            case 'x': keep_running = false; wants_down = true; if (state == RUNNING) stop_service(SIGTERM); break;
        }
    }

    void check_fifo() {
        if (fifo_fd < 0) return;
        char cmd;
        ssize_t n = read(fifo_fd, &cmd, 1);
        if (n > 0) handle_control_command(cmd);
    }

    void init_signals() {
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, SIGCHLD);
        sigaddset(&signal_mask, SIGTERM);
        sigaddset(&signal_mask, SIGINT);
        sigprocmask(SIG_BLOCK, &signal_mask, nullptr);
        signal_fd = signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    }

    void start_service() {
        if (wants_down || !keep_running) { state = DOWN; pid = -1; write_runit_status(); return; }

        bool display_manager_service = (service_name == "display-manager" || service_name == "tty1");

        if (!display_manager_service && !fs::exists(run_script)) {
            state = DOWN;
            write_runit_status();
            return;
        }

        pid_t p = fork();
        if (p == 0) {
            sigset_t empty_mask; sigemptyset(&empty_mask); sigprocmask(SIG_SETMASK, &empty_mask, nullptr); setsid();

            if (display_manager_service) {
                if (!fallback_mode) {
                    if (!try_execute_display_manager()) {
                        fallback_mode = true;
                        execute_fallback_console();
                    }
                } else {
                    execute_fallback_console();
                }
            } else {
                if (!fs::exists(service_path + "/nolog")) {
                    setup_log_redirection();
                }
                char* argv[] = { const_cast<char*>(run_script.c_str()), nullptr };
                execv(run_script.c_str(), argv);
            }
            _exit(127);
        } else if (p > 0) {
            pid = p; state = RUNNING; paused = false;
            gettimeofday(&start_time, nullptr);
            write_runit_status();
        }
    }

    void run_finish_script() {
        if (!fs::exists(finish_script)) { state = DOWN; pid = -1; write_runit_status(); start_service(); return; }
        pid_t p = fork();
        if (p == 0) {
            sigset_t empty_mask; sigemptyset(&empty_mask); sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
            if (!fs::exists(service_path + "/nolog") && service_name != "display-manager" && service_name != "tty1") {
                setup_log_redirection();
            }
            char* argv[] = { const_cast<char*>(finish_script.c_str()), nullptr };
            execv(finish_script.c_str(), argv);
            _exit(127);
        } else if (p > 0) { pid = p; state = FINISH; write_runit_status(); }
    }

    void stop_service(int sig) { if (pid > 0) kill(pid, sig); }

    void handle_sigchld() {
        int status; pid_t p;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
            if (p == pid) {
                if (state == RUNNING) run_finish_script();
                else if (state == FINISH) {
                    state = DOWN; pid = -1; write_runit_status();
                    struct timeval now; gettimeofday(&now, nullptr);
                    if (now.tv_sec - start_time.tv_sec < 1) sleep(1);
                    if (!wants_down && keep_running) start_service();
                }
            }
        }
    }

public:
    FasdSupervisor(const std::string& name, const std::string& path) : service_name(name), service_path(path) {
        run_script = service_path + "/run"; finish_script = service_path + "/finish";
        init_signals(); open_control_pipe();
    }
    ~FasdSupervisor() {
        if (signal_fd >= 0) close(signal_fd); if (fifo_fd >= 0) close(fifo_fd);
        unlink((service_path + "/control").c_str()); unlink((service_path + "/status").c_str());
    }

    void run() {
        if (fs::exists(service_path + "/down")) wants_down = true;
        if (!wants_down) start_service();
        else write_runit_status();

        struct signalfd_siginfo fdsi;
        while (keep_running || state != DOWN) {
            check_fifo();
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(signal_fd, &fds);

            struct timeval tv = {0, 10000};
            int ret = select(signal_fd + 1, &fds, nullptr, nullptr, &tv);

            if (ret > 0 && FD_ISSET(signal_fd, &fds)) {
                ssize_t s = read(signal_fd, &fdsi, sizeof(struct signalfd_siginfo));
                if (s == sizeof(struct signalfd_siginfo)) {
                    if (fdsi.ssi_signo == SIGCHLD) {
                        handle_sigchld();
                    } else if (fdsi.ssi_signo == SIGTERM || fdsi.ssi_signo == SIGINT) {
                        wants_down = true;
                        keep_running = false;
                        if (state == RUNNING) {
                            stop_service(SIGTERM);
                        }
                    }
                }
            }
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3 || argv[1] == nullptr || argv[2] == nullptr) {
        return 1;
    }
    FasdSupervisor supervisor(argv[1], argv[2]);
    supervisor.run();
    return 0;
}
