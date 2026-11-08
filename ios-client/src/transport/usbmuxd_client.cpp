#include "transport/usbmuxd_client.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <pwd.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace ios::transport {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view ExtractTagValue(std::string_view xml, std::string_view key, std::string_view tag_name) {
    std::string key_tag = "<key>" + std::string(key) + "</key>";
    auto pos = xml.find(key_tag);
    if (pos == std::string_view::npos) {
        return {};
    }

    auto after_key = xml.substr(pos + key_tag.size());
    std::string open_tag = "<" + std::string(tag_name) + ">";
    std::string close_tag = "</" + std::string(tag_name) + ">";

    auto tag_pos = after_key.find(open_tag);
    if (tag_pos == std::string_view::npos) {
        return {};
    }

    auto next_key = after_key.find("<key>");
    if (next_key != std::string_view::npos && next_key < tag_pos) {
        return {};
    }

    auto val_start = tag_pos + open_tag.size();
    auto close_pos = after_key.find(close_tag, val_start);
    if (close_pos == std::string_view::npos) {
        return {};
    }

    return Trim(after_key.substr(val_start, close_pos - val_start));
}

int64_t ExtractInteger(std::string_view xml, std::string_view key, int64_t default_val = -1) {
    auto val = ExtractTagValue(xml, key, "integer");
    if (val.empty()) {
        return default_val;
    }
    try {
        return std::stoll(std::string(val));
    } catch (...) {
        return default_val;
    }
}

std::string ExtractString(std::string_view xml, std::string_view key) {
    return std::string(ExtractTagValue(xml, key, "string"));
}

std::vector<std::string_view> ExtractArrayDicts(std::string_view xml, std::string_view array_key) {
    std::vector<std::string_view> result;
    std::string key_tag = "<key>" + std::string(array_key) + "</key>";
    auto pos = xml.find(key_tag);
    if (pos == std::string_view::npos) {
        return result;
    }

    auto after_key = xml.substr(pos + key_tag.size());
    auto arr_start = after_key.find("<array>");
    if (arr_start == std::string_view::npos) {
        return result;
    }

    auto arr_content = after_key.substr(arr_start + 7);
    auto arr_end = arr_content.find("</array>");
    if (arr_end == std::string_view::npos) {
        return result;
    }

    std::string_view inner = arr_content.substr(0, arr_end);

    size_t cur = 0;
    while (cur < inner.size()) {
        auto d_start = inner.find("<dict>", cur);
        if (d_start == std::string_view::npos) {
            break;
        }

        size_t depth = 1;
        size_t scan = d_start + 6;
        while (depth > 0 && scan < inner.size()) {
            auto next_open = inner.find("<dict>", scan);
            auto next_close = inner.find("</dict>", scan);
            if (next_close == std::string_view::npos) {
                break;
            }

            if (next_open != std::string_view::npos && next_open < next_close) {
                depth++;
                scan = next_open + 6;
            } else {
                depth--;
                scan = next_close + 7;
                if (depth == 0) {
                    result.push_back(inner.substr(d_start, scan - d_start));
                    cur = scan;
                    break;
                }
            }
        }
        if (depth != 0) {
            break;
        }
    }
    return result;
}

} // namespace

UsbmuxdClient::UsbmuxdClient(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

UsbmuxdClient::UsbmuxdClient(UsbmuxdClient&& other) noexcept
    : socket_fd_(other.socket_fd_),
      socket_path_(std::move(other.socket_path_)),
      pair_record_dir_(std::move(other.pair_record_dir_)),
      tag_(other.tag_) {
    other.socket_fd_ = -1;
}

UsbmuxdClient& UsbmuxdClient::operator=(UsbmuxdClient&& other) noexcept {
    if (this != &other) {
        Disconnect();
        socket_fd_ = other.socket_fd_;
        socket_path_ = std::move(other.socket_path_);
        pair_record_dir_ = std::move(other.pair_record_dir_);
        tag_ = other.tag_;
        other.socket_fd_ = -1;
    }
    return *this;
}

UsbmuxdClient::~UsbmuxdClient() {
    Disconnect();
}

shared::Result<void, UsbmuxdError> UsbmuxdClient::Connect(std::chrono::milliseconds timeout) {
    if (socket_fd_ >= 0) {
        Disconnect();
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return UsbmuxdError::SocketConnectionFailed;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return UsbmuxdError::SocketConnectionFailed;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return UsbmuxdError::SocketConnectionFailed;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0) {
        if (errno != EINPROGRESS) {
            int err = errno;
            ::close(fd);
            if (err == EACCES || err == EPERM) {
                return UsbmuxdError::PermissionDenied;
            }
            return UsbmuxdError::SocketConnectionFailed;
        }

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int poll_ret = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (poll_ret == 0) {
            ::close(fd);
            return UsbmuxdError::Timeout;
        } else if (poll_ret < 0) {
            ::close(fd);
            return UsbmuxdError::SocketConnectionFailed;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            ::close(fd);
            if (so_error == EACCES || so_error == EPERM) {
                return UsbmuxdError::PermissionDenied;
            }
            return UsbmuxdError::SocketConnectionFailed;
        }
    }

    // Restore blocking mode
    if (::fcntl(fd, F_SETFL, flags) < 0) {
        ::close(fd);
        return UsbmuxdError::SocketConnectionFailed;
    }

    socket_fd_ = fd;
    return {};
}

void UsbmuxdClient::Disconnect() noexcept {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

shared::Result<void, UsbmuxdError> UsbmuxdClient::SendPacket(uint32_t message_type,
                                                             const std::string& plist_payload) {
    if (socket_fd_ < 0) {
        return UsbmuxdError::SocketConnectionFailed;
    }

    wire::UsbmuxdHeader header{};
    header.length = static_cast<uint32_t>(sizeof(wire::UsbmuxdHeader) + plist_payload.size());
    header.version = static_cast<uint32_t>(wire::UsbmuxdVersion::Plist);
    header.message = message_type;
    header.tag = ++tag_;

    // Write header
    ssize_t bytes_written = ::send(socket_fd_, &header, sizeof(header), MSG_NOSIGNAL);
    if (bytes_written != static_cast<ssize_t>(sizeof(header))) {
        Disconnect();
        return UsbmuxdError::BrokenPipe;
    }

    // Write payload
    if (!plist_payload.empty()) {
        size_t total_sent = 0;
        while (total_sent < plist_payload.size()) {
            ssize_t sent = ::send(socket_fd_,
                                  plist_payload.data() + total_sent,
                                  plist_payload.size() - total_sent,
                                  MSG_NOSIGNAL);
            if (sent <= 0) {
                Disconnect();
                return UsbmuxdError::BrokenPipe;
            }
            total_sent += static_cast<size_t>(sent);
        }
    }

    return {};
}

shared::Result<std::string, UsbmuxdError> UsbmuxdClient::ReceivePacket(
    uint32_t expected_tag, std::chrono::milliseconds timeout) {
    if (socket_fd_ < 0) {
        return UsbmuxdError::SocketConnectionFailed;
    }

    struct pollfd pfd{};
    pfd.fd = socket_fd_;
    pfd.events = POLLIN;

    int poll_ret = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (poll_ret == 0) {
        return UsbmuxdError::Timeout;
    } else if (poll_ret < 0) {
        Disconnect();
        return UsbmuxdError::BrokenPipe;
    }

    if (!(pfd.revents & POLLIN) && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        Disconnect();
        return UsbmuxdError::BrokenPipe;
    }

    wire::UsbmuxdHeader header{};
    ssize_t bytes_read = ::recv(socket_fd_, &header, sizeof(header), MSG_WAITALL);
    if (bytes_read != static_cast<ssize_t>(sizeof(header))) {
        Disconnect();
        return UsbmuxdError::BrokenPipe;
    }

    if (header.length < sizeof(wire::UsbmuxdHeader)) {
        Disconnect();
        return UsbmuxdError::ProtocolViolation;
    }

    if (expected_tag != 0 && header.tag != expected_tag) {
        // Tag mismatch
        Disconnect();
        return UsbmuxdError::ProtocolViolation;
    }

    uint32_t payload_len = header.length - sizeof(wire::UsbmuxdHeader);
    std::string payload;
    if (payload_len > 0) {
        payload.resize(payload_len);
        ssize_t p_read = ::recv(socket_fd_, payload.data(), payload_len, MSG_WAITALL);
        if (p_read != static_cast<ssize_t>(payload_len)) {
            Disconnect();
            return UsbmuxdError::BrokenPipe;
        }
    }

    return payload;
}

shared::Result<std::vector<UsbDeviceEntry>, UsbmuxdError> UsbmuxdClient::EnumerateDevices() {
    if (socket_fd_ < 0) {
        auto conn_res = Connect();
        if (conn_res.is_err()) {
            return conn_res.error();
        }
    }

    std::string req =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>ClientVersionString</key>\n"
        "\t<string>ios-mirror</string>\n"
        "\t<key>MessageType</key>\n"
        "\t<string>ListDevices</string>\n"
        "\t<key>ProgName</key>\n"
        "\t<string>ios-mirror</string>\n"
        "</dict>\n"
        "</plist>\n";

    auto send_res = SendPacket(static_cast<uint32_t>(wire::UsbmuxdMessageType::PlistWrapper), req);
    if (send_res.is_err()) {
        return send_res.error();
    }

    auto recv_res = ReceivePacket(tag_, std::chrono::milliseconds(2000));
    if (recv_res.is_err()) {
        return recv_res.error();
    }

    const std::string& payload = recv_res.value();
    std::vector<UsbDeviceEntry> devices;

    auto dicts = ExtractArrayDicts(payload, "DeviceList");
    for (auto d : dicts) {
        UsbDeviceEntry entry{};
        entry.device_id = static_cast<uint32_t>(ExtractInteger(d, "DeviceID", 0));
        entry.product_id = static_cast<uint32_t>(ExtractInteger(d, "ProductID", 0));
        entry.serial_number = ExtractString(d, "SerialNumber");
        entry.location_id = static_cast<uint32_t>(ExtractInteger(d, "LocationID", 0));
        entry.connection_speed = static_cast<uint32_t>(ExtractInteger(d, "ConnectionSpeed", 0));
        devices.push_back(std::move(entry));
    }

    return devices;
}

shared::Result<int, UsbmuxdError> UsbmuxdClient::ConnectToDevicePort(uint32_t device_id, uint16_t port) {
    if (socket_fd_ < 0) {
        auto conn_res = Connect();
        if (conn_res.is_err()) {
            return conn_res.error();
        }
    }

    // Wire port forwarding requires big-endian byte-order mapping
    uint16_t port_be = static_cast<uint16_t>(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));

    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
       << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       << "<plist version=\"1.0\">\n"
       << "<dict>\n"
       << "\t<key>ClientVersionString</key>\n"
       << "\t<string>ios-mirror</string>\n"
       << "\t<key>DeviceID</key>\n"
       << "\t<integer>" << device_id << "</integer>\n"
       << "\t<key>MessageType</key>\n"
       << "\t<string>Connect</string>\n"
       << "\t<key>PortNumber</key>\n"
       << "\t<integer>" << port_be << "</integer>\n"
       << "\t<key>ProgName</key>\n"
       << "\t<string>ios-mirror</string>\n"
       << "</dict>\n"
       << "</plist>\n";

    std::string req = ss.str();
    auto send_res = SendPacket(static_cast<uint32_t>(wire::UsbmuxdMessageType::PlistWrapper), req);
    if (send_res.is_err()) {
        return send_res.error();
    }

    auto recv_res = ReceivePacket(tag_, std::chrono::milliseconds(2000));
    if (recv_res.is_err()) {
        return recv_res.error();
    }

    const std::string& payload = recv_res.value();
    int64_t result_code = -1;

    if (payload.size() >= 4 && payload.find("<plist") == std::string::npos) {
        uint32_t raw_code = 0;
        std::memcpy(&raw_code, payload.data(), 4);
        result_code = static_cast<int64_t>(raw_code);
    } else {
        result_code = ExtractInteger(payload, "Number", -1);
    }

    if (result_code == 0) {
        // Success: socket is now directly tunneled to the device port
        int tunnel_fd = socket_fd_;
        socket_fd_ = -1;
        return tunnel_fd;
    }

    Disconnect();
    switch (result_code) {
        case 2:
            return UsbmuxdError::DeviceNotFound;
        case 3:
            return UsbmuxdError::DeviceNotPaired;
        case 6:
            return UsbmuxdError::PermissionDenied;
        default:
            return UsbmuxdError::HandshakeFailed;
    }
}

shared::Result<std::string, UsbmuxdError> UsbmuxdClient::ReadPairRecord(std::string_view udid) {
    if (udid.empty() || udid.find('/') != std::string_view::npos || udid.find('\\') != std::string_view::npos) {
        return UsbmuxdError::ProtocolViolation;
    }

    std::vector<std::string> candidates;
    if (!pair_record_dir_.empty()) {
        candidates.push_back(pair_record_dir_ + "/" + std::string(udid) + ".plist");
    }

    const char* env_path = std::getenv("USBMUXD_LOCKDOWN_PATH");
    if (env_path && env_path[0] != '\0') {
        candidates.push_back(std::string(env_path) + "/" + std::string(udid) + ".plist");
    }

    candidates.push_back("/var/lib/lockdown/" + std::string(udid) + ".plist");

    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        candidates.push_back(std::string(home) + "/.lockdown/" + std::string(udid) + ".plist");
    } else {
        struct passwd* pw = ::getpwuid(::getuid());
        if (pw && pw->pw_dir) {
            candidates.push_back(std::string(pw->pw_dir) + "/.lockdown/" + std::string(udid) + ".plist");
        }
    }

    bool permission_denied_found = false;

    for (const auto& path : candidates) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
            std::string content;
            char buffer[4096];
            ssize_t bytes = 0;
            while ((bytes = ::read(fd, buffer, sizeof(buffer))) > 0) {
                content.append(buffer, static_cast<size_t>(bytes));
            }
            ::close(fd);
            return content;
        } else {
            if (errno == EACCES || errno == EPERM) {
                permission_denied_found = true;
            }
        }
    }

    if (permission_denied_found) {
        return UsbmuxdError::PermissionDenied;
    }

    return UsbmuxdError::DeviceNotPaired;
}

shared::Result<bool, UsbmuxdError> UsbmuxdClient::VerifyDevicePairing(std::string_view udid) {
    auto record_res = ReadPairRecord(udid);
    if (record_res.is_err()) {
        if (record_res.error() == UsbmuxdError::DeviceNotPaired ||
            record_res.error() == UsbmuxdError::DeviceNotFound) {
            return false;
        }
        return record_res.error();
    }

    const std::string& record = record_res.value();
    if (record.find("<plist") == std::string::npos || record.find("</plist>") == std::string::npos) {
        return UsbmuxdError::PairRecordCorrupted;
    }

    // A valid pair record must contain device certificate or host identifier keys
    bool has_device_cert = (record.find("<key>DeviceCertificate</key>") != std::string::npos);
    bool has_host_id = (record.find("<key>HostID</key>") != std::string::npos);
    bool has_host_cert = (record.find("<key>HostCertificate</key>") != std::string::npos);
    bool has_root_cert = (record.find("<key>RootCertificate</key>") != std::string::npos);

    if (!has_device_cert && !has_host_id && !has_host_cert && !has_root_cert) {
        return UsbmuxdError::PairRecordCorrupted;
    }

    return true;
}

void UsbmuxdClient::SetSocketPath(std::string path) {
    socket_path_ = std::move(path);
}

const std::string& UsbmuxdClient::GetSocketPath() const noexcept {
    return socket_path_;
}

void UsbmuxdClient::SetPairRecordDirectory(std::string dir) {
    pair_record_dir_ = std::move(dir);
}

const std::string& UsbmuxdClient::GetPairRecordDirectory() const noexcept {
    return pair_record_dir_;
}

bool UsbmuxdClient::IsConnected() const noexcept {
    return socket_fd_ >= 0;
}

int UsbmuxdClient::GetSocket() const noexcept {
    return socket_fd_;
}

} // namespace ios::transport
