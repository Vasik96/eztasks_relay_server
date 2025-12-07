#pragma once

#include <iostream>
#include <thread>
#include <mutex>
#include <string>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using socklen_t = int;
using ssize_t = int;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

struct ClientConnection {
    int socket_fd;
    std::atomic<std::chrono::steady_clock::time_point> last_heartbeat;
};
