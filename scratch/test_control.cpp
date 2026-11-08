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

    std::cout << "Sending 0x40, 0x52, 0, 2...\n";
    int ret = libusb_control_transfer(handle, 0x40, 0x52, 0x0000, 0x0002, nullptr, 0, 2000);
    std::cout << "Control transfer res: " << ret << " (" << libusb_error_name(ret) << ")\n";

    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
