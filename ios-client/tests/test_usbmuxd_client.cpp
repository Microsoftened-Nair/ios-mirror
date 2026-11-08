#include "transport/usbmuxd_client.hpp"
#include "transport/usbmuxd_wire.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

const std::string kMockSocketPath = "/tmp/mock_usbmuxd_test.sock";

void SendHeaderAndPayload(int fd, uint32_t tag, const std::string& payload) {
    ios::transport::wire::UsbmuxdHeader resp{};
    resp.length = static_cast<uint32_t>(sizeof(resp) + payload.size());
    resp.version = static_cast<uint32_t>(ios::transport::wire::UsbmuxdVersion::Plist);
    resp.message = static_cast<uint32_t>(ios::transport::wire::UsbmuxdMessageType::PlistWrapper);
    resp.tag = tag;

    ssize_t s1 = ::send(fd, &resp, sizeof(resp), 0);
    assert(s1 == sizeof(resp));
    if (!payload.empty()) {
        ssize_t s2 = ::send(fd, payload.data(), payload.size(), 0);
        assert(s2 == static_cast<ssize_t>(payload.size()));
    }
}

// Background mock usbmuxd server
class MockUsbmuxdServer {
public:
    explicit MockUsbmuxdServer(std::string socket_path)
        : socket_path_(std::move(socket_path)), running_(false), server_fd_(-1) {}

    ~MockUsbmuxdServer() {
        Stop();
    }

    void Start() {
        ::unlink(socket_path_.c_str());

        server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        assert(server_fd_ >= 0);

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        int b = ::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        assert(b == 0);

        int l = ::listen(server_fd_, 5);
        assert(l == 0);

        running_ = true;
        server_thread_ = std::thread([this]() { Run(); });
    }

    void Stop() {
        if (!running_) {
            return;
        }
        running_ = false;
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }
        ::unlink(socket_path_.c_str());
    }

private:
    void Run() {
        while (running_) {
            struct pollfd pfd{};
            pfd.fd = server_fd_;
            pfd.events = POLLIN;
            int pr = ::poll(&pfd, 1, 50);
            if (pr <= 0 || !running_) {
                continue;
            }

            struct sockaddr_un client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = ::accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &len);
            if (client_fd < 0) {
                continue;
            }

            // Handle client requests in this connection
            while (running_) {
                ios::transport::wire::UsbmuxdHeader req_hdr{};
                ssize_t r = ::recv(client_fd, &req_hdr, sizeof(req_hdr), MSG_WAITALL);
                if (r <= 0) {
                    break;
                }

                uint32_t payload_len = req_hdr.length - sizeof(req_hdr);
                std::string payload(payload_len, '\0');
                if (payload_len > 0) {
                    ssize_t pr_bytes = ::recv(client_fd, payload.data(), payload_len, MSG_WAITALL);
                    if (pr_bytes <= 0) {
                        break;
                    }
                }

                if (payload.find("<string>ListDevices</string>") != std::string::npos) {
                    std::string resp_plist =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<plist version=\"1.0\">\n"
                        "<dict>\n"
                        "\t<key>DeviceList</key>\n"
                        "\t<array>\n"
                        "\t\t<dict>\n"
                        "\t\t\t<key>DeviceID</key>\n"
                        "\t\t\t<integer>42</integer>\n"
                        "\t\t\t<key>MessageType</key>\n"
                        "\t\t\t<string>Attached</string>\n"
                        "\t\t\t<key>Properties</key>\n"
                        "\t\t\t<dict>\n"
                        "\t\t\t\t<key>ConnectionSpeed</key>\n"
                        "\t\t\t\t<integer>480000000</integer>\n"
                        "\t\t\t\t<key>ConnectionType</key>\n"
                        "\t\t\t\t<string>USB</string>\n"
                        "\t\t\t\t<key>DeviceID</key>\n"
                        "\t\t\t\t<integer>42</integer>\n"
                        "\t\t\t\t<key>LocationID</key>\n"
                        "\t\t\t\t<integer>100</integer>\n"
                        "\t\t\t\t<key>ProductID</key>\n"
                        "\t\t\t\t<integer>4776</integer>\n"
                        "\t\t\t\t<key>SerialNumber</key>\n"
                        "\t\t\t\t<string>00008030-001234567890ABCD</string>\n"
                        "\t\t\t</dict>\n"
                        "\t\t</dict>\n"
                        "\t</array>\n"
                        "</dict>\n"
                        "</plist>\n";
                    SendHeaderAndPayload(client_fd, req_hdr.tag, resp_plist);
                } else if (payload.find("<string>Connect</string>") != std::string::npos) {
                    // Check big-endian mapping for port 62078 (0xF27E -> 0x7EF2 = 32498)
                    bool is_port_62078 = (payload.find("<integer>32498</integer>") != std::string::npos);
                    bool is_device_42 = (payload.find("<integer>42</integer>") != std::string::npos);

                    if (is_device_42 && is_port_62078) {
                        std::string resp_plist =
                            "<plist version=\"1.0\">\n"
                            "<dict>\n"
                            "\t<key>MessageType</key>\n"
                            "\t<string>Result</string>\n"
                            "\t<key>Number</key>\n"
                            "\t<integer>0</integer>\n"
                            "</dict>\n"
                            "</plist>\n";
                        SendHeaderAndPayload(client_fd, req_hdr.tag, resp_plist);

                        // Tunnel mode: echo test data
                        char echo_buf[64]{};
                        ssize_t eb = ::recv(client_fd, echo_buf, sizeof(echo_buf) - 1, 0);
                        if (eb > 0) {
                            if (std::string_view(echo_buf, static_cast<size_t>(eb)) == "PING") {
                                ::send(client_fd, "PONG", 4, 0);
                            }
                        }
                    } else {
                        // Error: DeviceNotFound (Number 2)
                        std::string resp_plist =
                            "<plist version=\"1.0\">\n"
                            "<dict>\n"
                            "\t<key>MessageType</key>\n"
                            "\t<string>Result</string>\n"
                            "\t<key>Number</key>\n"
                            "\t<integer>2</integer>\n"
                            "</dict>\n"
                            "</plist>\n";
                        SendHeaderAndPayload(client_fd, req_hdr.tag, resp_plist);
                    }
                    break;
                }
            }
            ::close(client_fd);
        }
    }

    std::string socket_path_;
    std::atomic<bool> running_{false};
    int server_fd_{-1};
    std::thread server_thread_;
};

} // namespace

void TestConnectAndEnumerate() {
    ios::transport::UsbmuxdClient client(kMockSocketPath);

    auto conn_res = client.Connect();
    assert(conn_res.is_ok());
    assert(client.IsConnected());

    auto dev_res = client.EnumerateDevices();
    assert(dev_res.is_ok());
    const auto& devices = dev_res.value();
    assert(devices.size() == 1);
    assert(devices[0].device_id == 42);
    assert(devices[0].product_id == 4776);
    assert(devices[0].serial_number == "00008030-001234567890ABCD");
    assert(devices[0].location_id == 100);
    assert(devices[0].connection_speed == 480000000);

    client.Disconnect();
    assert(!client.IsConnected());
}

void TestConnectToPortAndTunnel() {
    ios::transport::UsbmuxdClient client(kMockSocketPath);

    // Connecting to device 42, port 62078 (should be mapped to big-endian 32498)
    auto tunnel_res = client.ConnectToDevicePort(42, 62078);
    assert(tunnel_res.is_ok());

    int tunnel_fd = tunnel_res.value();
    assert(tunnel_fd >= 0);
    assert(!client.IsConnected()); // Ownership transferred to caller

    // Verify tunnel communication
    ssize_t s = ::send(tunnel_fd, "PING", 4, 0);
    assert(s == 4);

    char buf[16]{};
    ssize_t r = ::recv(tunnel_fd, buf, sizeof(buf) - 1, 0);
    assert(r == 4);
    assert(std::string_view(buf, 4) == "PONG");

    ::close(tunnel_fd);
}

void TestConnectToPortFailure() {
    ios::transport::UsbmuxdClient client(kMockSocketPath);

    // Device 99 does not exist on mock server
    auto tunnel_res = client.ConnectToDevicePort(99, 1234);
    assert(tunnel_res.is_err());
    assert(tunnel_res.error() == ios::transport::UsbmuxdError::DeviceNotFound);
}

void TestMoveSemantics() {
    ios::transport::UsbmuxdClient client1(kMockSocketPath);
    auto conn = client1.Connect();
    assert(conn.is_ok());
    assert(client1.IsConnected());
    int original_fd = client1.GetSocket();
    assert(original_fd >= 0);

    // Move construct
    ios::transport::UsbmuxdClient client2(std::move(client1));
    assert(!client1.IsConnected());
    assert(client1.GetSocket() == -1);
    assert(client2.IsConnected());
    assert(client2.GetSocket() == original_fd);

    // Move assign
    ios::transport::UsbmuxdClient client3;
    client3 = std::move(client2);
    assert(!client2.IsConnected());
    assert(client2.GetSocket() == -1);
    assert(client3.IsConnected());
    assert(client3.GetSocket() == original_fd);

    client3.Disconnect();
    assert(!client3.IsConnected());
}

void TestPairRecordValidation() {
    std::string test_dir = "/tmp/usbmuxd_test_lockdown";
    std::filesystem::create_directories(test_dir);

    std::string test_udid = "00008030-001234567890ABCD";

    // 1. Not paired
    {
        ios::transport::UsbmuxdClient client;
        client.SetPairRecordDirectory(test_dir);

        auto read_res = client.ReadPairRecord(test_udid);
        assert(read_res.is_err());
        assert(read_res.error() == ios::transport::UsbmuxdError::DeviceNotPaired);

        auto verify_res = client.VerifyDevicePairing(test_udid);
        assert(verify_res.is_ok());
        assert(verify_res.value() == false);
    }

    // 2. Corrupted pair record
    {
        std::string corrupted_path = test_dir + "/" + test_udid + ".plist";
        std::ofstream ofs(corrupted_path);
        ofs << "not a valid plist at all";
        ofs.close();

        ios::transport::UsbmuxdClient client;
        client.SetPairRecordDirectory(test_dir);

        auto verify_res = client.VerifyDevicePairing(test_udid);
        assert(verify_res.is_err());
        assert(verify_res.error() == ios::transport::UsbmuxdError::PairRecordCorrupted);
    }

    // 3. Valid pair record
    {
        std::string valid_path = test_dir + "/" + test_udid + ".plist";
        std::ofstream ofs(valid_path);
        ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            << "<plist version=\"1.0\">\n"
            << "<dict>\n"
            << "\t<key>DeviceCertificate</key>\n"
            << "\t<data>dGVzdGNlcnQ=</data>\n"
            << "\t<key>HostID</key>\n"
            << "\t<string>TEST-HOST-ID</string>\n"
            << "</dict>\n"
            << "</plist>\n";
        ofs.close();

        ios::transport::UsbmuxdClient client;
        client.SetPairRecordDirectory(test_dir);

        auto read_res = client.ReadPairRecord(test_udid);
        assert(read_res.is_ok());
        assert(read_res.value().find("DeviceCertificate") != std::string::npos);

        auto verify_res = client.VerifyDevicePairing(test_udid);
        assert(verify_res.is_ok());
        assert(verify_res.value() == true);
    }

    std::filesystem::remove_all(test_dir);
}

int main() {
    std::cout << "[TEST] Starting Mock usbmuxd server..." << std::endl;
    MockUsbmuxdServer server(kMockSocketPath);
    server.Start();

    // Give server thread a moment to bind and listen
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "[TEST] Running TestConnectAndEnumerate..." << std::endl;
    TestConnectAndEnumerate();

    std::cout << "[TEST] Running TestConnectToPortAndTunnel..." << std::endl;
    TestConnectToPortAndTunnel();

    std::cout << "[TEST] Running TestConnectToPortFailure..." << std::endl;
    TestConnectToPortFailure();

    std::cout << "[TEST] Running TestMoveSemantics..." << std::endl;
    TestMoveSemantics();

    std::cout << "[TEST] Running TestPairRecordValidation..." << std::endl;
    TestPairRecordValidation();

    server.Stop();
    std::cout << "[TEST] All usbmuxd client unit tests passed successfully!" << std::endl;
    return 0;
}
