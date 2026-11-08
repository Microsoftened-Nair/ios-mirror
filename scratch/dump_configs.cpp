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

    libusb_device* dev = libusb_get_device(handle);
    libusb_device_descriptor dev_desc;
    libusb_get_device_descriptor(dev, &dev_desc);

    std::cout << "bNumConfigurations: " << (int)dev_desc.bNumConfigurations << "\n";

    for (int i = 0; i < dev_desc.bNumConfigurations; ++i) {
        libusb_config_descriptor* cfg = nullptr;
        int r = libusb_get_config_descriptor(dev, i, &cfg);
        if (r == 0) {
            std::cout << "Config index " << i << ": bConfigurationValue=" << (int)cfg->bConfigurationValue
                      << ", bNumInterfaces=" << (int)cfg->bNumInterfaces << "\n";
            for (int iface_idx = 0; iface_idx < cfg->bNumInterfaces; ++iface_idx) {
                const auto& iface = cfg->interface[iface_idx];
                for (int a = 0; a < iface.num_altsetting; ++a) {
                    const auto& alt = iface.altsetting[a];
                    std::cout << "  Iface " << (int)alt.bInterfaceNumber
                              << " alt " << (int)alt.bAlternateSetting
                              << " class=" << (int)alt.bInterfaceClass
                              << " subclass=" << (int)alt.bInterfaceSubClass
                              << " proto=" << (int)alt.bInterfaceProtocol
                              << " endpoints=" << (int)alt.bNumEndpoints << "\n";
                }
            }
            libusb_free_config_descriptor(cfg);
        } else {
            std::cout << "Config index " << i << " failed: " << r << " (" << libusb_error_name(r) << ")\n";
        }
    }

    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}
