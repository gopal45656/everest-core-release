#include "TCPReceiver.hpp"

#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <chrono>
#include <thread>

namespace module {

void TCPReceiver::init() {
    invoke_init(*p_main);
}

void TCPReceiver::ready() {
    invoke_ready(*p_main);

    EVLOG_info << "TCPReceiver ready, starting TCP "
               << config.socket_mode
               << " on " << config.host << ":" << config.port;

    server_thread = std::thread([this]() {

        while (running) {

            int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (sock_fd < 0) {
                EVLOG_error << "Failed to create TCP socket";
                return;
            }

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(config.port);
            inet_pton(AF_INET, config.host.c_str(), &address.sin_addr);

            int client_fd = -1;

            if (config.socket_mode == "server") {

                int opt = 1;
                setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

                if (bind(sock_fd,
                         reinterpret_cast<struct sockaddr*>(&address),
                         sizeof(address)) < 0) {
                    EVLOG_error << "Bind failed";
                    close(sock_fd);
                    return;
                }

                if (listen(sock_fd, 1) < 0) {
                    EVLOG_error << "Listen failed";
                    close(sock_fd);
                    return;
                }

                EVLOG_info << "Listening on TCP port " << config.port;

                client_fd = accept(sock_fd, nullptr, nullptr);
                if (client_fd < 0) {
                    EVLOG_error << "Accept failed";
                    close(sock_fd);
                    continue;
                }

                EVLOG_info << "Client connected";

            } else { // client mode

                if (connect(sock_fd,
                            reinterpret_cast<struct sockaddr*>(&address),
                            sizeof(address)) < 0) {
                    EVLOG_warning << "Connect failed, retrying...";
                    close(sock_fd);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(config.reconnect_interval_ms));
                    continue;
                }

                EVLOG_info << "Connected to server";
                client_fd = sock_fd;
            }

            // Receive loop
            while (running) {
                int value = 0;
                ssize_t bytes = recv(client_fd, &value, sizeof(value), MSG_WAITALL);

                if (bytes == 0) {
                    EVLOG_info << "Connection closed";
                    break;
                }

                if (bytes < 0) {
                    EVLOG_error << "recv() failed";
                    break;
                }

                if (bytes != sizeof(value)) {
                    EVLOG_warning << "Partial read: " << bytes << " bytes";
                    continue;
                }

                value = ntohl(value);  // network to host byte order

                EVLOG_info << "Received binary value: " << value;
                p_main->publish_value(value);

                if (value == -1) {
                    running = false;
                    break;
                }
            }

            close(client_fd);
            if (config.socket_mode == "server")
                close(sock_fd);

            if (running) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config.reconnect_interval_ms));
            }
        }

        EVLOG_info << "TCP socket stopped";
    });
}

TCPReceiver::~TCPReceiver() {
    running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

} // namespace module
