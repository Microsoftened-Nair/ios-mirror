#include <libusb-1.0/libusb.h>
#include <iostream>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

#pragma pack(push, 1)

struct HandshakeHeader {
    uint32_t length;
    uint32_t magic;
    uint64_t correlation_id;
};

struct SyncClokPacket {
    HandshakeHeader header{32, 0x73796E63, 1}; // 'sync'
    uint32_t sub_type{0x636C6F6B};             // 'clok'
    uint32_t clock_id{1};
    uint64_t correlation_id{1};
};

struct SprpNeedPacket {
    HandshakeHeader header{28, 0x73707270, 2}; // 'sprp'
    uint32_t sub_type{0x6E656564};             // 'need'
    uint32_t target_fps{60};
    uint32_t quality{1};
};

#pragma pack(pop)

int main() {
    libusb_context* ctx = nullptr;
    libusb_init(&ctx);

    libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, 0x05ac, 0x12a8);
    if (!handle) {
        std::cerr << "Failed to open device\n";
        libusb_exit(ctx);
        return 1;
    }

    int cur_cfg = 0;
    libusb_get_configuration(handle, &cur_cfg);
    std::cout << "Active config: " << cur_cfg << "\n";
    if (cur_cfg != 5) {
        std::cout << "Switching to config 5...\n";
        int r = libusb_set_configuration(handle, 5);
        std::cout << "Set config 5 res: " << r << "\n";
    }

    int claim_res = libusb_claim_interface(handle, 2);
    std::cout << "Claim iface 2: " << claim_res << " (" << libusb_error_name(claim_res) << ")\n";

    libusb_clear_halt(handle, 0x86);
    libusb_clear_halt(handle, 0x05);

    std::atomic<bool> running{true};
    std::atomic<int> packets_received{0};

    // Receiver thread
    std::thread rx_thread([handle, &running, &packets_received]() {
        uint8_t buffer[65536];
        while (running.load()) {
            int transferred = 0;
            int r = libusb_bulk_transfer(handle, 0x86, buffer, sizeof(buffer), &transferred, 500);
            if (r == 0 && transferred > 0) {
                packets_received++;
                uint32_t len = 0;
                char magic[5] = {0};
                if (transferred >= 4) len = *reinterpret_cast<uint32_t*>(buffer);
                if (transferred >= 8) std::memcpy(magic, buffer + 4, 4);
                std::cout << "RX [" << packets_received << "]: len=" << len
                          << ", magic=" << magic << ", total_bytes=" << transferred << "\n";
            }
        }
    });

    // Step 1: Wait 500ms for incoming ping, then send ping back
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    struct {
        uint32_t length{16};
        uint32_t magic{0x70696E67}; // 'ping'
        uint64_t correlation_id{0};
    } ping;
    int transferred = 0;
    int r = libusb_bulk_transfer(handle, 0x05, reinterpret_cast<uint8_t*>(&ping), sizeof(ping), &transferred, 1000);
    std::cout << "Send PING: res=" << r << ", bytes=" << transferred << "\n";

    // Step 2: Send Clock Parameters (SYNC clok)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SyncClokPacket clok;
    r = libusb_bulk_transfer(handle, 0x05, reinterpret_cast<uint8_t*>(&clok), sizeof(clok), &transferred, 1000);
    std::cout << "Send CLOK: res=" << r << ", bytes=" << transferred << "\n";

    // Step 3: Send Stream Parameters (SPRP need)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SprpNeedPacket need;
    r = libusb_bulk_transfer(handle, 0x05, reinterpret_cast<uint8_t*>(&need), sizeof(need), &transferred, 1000);
    std::cout << "Send NEED: res=" << r << ", bytes=" << transferred << "\n";

    // Step 4: Send SPRP vfmt plist (Enable stream)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>Valuer</key>\n\t<true/>\n"
        "</dict>\n"
        "</plist>\n";

    uint32_t total_length = static_cast<uint32_t>(sizeof(HandshakeHeader) + sizeof(uint32_t) + plist.size());
    std::vector<uint8_t> packet_data(total_length);
    HandshakeHeader header{total_length, 0x73707270, 3}; // 'sprp'
    uint32_t sub_type = 0x76666D74;                      // 'vfmt'
    std::memcpy(packet_data.data(), &header, sizeof(header));
    std::memcpy(packet_data.data() + sizeof(header), &sub_type, sizeof(sub_type));
    std::memcpy(packet_data.data() + sizeof(header) + sizeof(sub_type), plist.data(), plist.size());

    r = libusb_bulk_transfer(handle, 0x05, packet_data.data(), packet_data.size(), &transferred, 2000);
    std::cout << "Send VFMT: res=" << r << ", bytes=" << transferred << "\n";

    // Wait 3 seconds to observe incoming stream packets
    std::cout << "Observing incoming stream for 3 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    running.store(false);
    if (rx_thread.joinable()) {
        rx_thread.join();
    }

    libusb_release_interface(handle, 2);
    libusb_close(handle);
    libusb_exit(ctx);
    std::cout << "Done! Total packets received: " << packets_received.load() << "\n";
    return 0;
}
