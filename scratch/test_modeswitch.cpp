#include <libusb-1.0/libusb.h>
#include <iostream>
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

    // First, let's query the mode with 0x45 (GET_MODE)
    // bRequestType = 0xC0, bRequest = 0x45, wValue = 0, wIndex = 0, wLength = 4
    unsigned char mode_data[16] = {0};
    int r_get = libusb_control_transfer(handle, 0xC0, 0x45, 0, 0, mode_data, 4, 1000);
    std::cout << "GET_MODE (0x45) res: " << r_get << ", data: ";
    for (int i = 0; i < 4; ++i) std::cout << (int)mode_data[i] << " ";
    std::cout << "\n";

    // Now send SET_MODE (0x52) with wIndex = 2 (Valeria)
    // bRequestType = 0xC0, bRequest = 0x52, wValue = 0, wIndex = 2, wLength = 1
    unsigned char set_resp[4] = {0};
    int r_set = libusb_control_transfer(handle, 0xC0, 0x52, 0, 2, set_resp, 1, 1000);
    std::cout << "SET_MODE (0x52, wIndex=2) res: " << r_set << ", resp: " << (int)set_resp[0] << "\n";

    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
