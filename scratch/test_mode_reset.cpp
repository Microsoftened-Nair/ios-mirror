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

    // Check GET_MODE
    unsigned char mode_data[16] = {0};
    libusb_control_transfer(handle, 0xC0, 0x45, 0, 0, mode_data, 4, 1000);
    std::cout << "Current GET_MODE (0x45): " << (int)mode_data[0] << ":" << (int)mode_data[1] << ":" << (int)mode_data[2] << "\n";

    // Reset to mode 1 (Initial mode)
    unsigned char resp[4] = {0};
    int r = libusb_control_transfer(handle, 0xC0, 0x52, 0, 1, resp, 1, 1000);
    std::cout << "SET_MODE (1): res=" << r << ", resp=" << (int)resp[0] << "\n";

    // Query GET_MODE again
    std::memset(mode_data, 0, sizeof(mode_data));
    libusb_control_transfer(handle, 0xC0, 0x45, 0, 0, mode_data, 4, 1000);
    std::cout << "After reset GET_MODE: " << (int)mode_data[0] << ":" << (int)mode_data[1] << ":" << (int)mode_data[2] << "\n";

    // Now switch to mode 2 (Valeria)
    std::memset(resp, 0, sizeof(resp));
    r = libusb_control_transfer(handle, 0xC0, 0x52, 0, 2, resp, 1, 1000);
    std::cout << "SET_MODE (2): res=" << r << ", resp=" << (int)resp[0] << "\n";

    // Query GET_MODE again
    std::memset(mode_data, 0, sizeof(mode_data));
    libusb_control_transfer(handle, 0xC0, 0x45, 0, 0, mode_data, 4, 1000);
    std::cout << "Final GET_MODE: " << (int)mode_data[0] << ":" << (int)mode_data[1] << ":" << (int)mode_data[2] << "\n";

    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
