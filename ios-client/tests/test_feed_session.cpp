#include "session/feed_session.hpp"
#ifndef IOS_MIRROR_CMAKE_BUILD
#define IOS_MIRROR_STANDALONE_TEST 1
#include "../src/protocol/stream_demuxer.cpp"
#include "../src/transport/usbmuxd_client.cpp"
#include "../src/session/feed_session.cpp"
#endif

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

const std::string kMockSocketPath = "/tmp/mock_feed_session_usbmuxd.sock";
const std::string kTestDir = "/tmp/mock_lockdown_feed_session_test";

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
        if (!running_) return;
        running_ = false;
        if (server_thread_.joinable()) server_thread_.join();
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }
        ::unlink(socket_path_.c_str());
    }

    void SetEmptyDevices(bool empty) { empty_devices_ = empty; }

private:
    void Run() {
        while (running_) {
            struct pollfd pfd{};
            pfd.fd = server_fd_;
            pfd.events = POLLIN;
            int pr = ::poll(&pfd, 1, 50);
            if (pr <= 0 || !running_) continue;

            struct sockaddr_un client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = ::accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &len);
            if (client_fd < 0) continue;

            while (running_) {
                ios::transport::wire::UsbmuxdHeader req_hdr{};
                ssize_t r = ::recv(client_fd, &req_hdr, sizeof(req_hdr), MSG_WAITALL);
                if (r <= 0) break;

                uint32_t payload_len = req_hdr.length - sizeof(req_hdr);
                std::string payload(payload_len, '\0');
                if (payload_len > 0) {
                    ssize_t pr_bytes = ::recv(client_fd, payload.data(), payload_len, MSG_WAITALL);
                    if (pr_bytes <= 0) break;
                }

                if (payload.find("<string>ListDevices</string>") != std::string::npos) {
                    std::string resp_plist;
                    if (empty_devices_) {
                        resp_plist =
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<plist version=\"1.0\"><dict><key>DeviceList</key><array></array></dict></plist>\n";
                    } else {
                        resp_plist =
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<plist version=\"1.0\">\n"
                            "<dict>\n"
                            "  <key>DeviceList</key>\n"
                            "  <array>\n"
                            "    <dict>\n"
                            "      <key>DeviceID</key><integer>1</integer>\n"
                            "      <key>MessageType</key><string>Attached</string>\n"
                            "      <key>Properties</key>\n"
                            "      <dict>\n"
                            "        <key>ConnectionSpeed</key><integer>480000000</integer>\n"
                            "        <key>ConnectionType</key><string>USB</string>\n"
                            "        <key>DeviceID</key><integer>1</integer>\n"
                            "        <key>LocationID</key><integer>65542</integer>\n"
                            "        <key>ProductID</key><integer>4776</integer>\n"
                            "        <key>SerialNumber</key><string>TEST-UDID-12345</string>\n"
                            "      </dict>\n"
                            "    </dict>\n"
                            "  </array>\n"
                            "</dict>\n"
                            "</plist>\n";
                    }
                    SendHeaderAndPayload(client_fd, req_hdr.tag, resp_plist);
                }
            }
            ::close(client_fd);
        }
    }

    std::string socket_path_;
    std::atomic<bool> running_{false};
    std::atomic<bool> empty_devices_{false};
    int server_fd_{-1};
    std::thread server_thread_;
};

class MockTransport : public ios::transport::IUsbTransport {
public:
    shared::Result<void, ios::transport::UsbTransportError> Initialize() override {
        initialized = true;
        return {};
    }

    shared::Result<void, ios::transport::UsbTransportError> OpenDevice(
        uint16_t vid, uint16_t pid, std::string_view serial) override {
        opened_vid = vid;
        opened_pid = pid;
        opened_serial = serial;
        return {};
    }

    shared::Result<void, ios::transport::UsbTransportError> TriggerCoreMediaMode() override {
        core_media_triggered = true;
        return {};
    }

    shared::Result<void, ios::transport::UsbTransportError> ClaimCoreMediaInterface() override {
        interface_claimed = true;
        return {};
    }

    shared::Result<void, ios::transport::UsbTransportError> StartStreaming(
        ios::transport::UsbDataCallback callback) override {
        streaming_active = true;
        data_callback = std::move(callback);
        return {};
    }

    void StopStreaming() noexcept override {
        streaming_active = false;
    }

    shared::Result<size_t, ios::transport::UsbTransportError> SendBulkData(
        shared::ReadOnlyBuffer data, uint32_t /*timeout_ms*/) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_data.emplace_back(data.begin(), data.end());
        return data.size();
    }

    bool IsConnected() const noexcept override {
        return true;
    }

    ios::transport::UsbEndpointPair GetActiveEndpoints() const noexcept override {
        return {0x86, 0x05};
    }

    void SimulateIncomingData(shared::ReadOnlyBuffer chunk) {
        if (streaming_active && data_callback) {
            data_callback(chunk);
        }
    }

    mutable std::mutex mutex_;
    bool initialized{false};
    uint16_t opened_vid{0};
    uint16_t opened_pid{0};
    std::string opened_serial;
    bool core_media_triggered{false};
    bool interface_claimed{false};
    bool streaming_active{false};
    ios::transport::UsbDataCallback data_callback;
    std::vector<std::vector<uint8_t>> sent_data;
};

void SetupValidPairRecord(const std::string& dir, const std::string& udid) {
    std::filesystem::create_directories(dir);
    std::string plist_path = dir + "/" + udid + ".plist";
    std::ofstream out(plist_path);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<plist version=\"1.0\">\n<dict>\n"
        << "  <key>DeviceCertificate</key><data>VGVzdENlcnQ=</data>\n"
        << "  <key>HostCertificate</key><data>VGVzdEhvc3RDZXJ0</data>\n"
        << "  <key>HostID</key><string>TEST-HOST-ID</string>\n"
        << "  <key>RootCertificate</key><data>VGVzdFJvb3RDZXJ0</data>\n"
        << "  <key>SystemBUID</key><string>TEST-SYSTEM-BUID</string>\n"
        << "</dict>\n</plist>\n";
    out.close();
}

void TestSuccessfulSessionLifecycle() {
    std::cout << "[Test] Running TestSuccessfulSessionLifecycle..." << std::endl;

    SetupValidPairRecord(kTestDir, "TEST-UDID-12345");

    ::setenv("USBMUXD_LOCKDOWN_PATH", kTestDir.c_str(), 1);

    auto mock_muxd = std::make_unique<ios::transport::UsbmuxdClient>(kMockSocketPath);
    auto mock_transport = std::make_unique<MockTransport>();
    auto* transport_ptr = mock_transport.get();

    ios::session::FeedSession session(std::move(mock_muxd), std::move(mock_transport));

    assert(session.GetState() == ios::session::SessionState::Disconnected);

    // 1. Connect
    auto conn_res = session.Connect();
    assert(conn_res.is_ok());
    assert(session.GetState() == ios::session::SessionState::ClaimingInterface);
    assert(session.GetResolvedUdid() == "TEST-UDID-12345");
    assert(transport_ptr->initialized);
    assert(transport_ptr->opened_vid == 0x05AC);
    assert(transport_ptr->opened_pid == 4776);
    assert(transport_ptr->core_media_triggered);
    assert(transport_ptr->interface_claimed);

    // 2. Start streaming & handshake
    bool frame_notified = false;
    ios::session::StreamConfiguration config{60, true, 1920, 1080};
    auto start_res = session.Start(config, [&frame_notified]() {
        frame_notified = true;
    });
    assert(start_res.is_ok());
    assert(session.GetState() == ios::session::SessionState::Streaming);
    assert(transport_ptr->streaming_active);
    assert(transport_ptr->sent_data.size() == 4); // PING, clok, need, vfmt

    // 3. Ingest a video sample via transport
    uint8_t nalu[] = {0x65, 0x88, 0x84, 0x00, 0x10}; // IDR
    uint32_t nalu_len_be = ios::protocol::wire::AvccHeader::from_nalu_length(sizeof(nalu)).nalu_length_be;

    std::vector<uint8_t> avcc_slice(sizeof(uint32_t) + sizeof(nalu));
    std::memcpy(avcc_slice.data(), &nalu_len_be, sizeof(uint32_t));
    std::memcpy(avcc_slice.data() + sizeof(uint32_t), nalu, sizeof(nalu));

    ios::protocol::wire::PacketHeader video_hdr;
    video_hdr.magic = static_cast<uint32_t>(ios::protocol::wire::PacketMagic::Asyn);
    video_hdr.correlation_id = 999;
    ios::protocol::wire::AsynFeedHeader video_feed{0, 0}; // video, keyframe
    ios::protocol::wire::CMTimeWire video_pts{1'000'000'000LL, 1'000'000'000, 1, 0};

    video_hdr.length = static_cast<uint32_t>(sizeof(video_hdr) + sizeof(video_feed) + sizeof(video_pts) + avcc_slice.size());
    std::vector<uint8_t> video_packet(video_hdr.length);
    size_t offset = 0;
    std::memcpy(video_packet.data() + offset, &video_hdr, sizeof(video_hdr)); offset += sizeof(video_hdr);
    std::memcpy(video_packet.data() + offset, &video_feed, sizeof(video_feed)); offset += sizeof(video_feed);
    std::memcpy(video_packet.data() + offset, &video_pts, sizeof(video_pts)); offset += sizeof(video_pts);
    std::memcpy(video_packet.data() + offset, avcc_slice.data(), avcc_slice.size());

    transport_ptr->SimulateIncomingData(shared::ReadOnlyBuffer(video_packet.data(), video_packet.size()));

    assert(frame_notified);

    // 4. Poll Sample (non-blocking)
    bool sample_polled = session.PollSample([](const ios::protocol::VideoSample& sample) {
        assert(sample.is_keyframe);
        assert(sample.size == 4 + 5); // Start code + 5 bytes nalu
        uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
        assert(std::memcmp(sample.data, start_code, 4) == 0);
    });
    assert(sample_polled);

    // 5. Stop (idempotency check)
    session.Stop();
    assert(session.GetState() == ios::session::SessionState::Disconnected);
    assert(!transport_ptr->streaming_active);

    // Second stop call must be a safe no-op
    session.Stop();
    assert(session.GetState() == ios::session::SessionState::Disconnected);

    std::cout << "[Test] TestSuccessfulSessionLifecycle passed." << std::endl;
}

void TestPairingFailure() {
    std::cout << "[Test] Running TestPairingFailure..." << std::endl;
    std::string unpair_dir = "/tmp/mock_unpaired_dir";
    std::filesystem::remove_all(unpair_dir);
    std::filesystem::create_directories(unpair_dir); // Empty directory, no pair record

    ::setenv("USBMUXD_LOCKDOWN_PATH", unpair_dir.c_str(), 1);
    auto mock_muxd = std::make_unique<ios::transport::UsbmuxdClient>(kMockSocketPath);
    auto mock_transport = std::make_unique<MockTransport>();

    ios::session::FeedSession session(std::move(mock_muxd), std::move(mock_transport));
    auto res = session.Connect();
    assert(res.is_err());
    assert(res.error() == ios::session::SessionError::PairingNotTrusted);
    assert(session.GetState() == ios::session::SessionState::Error);
    std::filesystem::remove_all(unpair_dir);
    std::cout << "[Test] TestPairingFailure passed." << std::endl;
}

void TestDeviceNotConnectedFailure(MockUsbmuxdServer& server) {
    std::cout << "[Test] Running TestDeviceNotConnectedFailure..." << std::endl;
    server.SetEmptyDevices(true);

    ::setenv("USBMUXD_LOCKDOWN_PATH", kTestDir.c_str(), 1);
    auto mock_muxd = std::make_unique<ios::transport::UsbmuxdClient>(kMockSocketPath);
    auto mock_transport = std::make_unique<MockTransport>();

    ios::session::FeedSession session(std::move(mock_muxd), std::move(mock_transport));
    auto res = session.Connect();
    assert(res.is_err());
    assert(res.error() == ios::session::SessionError::DeviceNotConnected);
    assert(session.GetState() == ios::session::SessionState::Error);

    server.SetEmptyDevices(false);
    std::cout << "[Test] TestDeviceNotConnectedFailure passed." << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Module 3 Task 3 Verification Harness ===" << std::endl;
    MockUsbmuxdServer server(kMockSocketPath);
    server.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TestSuccessfulSessionLifecycle();
    TestPairingFailure();
    TestDeviceNotConnectedFailure(server);

    server.Stop();
    std::filesystem::remove_all(kTestDir);
    std::cout << "=== All M3_T3 tests successfully passed! ===" << std::endl;
    return 0;
}
