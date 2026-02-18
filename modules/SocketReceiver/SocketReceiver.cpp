// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "SocketReceiver.hpp"

#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>
#include <algorithm>
#include <cctype>

namespace module {

void SocketReceiver::init() {
    invoke_init(*p_main);
}

void SocketReceiver::ready() {
    invoke_ready(*p_main);

    EVLOG_info << "SocketReceiver ready, starting UNIX socket server at "
               << config.socket_path;

    server_thread = std::thread([this]() {
        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd < 0) {
            EVLOG_error << "Failed to create UNIX socket";
            return;
        }

        unlink(config.socket_path.c_str());

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path,
                     config.socket_path.c_str(),
                     sizeof(address.sun_path) - 1);

        if (bind(server_fd,
                 reinterpret_cast<struct sockaddr*>(&address),
                 sizeof(address)) < 0) {
            EVLOG_error << "Bind failed for UNIX socket";
            close(server_fd);
            return;
        }

        if (listen(server_fd, 1) < 0) {
            EVLOG_error << "Listen failed";
            close(server_fd);
            return;
        }

        EVLOG_info << "Listening on UNIX socket " << config.socket_path;

        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            EVLOG_error << "Accept failed";
            close(server_fd);
            return;
        }

        EVLOG_info << "Client connected";

        // std::vector<char> buffer(BUFFER_SIZE);

        while (running) {
            int value = 0;
            ssize_t bytes = recv(client_fd, &value, sizeof(value), MSG_WAITALL);

            if (bytes == 0) {
                EVLOG_info << "Client disconnected";
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

            EVLOG_info << "Received binary value: " << value;

            p_main->publish_value(value);

            if (value == -1) {   // optional STOP convention
                running = false;
                break;
            }
            // // publish via interface
            // p_main->publish_value(value);
        }


        close(client_fd);
        close(server_fd);
        unlink(config.socket_path.c_str());

        EVLOG_info << "UNIX socket server stopped";
    });
}

SocketReceiver::~SocketReceiver() {
    running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

} // namespace module
