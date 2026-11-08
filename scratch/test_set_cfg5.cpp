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

    int cur_cfg = -1;
    libusb_get_configuration(handle, &cur_cfg);
    std::cout << "Current configuration: " << cur_cfg << "\n";

    int r = libusb_set_configuration(handle, 5);
    std::cout << "libusb_set_configuration(5) res: " << r << " (" << libusb_error_name(r) << ")\n";

    libusb_get_configuration(handle, &cur_cfg);
    std::cout << "After switch, configuration: " << cur_cfg << "\n";

    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
