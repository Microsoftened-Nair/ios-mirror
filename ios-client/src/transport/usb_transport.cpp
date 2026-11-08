#include "transport/usb_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#elif __has_include(<libusb.h>)
#include <libusb.h>
#endif

namespace ios::transport {

class LibUsbTransport::Impl {
public:
    static constexpr size_t kTransferPoolSize = 16;
    static constexpr size_t kTransferBufferSize = RawUsbBlock::kCapacity;

    struct TransferEntry {
        libusb_transfer* transfer{nullptr};
        uint8_t* buffer{nullptr};
        bool in_flight{false};
    };

    Impl() = default;

    ~Impl() {
        StopStreaming();
        CloseDevice();
        if (ctx_) {
            libusb_exit(ctx_);
            ctx_ = nullptr;
        }
    }

    shared::Result<void, UsbTransportError> Initialize() {
        if (ctx_) {
            return {};
        }
        int r = libusb_init(&ctx_);
        if (r < 0) {
            return UsbTransportError::InitFailed;
        }
        return {};
    }

    shared::Result<void, UsbTransportError> OpenDevice(
        uint16_t vid, uint16_t pid, std::string_view serial) {
        if (!ctx_) {
            auto init_res = Initialize();
            if (init_res.is_err()) {
                return init_res.error();
            }
        }

        CloseDevice();

        libusb_device** dev_list = nullptr;
        ssize_t count = libusb_get_device_list(ctx_, &dev_list);
        if (count < 0) {
            return UsbTransportError::DeviceNotFound;
        }

        libusb_device_handle* found_handle = nullptr;

        for (ssize_t i = 0; i < count; ++i) {
            libusb_device* dev = dev_list[i];
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(dev, &desc) != 0) {
                continue;
            }

            if (desc.idVendor != vid) {
                continue;
            }

            if (pid != 0 && desc.idProduct != pid) {
                continue;
            }

            libusb_device_handle* h = nullptr;
            int open_err = libusb_open(dev, &h);
            if (open_err != 0 || !h) {
                continue;
            }

            if (!serial.empty() && desc.iSerialNumber != 0) {
                unsigned char str_buf[256]{};
                int str_len = libusb_get_string_descriptor_ascii(
                    h, desc.iSerialNumber, str_buf, sizeof(str_buf) - 1);
                if (str_len > 0) {
                    std::string_view dev_serial(
                        reinterpret_cast<char*>(str_buf), static_cast<size_t>(str_len));
                    if (dev_serial == serial) {
                        found_handle = h;
                        break;
                    }
                }
                libusb_close(h);
            } else {
                found_handle = h;
                break;
            }
        }

        libusb_free_device_list(dev_list, 1);

        if (!found_handle) {
            return UsbTransportError::DeviceNotFound;
        }

        handle_ = found_handle;
        is_connected_ = true;
        return {};
    }

    shared::Result<void, UsbTransportError> TriggerCoreMediaMode() {
        if (!handle_) {
            return UsbTransportError::NotConnected;
        }

        // Primary: AV mode (wIndex = 0x0002)
        int ret = libusb_control_transfer(
            handle_,
            wire::UsbControlSetup::kVendorTypeOut,    // 0x40
            wire::UsbControlSetup::kRequestQuickTime,  // 0x52
            wire::UsbControlSetup::kValueDefault,     // 0x0000
            wire::UsbControlSetup::kIndexModeAv,      // 0x0002
            nullptr,
            wire::UsbControlSetup::kLengthZero,       // 0x0000
            wire::UsbControlSetup::kTimeoutMs         // 2000
        );

        if (ret < 0) {
            // Fallback: Legacy mode (wIndex = 0x0000)
            ret = libusb_control_transfer(
                handle_,
                wire::UsbControlSetup::kVendorTypeOut,
                wire::UsbControlSetup::kRequestQuickTime,
                wire::UsbControlSetup::kValueDefault,
                wire::UsbControlSetup::kIndexModeLegacy, // 0x0000
                nullptr,
                wire::UsbControlSetup::kLengthZero,
                wire::UsbControlSetup::kTimeoutMs
            );
            if (ret < 0) {
                return UsbTransportError::VendorControlFailed;
            }
        }

        return {};
    }

    shared::Result<void, UsbTransportError> ClaimCoreMediaInterface() {
        if (!handle_) {
            return UsbTransportError::NotConnected;
        }

        libusb_device* dev = libusb_get_device(handle_);
        if (!dev) {
            return UsbTransportError::DeviceNotFound;
        }

        libusb_config_descriptor* config = nullptr;
        int r = libusb_get_active_config_descriptor(dev, &config);
        if (r != 0 || !config) {
            r = libusb_get_config_descriptor(dev, 0, &config);
        }
        if (r != 0 || !config) {
            return UsbTransportError::InterfaceNotFound;
        }

        int target_iface = -1;
        int target_alt = 0;
        UsbEndpointPair eps{};

        for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
            const auto& iface = config->interface[i];
            for (int a = 0; a < iface.num_altsetting; ++a) {
                const auto& alt = iface.altsetting[a];
                if (alt.bInterfaceClass == wire::CoreMediaInterfaceSpec::kInterfaceClassVendor &&
                    alt.bInterfaceSubClass == wire::CoreMediaInterfaceSpec::kInterfaceSubClassAvFeed) {
                    target_iface = alt.bInterfaceNumber;
                    target_alt = alt.bAlternateSetting;

                    for (uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                        const auto& ep = alt.endpoint[e];
                        if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                            if ((ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                                eps.bulk_in = ep.bEndpointAddress;
                            } else {
                                eps.bulk_out = ep.bEndpointAddress;
                            }
                        }
                    }
                    break;
                }
            }
            if (target_iface != -1) {
                break;
            }
        }

        libusb_free_config_descriptor(config);

        if (target_iface == -1) {
            return UsbTransportError::InterfaceNotFound;
        }

        if (eps.bulk_in == 0 || eps.bulk_out == 0) {
            return UsbTransportError::EndpointNotFound;
        }

        // Kernel driver safety: check active driver and detach before claiming interface
        if (libusb_kernel_driver_active(handle_, target_iface) == 1) {
            int drv_res = libusb_detach_kernel_driver(handle_, target_iface);
            if (drv_res != 0 && drv_res != LIBUSB_ERROR_NOT_FOUND) {
                return UsbTransportError::KernelDriverDetachFailed;
            }
            kernel_driver_detached_ = true;
        }

        if (target_alt != 0) {
            libusb_set_interface_alt_setting(handle_, target_iface, target_alt);
        }

        int claim_res = libusb_claim_interface(handle_, target_iface);
        if (claim_res != 0) {
            return UsbTransportError::ClaimFailed;
        }

        claimed_interface_ = target_iface;
        endpoints_ = eps;
        return {};
    }

    shared::Result<void, UsbTransportError> StartStreaming(UsbDataCallback callback) {
        if (!handle_) {
            return UsbTransportError::NotConnected;
        }
        if (claimed_interface_ < 0 || endpoints_.bulk_in == 0) {
            return UsbTransportError::EndpointNotFound;
        }
        if (is_streaming_) {
            return UsbTransportError::StreamingAlreadyActive;
        }

        if (!AllocateTransferPool()) {
            return UsbTransportError::InitFailed;
        }

        data_callback_ = std::move(callback);
        raw_callback_ = nullptr;
        is_streaming_ = true;
        active_transfers_ = 0;

        // Start background event pump on dedicated jthread using C++20 stop_token
        io_thread_ = std::jthread([this](std::stop_token st) {
            IoWorker(st);
        });

        // Submit all 16 ring buffer transfers
        for (size_t i = 0; i < kTransferPoolSize; ++i) {
            int r = libusb_submit_transfer(transfer_pool_[i].transfer);
            if (r == 0) {
                transfer_pool_[i].in_flight = true;
                active_transfers_++;
            } else {
                transfer_pool_[i].in_flight = false;
            }
        }

        if (active_transfers_ == 0) {
            StopStreaming();
            return UsbTransportError::TransferFailed;
        }

        return {};
    }

    shared::Result<void, UsbTransportError> StartStreaming(RawBlockCallback callback) {
        raw_callback_ = std::move(callback);
        return StartStreaming(UsbDataCallback([this](shared::ReadOnlyBuffer buf) {
            if (raw_callback_) {
                RawUsbBlock block;
                size_t copy_len = std::min(buf.size(), RawUsbBlock::kCapacity);
                std::memcpy(block.data, buf.data(), copy_len);
                block.size = copy_len;
                block.sequence_id = sequence_counter_++;
                raw_callback_(block);
            }
        }));
    }

    void StopStreaming() noexcept {
        if (!is_streaming_ && active_transfers_ == 0) {
            return;
        }

        is_streaming_ = false;

        // Request worker thread stop
        io_thread_.request_stop();

        // Cancel all in-flight transfers
        for (auto& entry : transfer_pool_) {
            if (entry.in_flight && entry.transfer) {
                libusb_cancel_transfer(entry.transfer);
            }
        }

        // Teardown safety: drain all pending callbacks before freeing pool memory
        while (active_transfers_ > 0 && ctx_) {
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000; // 10 ms
            libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
        }

        if (io_thread_.joinable()) {
            io_thread_.join();
        }

        FreeTransferPool();
    }

    shared::Result<size_t, UsbTransportError> SendBulkData(
        shared::ReadOnlyBuffer data, uint32_t timeout_ms) {
        if (!handle_) {
            return UsbTransportError::NotConnected;
        }
        if (endpoints_.bulk_out == 0) {
            return UsbTransportError::EndpointNotFound;
        }

        int transferred = 0;
        int ret = libusb_bulk_transfer(
            handle_,
            endpoints_.bulk_out,
            const_cast<uint8_t*>(data.data()),
            static_cast<int>(data.size()),
            &transferred,
            timeout_ms
        );

        if (ret != 0) {
            if (ret == LIBUSB_ERROR_TIMEOUT) {
                return UsbTransportError::Timeout;
            }
            if (ret == LIBUSB_ERROR_PIPE) {
                return UsbTransportError::PipeError;
            }
            if (ret == LIBUSB_ERROR_NO_DEVICE) {
                is_connected_ = false;
                return UsbTransportError::NotConnected;
            }
            return UsbTransportError::TransferFailed;
        }

        return static_cast<size_t>(transferred);
    }

    bool IsConnected() const noexcept {
        return handle_ != nullptr && is_connected_;
    }

    UsbEndpointPair GetActiveEndpoints() const noexcept {
        return endpoints_;
    }

    void CloseDevice() noexcept {
        StopStreaming();
        if (handle_) {
            if (claimed_interface_ >= 0) {
                libusb_release_interface(handle_, claimed_interface_);
                if (kernel_driver_detached_) {
                    libusb_attach_kernel_driver(handle_, claimed_interface_);
                    kernel_driver_detached_ = false;
                }
                claimed_interface_ = -1;
            }
            libusb_close(handle_);
            handle_ = nullptr;
        }
        is_connected_ = false;
        endpoints_ = {};
    }

private:
    bool AllocateTransferPool() {
        transfer_pool_.resize(kTransferPoolSize);
        for (size_t i = 0; i < kTransferPoolSize; ++i) {
            transfer_pool_[i].buffer = static_cast<uint8_t*>(std::aligned_alloc(64, kTransferBufferSize));
            if (!transfer_pool_[i].buffer) {
                FreeTransferPool();
                return false;
            }
            std::memset(transfer_pool_[i].buffer, 0, kTransferBufferSize);

            transfer_pool_[i].transfer = libusb_alloc_transfer(0);
            if (!transfer_pool_[i].transfer) {
                FreeTransferPool();
                return false;
            }

            libusb_fill_bulk_transfer(
                transfer_pool_[i].transfer,
                handle_,
                endpoints_.bulk_in,
                transfer_pool_[i].buffer,
                static_cast<int>(kTransferBufferSize),
                &Impl::TransferCallback,
                this,
                0 // 0 = unlimited timeout for continuous streaming
            );
            transfer_pool_[i].in_flight = false;
        }
        return true;
    }

    void FreeTransferPool() {
        for (auto& entry : transfer_pool_) {
            if (entry.transfer) {
                libusb_free_transfer(entry.transfer);
                entry.transfer = nullptr;
            }
            if (entry.buffer) {
                std::free(entry.buffer);
                entry.buffer = nullptr;
            }
            entry.in_flight = false;
        }
        transfer_pool_.clear();
    }

    void IoWorker(std::stop_token stop_token) {
        while (!stop_token.stop_requested() && is_streaming_) {
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 50000; // 50 ms slices
            int r = libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
            if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
                if (r == LIBUSB_ERROR_NO_DEVICE) {
                    is_connected_ = false;
                    is_streaming_ = false;
                    break;
                }
            }
        }
    }

    static void LIBUSB_CALL TransferCallback(struct libusb_transfer* transfer) {
        auto* self = static_cast<Impl*>(transfer->user_data);
        if (self) {
            self->HandleTransferCompletion(transfer);
        }
    }

    void HandleTransferCompletion(struct libusb_transfer* transfer) {
        if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
            if (transfer->actual_length > 0 && data_callback_) {
                shared::ReadOnlyBuffer buf(transfer->buffer, static_cast<size_t>(transfer->actual_length));
                data_callback_(buf);
            }

            // Resubmit immediately if streaming is active
            if (is_streaming_ && !io_thread_.get_stop_token().stop_requested()) {
                int r = libusb_submit_transfer(transfer);
                if (r == 0) {
                    return;
                }
            }
        } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
            is_connected_ = false;
            is_streaming_ = false;
        } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
            // Cancelled during teardown
        } else {
            // Other error condition, retry if streaming
            if (is_streaming_ && !io_thread_.get_stop_token().stop_requested()) {
                int r = libusb_submit_transfer(transfer);
                if (r == 0) {
                    return;
                }
            }
        }

        // Mark transfer as no longer in-flight
        for (auto& entry : transfer_pool_) {
            if (entry.transfer == transfer) {
                entry.in_flight = false;
                break;
            }
        }
        active_transfers_--;
    }

    libusb_context* ctx_{nullptr};
    libusb_device_handle* handle_{nullptr};
    int claimed_interface_{-1};
    bool kernel_driver_detached_{false};
    bool is_connected_{false};
    UsbEndpointPair endpoints_{};

    std::vector<TransferEntry> transfer_pool_;
    std::atomic<int> active_transfers_{0};
    std::atomic<bool> is_streaming_{false};
    std::jthread io_thread_;
    UsbDataCallback data_callback_;
    RawBlockCallback raw_callback_;
    uint64_t sequence_counter_{0};
};

LibUsbTransport::LibUsbTransport()
    : impl_(std::make_unique<Impl>()) {}

LibUsbTransport::~LibUsbTransport() = default;

LibUsbTransport::LibUsbTransport(LibUsbTransport&& other) noexcept = default;

LibUsbTransport& LibUsbTransport::operator=(LibUsbTransport&& other) noexcept = default;

shared::Result<void, UsbTransportError> LibUsbTransport::Initialize() {
    return impl_->Initialize();
}

shared::Result<void, UsbTransportError> LibUsbTransport::OpenDevice(
    uint16_t vid, uint16_t pid, std::string_view serial) {
    return impl_->OpenDevice(vid, pid, serial);
}

shared::Result<void, UsbTransportError> LibUsbTransport::TriggerCoreMediaMode() {
    return impl_->TriggerCoreMediaMode();
}

shared::Result<void, UsbTransportError> LibUsbTransport::ClaimCoreMediaInterface() {
    return impl_->ClaimCoreMediaInterface();
}

shared::Result<void, UsbTransportError> LibUsbTransport::StartStreaming(UsbDataCallback callback) {
    return impl_->StartStreaming(std::move(callback));
}

shared::Result<void, UsbTransportError> LibUsbTransport::StartStreaming(RawBlockCallback callback) {
    return impl_->StartStreaming(std::move(callback));
}

void LibUsbTransport::StopStreaming() noexcept {
    impl_->StopStreaming();
}

shared::Result<size_t, UsbTransportError> LibUsbTransport::SendBulkData(
    shared::ReadOnlyBuffer data, uint32_t timeout_ms) {
    return impl_->SendBulkData(data, timeout_ms);
}

bool LibUsbTransport::IsConnected() const noexcept {
    return impl_->IsConnected();
}

UsbEndpointPair LibUsbTransport::GetActiveEndpoints() const noexcept {
    return impl_->GetActiveEndpoints();
}

void LibUsbTransport::CloseDevice() noexcept {
    impl_->CloseDevice();
}

} // namespace ios::transport
