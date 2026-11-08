#include <libusb-1.0/libusb.h>
#include <iostream>
#include <cstring>

int main() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        std::cerr << "libusb_init failed\n";
        return 1;
    }

    libusb_device** list;
    ssize_t cnt = libusb_get_device_list(ctx, &list);
    std::cout << "Device count: " << cnt << "\n";

    libusb_device_handle* handle = nullptr;
    for (ssize_t i = 0; i < cnt; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == 0) {
            if (desc.idVendor == 0x05ac && desc.idProduct == 0x12a8) {
                int r = libusb_open(list[i], &handle);
                std::cout << "Found iPhone! libusb_open res: " << r << "\n";
                break;
            }
        }
    }

    if (!handle) {
        std::cerr << "Could not open handle\n";
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return 1;
    }

    int cur_cfg = -1;
    libusb_get_configuration(handle, &cur_cfg);
    std::cout << "Current configuration: " << cur_cfg << "\n";

    // Try detaching kernel drivers on all interfaces first
    libusb_set_auto_detach_kernel_driver(handle, 1);
    for (int iface = 0; iface < 5; ++iface) {
        int act = libusb_kernel_driver_active(handle, iface);
        std::cout << "Iface " << iface << " kernel driver active: " << act << "\n";
        if (act == 1) {
            int det = libusb_detach_kernel_driver(handle, iface);
            std::cout << "Detach iface " << iface << " res: " << det << " (" << libusb_error_name(det) << ")\n";
        }
    }

    // Try setting configuration to 5
    int set_res = libusb_set_configuration(handle, 5);
    std::cout << "libusb_set_configuration(5) res: " << set_res << " (" << libusb_error_name(set_res) << ")\n";

    libusb_close(handle);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}
