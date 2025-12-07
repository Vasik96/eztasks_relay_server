#include "eztasks_relay_server.h"
#include <queue>

constexpr int PORT = 20001;
constexpr int HEARTBEAT_TIMEOUT_SEC = 20;

std::mutex executor_mutex;
ClientConnection* Executor = nullptr;

// Buffer messages from clients until executor connects
std::mutex buffer_mutex;
std::queue<std::string> message_buffer;

void executor_loop(ClientConnection* executor);
void heartbeat_monitor();
void handle_connection(int client_socket);
void send_line_to_executor(const std::string& line);

void close_socket(int sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen failed");
        return 1;
    }

    std::cout << "Server running on port " << PORT << std::endl;

    std::thread(heartbeat_monitor).detach();

    while (true) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_socket < 0) {
            perror("accept failed");
            continue;
        }
        std::thread(handle_connection, client_socket).detach();
    }

    close_socket(server_fd);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}

// Reads a line from socket (until '\n')
bool read_line(int sock, std::string& out_line) {
    out_line.clear();
    char c;
    while (true) {
        ssize_t n =
#ifdef _WIN32
            recv(sock, &c, 1, 0);
#else
            read(sock, &c, 1);
#endif
        if (n <= 0) return false;  // disconnected

        if (c == '\n') break;
        if (c != '\r') out_line += c;
    }
    return true;
}

// Sends a line to the executor
void send_line_to_executor(const std::string& line) {
    if (!Executor) return;
    std::string payload = line + "\n";
#ifdef _WIN32
    send(Executor->socket_fd, payload.c_str(), (int)payload.size(), 0);
#else
    send(Executor->socket_fd, payload.c_str(), payload.size(), 0);
#endif
    std::cout << "Forwarded client payload to executor: " << line << std::endl;
}

void handle_connection(int client_socket) {
    std::vector<std::string> lines;

    // Read all lines from this client first
    std::string line;
    while (read_line(client_socket, line)) {
        lines.push_back(line);
    }

    if (lines.empty()) {
        close_socket(client_socket);
        return;
    }

    // Check if this is the executor
    if (lines[0] == "executor") {
        std::lock_guard<std::mutex> lock(executor_mutex);
        if (Executor != nullptr) {
            std::cout << "Executor already connected. Rejecting." << std::endl;
            close_socket(client_socket);
            return;
        }

        Executor = new ClientConnection{ client_socket, std::chrono::steady_clock::now() };
        std::cout << "Executor connected." << std::endl;

        // Run executor loop in its own thread
        std::thread(executor_loop, Executor).detach();
        return;
    }

    // Normal client
    std::cout << "[info] client connected" << std::endl;

    // Lock executor once and decide what to do with all messages
    std::lock_guard<std::mutex> lock(executor_mutex);
    if (Executor != nullptr) {
        // Forward all lines to executor
        for (const auto& l : lines) {
            send_line_to_executor(l);
        }
    }
    else {
        // Drop all messages immediately
        for (const auto& l : lines) {
            std::cout << "[warn] Executor offline — dropping: " << l << std::endl;
        }
    }

    close_socket(client_socket);
}




void executor_loop(ClientConnection* executor) {
    std::string line;
    while (read_line(executor->socket_fd, line)) {
        if (line.rfind("heartbeat::", 0) == 0) {
            std::cout << "[info] (executor) heartbeat received" << std::endl;
            executor->last_heartbeat = std::chrono::steady_clock::now();
        }
        else {
            std::cout << "Received from executor (ignored): " << line << std::endl;
        }
    }
    std::cout << "Executor disconnected." << std::endl;
    close_socket(executor->socket_fd);
}

void heartbeat_monitor() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::lock_guard<std::mutex> lock(executor_mutex);
        if (Executor != nullptr) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - Executor->last_heartbeat.load()
            ).count();
            if (elapsed > HEARTBEAT_TIMEOUT_SEC) {
                std::cout << "Executor heartbeat timeout, disconnecting." << std::endl;
                close_socket(Executor->socket_fd);
                delete Executor;
                Executor = nullptr;
            }
        }
    }
}
