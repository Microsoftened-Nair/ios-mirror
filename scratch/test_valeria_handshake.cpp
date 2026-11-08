#include <libusb-1.0/libusb.h>
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

#pragma pack(push, 1)
struct PingPacket {
    uint32_t length{16};
    uint32_t magic{0x70696E67}; // 'ping'
    uint64_t correlation_id{0};
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

    // Claim Interface 2
    int claim_res = libusb_claim_interface(handle, 2);
    std::cout << "Claim iface 2 res: " << claim_res << " (" << libusb_error_name(claim_res) << ")\n";

    // Clear halt on 0x86 and 0x05
    int clr_in = libusb_clear_halt(handle, 0x86);
    int clr_out = libusb_clear_halt(handle, 0x05);
    std::cout << "Clear halt in (0x86): " << clr_in << ", out (0x05): " << clr_out << "\n";

    // Check if device sends a ping or if we can read from 0x86
    std::thread reader([handle]() {
        uint8_t rx_buf[4096];
        int transferred = 0;
        std::cout << "Reader thread waiting on 0x86...\n";
        int r = libusb_bulk_transfer(handle, 0x86, rx_buf, sizeof(rx_buf), &transferred, 3000);
        std::cout << "Reader result: " << r << " (" << libusb_error_name(r) << "), bytes=" << transferred << "\n";
        if (transferred >= 8) {
            uint32_t len = *reinterpret_cast<uint32_t*>(rx_buf);
            char magic[5] = {0};
            std::memcpy(magic, rx_buf + 4, 4);
            std::cout << "Received packet: len=" << len << ", magic=" << magic << "\n";
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Send PING packet to 0x05
    PingPacket ping;
    int transferred = 0;
    std::cout << "Sending PING to 0x05...\n";
    int r = libusb_bulk_transfer(handle, 0x05, reinterpret_cast<uint8_t*>(&ping), sizeof(ping), &transferred, 2000);
    std::cout << "Send PING res: " << r << " (" << libusb_error_name(r) << "), sent=" << transferred << "\n";

    if (reader.joinable()) {
        reader.join();
    }

    libusb_release_interface(handle, 2);
    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
