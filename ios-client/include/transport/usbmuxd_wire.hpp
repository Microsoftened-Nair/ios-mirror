#pragma once

#include <cstdint>

namespace ios::transport::wire {

/**
 * @brief Usbmuxd protocol versions.
 */
enum class UsbmuxdVersion : uint32_t {
    Binary = 0,
    Plist = 1,
};

/**
 * @brief Usbmuxd message types across binary and plist protocols.
 */
enum class UsbmuxdMessageType : uint32_t {
    Result = 1,
    Connect = 2,
    Listen = 3,
    DeviceAdd = 4,
    DeviceRemove = 5,
    DevicePaired = 6,
    PlistWrapper = 8,
};

/**
 * @brief Usbmuxd result response status codes (binary protocol).
 */
enum class UsbmuxdResultCode : uint32_t {
    Ok = 0,
    BadCommand = 1,
    BadDevice = 2,
    ConnectionRefused = 3,
    Unknown = 4,
    BadVersion = 5,
};

#pragma pack(push, 1)

/**
 * @brief 16-byte fixed-size header prefixing every usbmuxd packet.
 * All integer fields are little-endian on Linux/x86_64 host.
 */
struct UsbmuxdHeader {
    uint32_t length{0};
    uint32_t version{0};
    uint32_t message{0};
    uint32_t tag{0};

    constexpr UsbmuxdHeader() = default;

    constexpr UsbmuxdHeader(uint32_t len, uint32_t ver, uint32_t msg, uint32_t t) noexcept
        : length(len), version(ver), message(msg), tag(t) {}

    constexpr UsbmuxdHeader(uint32_t len, UsbmuxdVersion ver, UsbmuxdMessageType msg, uint32_t t) noexcept
        : length(len),
          version(static_cast<uint32_t>(ver)),
          message(static_cast<uint32_t>(msg)),
          tag(t) {}
};

/**
 * @brief USB control transfer setup packet and Apple vendor switch constants.
 */
struct UsbControlSetup {
    uint8_t bmRequestType{0};
    uint8_t bRequest{0};
    uint16_t wValue{0};
    uint16_t wIndex{0};
    uint16_t wLength{0};

    // Apple vendor switch constants
    static constexpr uint8_t kVendorTypeOut = 0x40;
    static constexpr uint8_t kRequestQuickTime = 0x52;
    static constexpr uint16_t kValueDefault = 0x0000;
    static constexpr uint16_t kIndexModeAv = 0x0002;
    static constexpr uint16_t kIndexModeLegacy = 0x0000;
    static constexpr uint16_t kLengthZero = 0x0000;
    static constexpr uint32_t kTimeoutMs = 2000;
};

#pragma pack(pop)

// Compile-time layout verification
static_assert(sizeof(UsbmuxdHeader) == 16, "UsbmuxdHeader must be exactly 16 bytes");
static_assert(sizeof(UsbControlSetup) == 8, "UsbControlSetup must be exactly 8 bytes");

/**
 * @brief Matching criteria for Apple CoreMedia / QuickTime AV streaming interface.
 */
struct CoreMediaInterfaceSpec {
    static constexpr uint16_t kAppleVendorId = 0x05AC;
    static constexpr uint8_t kInterfaceClassVendor = 0xFF;
    static constexpr uint8_t kInterfaceSubClassAvFeed = 0x2A;
    static constexpr uint8_t kInterfaceProtocolFeed = 0xFF;

    static constexpr bool matches(uint16_t vendor_id,
                                  uint8_t iface_class,
                                  uint8_t iface_subclass,
                                  uint8_t iface_protocol) noexcept {
        return vendor_id == kAppleVendorId &&
               iface_class == kInterfaceClassVendor &&
               iface_subclass == kInterfaceSubClassAvFeed &&
               iface_protocol == kInterfaceProtocolFeed;
    }
};

} // namespace ios::transport::wire
