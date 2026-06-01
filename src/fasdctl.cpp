#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

namespace fs = std::filesystem;

struct RunitStatus {
    unsigned char tai[12];
    unsigned char pid[4];
    unsigned char paused;
    unsigned char wants_down;
    unsigned char state;
};

uint64_t get_uptime(const unsigned char* tai) {
    uint64_t secs = ((uint64_t)tai[0] << 56) | ((uint64_t)tai[1] << 48) |
                    ((uint64_t)tai[2] << 40) | ((uint64_t)tai[3] << 32) |
                    ((uint64_t)tai[4] << 24) | ((uint64_t)tai[5] << 16) |
                    ((uint64_t)tai[6] << 8)  | (uint64_t)tai[7];

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint64_t current_tai = 4611686018427387914ULL + tv.tv_sec;

    if (current_tai > secs) return current_tai - secs;
    return 0;
}

void print_status(const std::string& service, const std::string& service_path) {
    std::string status_path = service_path + "/status";
    std::ifstream ifs(status_path, std::ios::binary);

    if (!ifs.is_open()) {
        std::cout << "[ WARNING ] " << service << ": Unable to read status file (initializing or missing)\n";
        return;
    }

    RunitStatus status;
    ifs.read(reinterpret_cast<char*>(&status), sizeof(status));
    ifs.close();

    pid_t pid = status.pid[0] | (status.pid[1] << 8) | (status.pid[2] << 16) | (status.pid[3] << 24);
    uint64_t uptime_secs = get_uptime(status.tai);

    if (status.state == 1 && pid > 0) {
        std::cout << "[ OK ] " << service << " is running (PID: " << pid << ", Uptime: " << uptime_secs << "s)";
        if (status.paused) std::cout << " [PAUSED]";
        std::cout << "\n";
    } else if (status.state == 2 && pid > 0) {
        std::cout << "[ WARNING ] " << service << " is executing finish script (PID: " << pid << ")\n";
    } else {
        std::cout << "[ FAILED ] " << service << " is stopped";
        if (status.wants_down) std::cout << " (Configured state: DOWN)";
        std::cout << "\n";
    }
}

void view_logs(const std::string& service, bool follow) {
    std::string log_file = "/var/log/fasdinit/" + service + ".log";

    if (!fs::exists(log_file)) {
        std::cout << "[ WARNING ] No logs found for service '" << service << "' yet.\n";
        return;
    }

    std::ifstream file(log_file);
    if (!file.is_open()) {
        std::cerr << "[ FAILED ] Cannot open log file: " << log_file << "\n";
        return;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
        if (lines.size() > 20) {
            lines.erase(lines.begin());
        }
    }

    std::cout << "--- Logs for " << service << " (Plain Text) ---\n";
    for (const auto& l : lines) {
        std::cout << l << "\n";
    }

    if (follow) {
        file.clear();
        while (true) {
            while (std::getline(file, line)) {
                std::cout << line << "\n";
            }
            file.clear();
            usleep(100000);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argv[1] == nullptr || argv[2] == nullptr) {
        std::cout << "Usage: fasdctl [start|stop|restart|status|log] [service_name] (-f)\n";
        return 1;
    }

    std::string action = argv[1];
    std::string service = argv[2];
    std::string service_path = "/var/service/" + service;

    if (!fs::exists(service_path)) {
        std::cerr << "[ FAILED ] fasdctl: Service directory '" << service_path << "' not found\n";
        return 1;
    }

    if (action == "status") {
        print_status(service, service_path);
        return 0;
    }

    if (action == "log") {
        bool follow = (argc > 3 && std::strcmp(argv[3], "-f") == 0);
        view_logs(service, follow);
        return 0;
    }

    std::string fifo_path = service_path + "/control";
    int fd = open(fifo_path.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "[ FAILED ] fasdctl: Control pipe offline (Supervisor not running)\n";
        return 1;
    }

    char cmd = '\0';
    if (action == "start") cmd = 'u';
    else if (action == "stop") cmd = 'd';
    else if (action == "restart") cmd = 't';
    else {
        std::cerr << "[ FAILED ] fasdctl: Unknown action '" << action << "'\n";
        close(fd);
        return 1;
    }

    if (write(fd, &cmd, 1) < 0) {
        std::cerr << "[ FAILED ] fasdctl: IPC write failed\n";
        close(fd);
        return 1;
    }

    close(fd);
    std::cout << "[ OK ] Command '" << action << "' dispatched to " << service << "\n";
    return 0;
}
