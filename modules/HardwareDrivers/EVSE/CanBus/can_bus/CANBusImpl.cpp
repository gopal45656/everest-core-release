// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "CANBusImpl.hpp"

namespace module {
namespace can_bus {

void CANBusImpl::init() {
    std::string ifname = mod->config.interface;
    handle_open(ifname);
}

void CANBusImpl::ready() {
    if (!is_open) {
        EVLOG_error << "CAN socket not open!";
        return;
    }

    running = true;

    // rx_thread = std::thread([this]() {
    //     while (running) {
    //         struct can_frame frame;
    //         int nbytes = read(socket_fd, &frame, sizeof(frame));

    //         if (nbytes <= 0) continue;

    //         Object result;
    //         result["id"] = frame.can_id & CAN_EFF_MASK;
    //         result["extended"] = (frame.can_id & CAN_EFF_FLAG) != 0;
    //         result["dlc"] = frame.can_dlc;

    //         Array data;
    //         for (int i = 0; i < frame.can_dlc; i++) {
    //             data.push_back(frame.data[i]);
    //         }

    //         result["data"] = data;

    //         // THIS is the important line
    //         publish_last_frame(result);

    //         EVLOG_info << "RX CAN id=" << result["id"];
    //     }
    // });
    rx_thread = std::thread([this]() {
        struct pollfd fds[1];
        fds[0].fd = socket_fd;
        fds[0].events = POLLIN;

        while (running) {
            int ret = poll(fds, 1, mod->config.recv_timeout_ms);

            if (ret < 0) {
                EVLOG_error << "poll() failed";
                continue;
            }

            if (ret == 0) {
                // timeout → no data
                continue;
            }

            if (fds[0].revents & POLLIN) {
                struct can_frame frame;
                int nbytes = read(socket_fd, &frame, sizeof(frame));

                if (nbytes < 0) {
                    EVLOG_error << "read() failed";
                    continue;
                }

                if (nbytes != sizeof(frame)) {
                    EVLOG_warning << "Incomplete CAN frame";
                    continue;
                }

                Object result;
                result["id"] = frame.can_id & CAN_EFF_MASK;
                result["extended"] = (frame.can_id & CAN_EFF_FLAG) != 0;
                result["dlc"] = frame.can_dlc;

                Array data;
                for (int i = 0; i < frame.can_dlc; i++) {
                    data.push_back(frame.data[i]);
                }

                result["data"] = data;

                publish_last_frame(result);

                // EVLOG_info << "RX CAN id=" << result["id"];
                int id = result["id"];

                std::stringstream ss;
                for (auto b : data) {
                    ss << std::hex << std::setw(2) << std::setfill('0')
                    << std::uppercase << (int)b << " ";
                }

                EVLOG_info << "RX " << std::setw(8) << std::setfill('0') << std::hex << std::uppercase << id << "   [" << std::dec << data.size() << "]  " << ss.str();
            }

            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                EVLOG_error << "CAN socket error";
                continue;
            }
        }
    });
}

bool CANBusImpl::handle_open(std::string& ifname) {
    struct ifreq ifr;
    struct sockaddr_can addr;

    // Bring the interface down first (required to change bitrate on a live interface)
    std::string down_cmd = "ip link set " + ifname + " down";
    int ret_down = std::system(down_cmd.c_str());
    if (ret_down != 0) {
        EVLOG_warning << "Failed to bring " << ifname << " down (may already be down): " << ret_down;
    }

    // Set bitrate and bring the interface up as a CAN interface
    std::string up_cmd = "ip link set " + ifname + " up type can bitrate " +
                         std::to_string(mod->config.bitrate);
    int ret_up = std::system(up_cmd.c_str());
    if (ret_up != 0) {
        EVLOG_error << "Failed to bring up CAN interface " << ifname
                    << " at " << mod->config.bitrate << " bps (exit=" << ret_up << ")";
        // Don't abort — the interface may already be up with the correct bitrate
    } else {
        EVLOG_info << "CAN interface " << ifname << " up at " << mod->config.bitrate << " bps";
    }

    // Increase TX queue length so multiple sockets on the same interface
    // don't overflow each other's send buffers (ENOBUFS)
    std::string txq_cmd = "ip link set " + ifname + " txqueuelen 1000";
    std::system(txq_cmd.c_str());

    fcntl(socket_fd, F_SETFL, O_NONBLOCK);

    socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd < 0) {
        return false;
    }

    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    if (ioctl(socket_fd, SIOCGIFINDEX, &ifr) < 0) {
        close(socket_fd);
        return false;
    }

    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(socket_fd);
        return false;
    }

    is_open = true;
    publish_is_open(true);
    return true;
}

void CANBusImpl::handle_close() {
    if (socket_fd >= 0) {
        close(socket_fd);
        socket_fd = -1;
    }
    is_open = false;
    publish_is_open(false);

    running = false;
    if (rx_thread.joinable()) {
        rx_thread.join();
    }
}

bool CANBusImpl::handle_send(int& id, bool& extended, int& dlc, Array& data) {
    if (!is_open) return false;

    struct can_frame frame;
    frame.can_id = id;

    if (extended) {
        frame.can_id |= CAN_EFF_FLAG;
    }

    frame.can_dlc = dlc;

    for (int i = 0; i < dlc && i < 8; i++) {
        frame.data[i] = data[i];
    }

    int nbytes = write(socket_fd, &frame, sizeof(frame));

    // ✅ LOG BEFORE RETURN
    if (nbytes == sizeof(frame)) {
        std::stringstream ss;
        for (int i = 0; i < dlc && i < data.size(); i++) {
            ss << std::hex << std::setw(2) << std::setfill('0')
               << std::uppercase << (int)data[i] << " ";
        }

        EVLOG_info << "TX " << std::setw(8) << std::setfill('0') << std::hex << std::uppercase << id << "   [" << std::dec << dlc << "]  " << ss.str();
    } else {
        EVLOG_error << "CAN TX failed";
    }

    return nbytes == sizeof(frame);
}

Object CANBusImpl::handle_receive() {
    Object result;

    if (!is_open) return result;

    struct can_frame frame;
    int nbytes = read(socket_fd, &frame, sizeof(frame));

    if (nbytes <= 0) return result;

    result["id"] = frame.can_id & CAN_EFF_MASK;
    result["extended"] = (frame.can_id & CAN_EFF_FLAG) != 0;
    result["dlc"] = frame.can_dlc;

    Array data;
    for (int i = 0; i < frame.can_dlc; i++) {
        data.push_back(frame.data[i]);
    }

    result["data"] = data;

    return result;
}

} // namespace can_bus
} // namespace module