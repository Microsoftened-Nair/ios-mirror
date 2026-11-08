#include <libusb-1.0/libusb.h>
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

int main() {
    libusb_context* ctx = nullptr;
    libusb_init(&ctx);

    libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, 0x05ac, 0x12a8);
    if (!handle) {
        std::cerr << "Failed to open device\n";
        libusb_exit(ctx);
        return 1;
    }

    libusb_set_auto_detach_kernel_driver(handle, 1);
    for (int i = 0; i < 5; ++i) {
        if (libusb_kernel_driver_active(handle, i) == 1) {
            libusb_detach_kernel_driver(handle, i);
        }
    }

    int claim_res = libusb_claim_interface(handle, 2);
    std::cout << "Claim iface 2: " << claim_res << " (" << libusb_error_name(claim_res) << ")\n";
    if (claim_res != 0) {
        libusb_close(handle);
        libusb_exit(ctx);
        return 1;
    }

    libusb_clear_halt(handle, 0x86);
    libusb_clear_halt(handle, 0x05);

    // Read first from 0x86
    uint8_t rx_buf[4096] = {0};
    int rx = 0;
    std::cout << "Waiting to read initial packet from 0x86 (up to 5s)...\n";
    int rx_res = libusb_bulk_transfer(handle, 0x86, rx_buf, sizeof(rx_buf), &rx, 5000);
    std::cout << "Read res: " << rx_res << " (" << libusb_error_name(rx_res) << "), bytes=" << rx << "\n";

    if (rx >= 8) {
        uint32_t len = *reinterpret_cast<uint32_t*>(rx_buf);
        char magic[5] = {0};
        std::memcpy(magic, rx_buf + 4, 4);
        std::cout << "Received initial packet: len=" << len << ", magic=" << magic << "\n";

        // Reply with ping to 0x05
        uint8_t ping_reply[16] = {
            0x10, 0x00, 0x00, 0x00,
            'p', 'i', 'n', 'g',
            0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00
        };
        int tx = 0;
        std::cout << "Sending PING reply to 0x05...\n";
        int tx_res = libusb_bulk_transfer(handle, 0x05, ping_reply, sizeof(ping_reply), &tx, 1000);
        std::cout << "Send PING reply res: " << tx_res << " (" << libusb_error_name(tx_res) << "), bytes=" << tx << "\n";
    }

    libusb_release_interface(handle, 2);
    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
