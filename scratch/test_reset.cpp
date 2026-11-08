#include <libusb-1.0/libusb.h>
#include <iostream>

int main() {
    libusb_context* ctx = nullptr;
    libusb_init(&ctx);

    libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, 0x05ac, 0x12a8);
    if (!handle) {
        std::cerr << "Failed to open device\n";
        libusb_exit(ctx);
        return 1;
    }

    std::cout << "Calling libusb_reset_device...\n";
    int ret = libusb_reset_device(handle);
    std::cout << "libusb_reset_device res: " << ret << " (" << libusb_error_name(ret) << ")\n";

    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
