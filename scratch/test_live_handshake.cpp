#include <libusb-1.0/libusb.h>
#include <iostream>
#include <cstring>
#include <vector>
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

    std::cout << "Resetting to mode 1...\n";
    unsigned char resp[4] = {0};
    libusb_control_transfer(handle, 0xC0, 0x52, 0, 1, resp, 1, 1000);
    libusb_set_configuration(handle, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    for (int i = 0; i < 5; ++i) {
        if (libusb_kernel_driver_active(handle, i) == 1) {
            libusb_detach_kernel_driver(handle, i);
        }
    }

    std::cout << "Activating mode 2 (Valeria)...\n";
    int r = libusb_control_transfer(handle, 0xC0, 0x52, 0, 2, resp, 1, 1000);
    std::cout << "SET_MODE(2) res: " << r << ", resp: " << (int)resp[0] << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "Setting config 5...\n";
    int set_cfg = libusb_set_configuration(handle, 5);
    std::cout << "Set config 5 res: " << set_cfg << " (" << libusb_error_name(set_cfg) << ")\n";

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

    std::cout << "Waiting for packets on 0x86...\n";
    uint8_t buffer[65536];
    for (int iter = 0; iter < 10; ++iter) {
        int transferred = 0;
        int r = libusb_bulk_transfer(handle, 0x86, buffer, sizeof(buffer), &transferred, 2000);
        if (r == 0 && transferred > 0) {
            uint32_t len = *reinterpret_cast<uint32_t*>(buffer);
            char magic[5] = {0};
            std::memcpy(magic, buffer + 4, 4);
            std::cout << "RX iter " << iter << ": len=" << len << ", magic=" << magic << ", bytes=" << transferred << "\n";

            // If PING:
            if (std::strcmp(magic, "ping") == 0) {
                uint8_t ping_reply[16] = {
                    0x10, 0x00, 0x00, 0x00,
                    'p', 'i', 'n', 'g',
                    0x00, 0x00, 0x00, 0x00,
                    0x01, 0x00, 0x00, 0x00
                };
                int tx = 0;
                int tx_res = libusb_bulk_transfer(handle, 0x05, ping_reply, sizeof(ping_reply), &tx, 1000);
                std::cout << "Sent PING reply: " << tx_res << " (" << libusb_error_name(tx_res) << "), bytes=" << tx << "\n";
            }
        } else {
            std::cout << "Read timeout/error: " << r << " (" << libusb_error_name(r) << ")\n";
            break;
        }
    }

    libusb_release_interface(handle, 2);
    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
