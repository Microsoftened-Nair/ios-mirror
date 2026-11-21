#pragma once

#include <cstdint>

namespace ios::session::wire {

/**
 * @brief Builds a CoreMedia FourCC as the big-endian integer of its (lowercase) ASCII characters,
 * e.g. 'p','i','n','g' -> 0x70696E67 (see protocol/coremedia_protocol.hpp).
 */
constexpr uint32_t MakeFourCC(char a, char b, char c, char d) noexcept {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(d));
}

/**
 * @brief Primary FourCC magic identifiers utilized across QuickTime CoreMedia handshake phases.
 */
enum class HandshakeMagic : uint32_t {
    Ping = MakeFourCC('p', 'i', 'n', 'g'), // 0x70696E67
    Sync = MakeFourCC('s', 'y', 'n', 'c'), // 0x73796E63
    Asyn = MakeFourCC('a', 's', 'y', 'n'), // 0x6173796E
    Sprp = MakeFourCC('s', 'p', 'r', 'p'), // 0x73707270
    Hbea = MakeFourCC('h', 'b', 'e', 'a'), // 0x68626561 (not seen in the QuickTime flow; unverified)
    Need = MakeFourCC('n', 'e', 'e', 'd'), // 0x6E656564
    Clok = MakeFourCC('c', 'l', 'o', 'k'), // 0x636C6F6B
    Time = MakeFourCC('t', 'i', 'm', 'e'), // 0x74696D65
    Cwpa = MakeFourCC('c', 'w', 'p', 'a'), // 0x63777061
    Afmt = MakeFourCC('a', 'f', 'm', 't'), // 0x61666D74
    Vfmt = MakeFourCC('v', 'f', 'm', 't'), // 0x76666D74
};

#pragma pack(push, 1)

/**
 * @brief 16-byte common binary envelope header prefixing all handshake communication packets.
 */
struct HandshakeHeader {
    uint32_t length{0};
    uint32_t magic{0};
    uint64_t correlation_id{0};

    constexpr HandshakeHeader() = default;
    constexpr HandshakeHeader(uint32_t len, uint32_t mag, uint64_t corr_id) noexcept
        : length(len), magic(mag), correlation_id(corr_id) {}
    constexpr HandshakeHeader(uint32_t len, HandshakeMagic mag, uint64_t corr_id) noexcept
        : length(len), magic(static_cast<uint32_t>(mag)), correlation_id(corr_id) {}
};

/**
 * @brief 8-byte header for CoreMedia Set Property (SPRP) requests and replies.
 */
struct SprpPayloadHeader {
    uint32_t sub_type{static_cast<uint32_t>(HandshakeMagic::Sprp)};
    uint32_t property_key{0};

    constexpr SprpPayloadHeader() = default;
    constexpr explicit SprpPayloadHeader(uint32_t prop_key) noexcept
        : sub_type(static_cast<uint32_t>(HandshakeMagic::Sprp)),
          property_key(prop_key) {}
    constexpr SprpPayloadHeader(uint32_t st, uint32_t prop_key) noexcept
        : sub_type(st),
          property_key(prop_key) {}
};

#pragma pack(pop)

// Compile-time wire structure layout verifications
static_assert(sizeof(HandshakeHeader) == 16, "HandshakeHeader must be 16 bytes");
static_assert(sizeof(SprpPayloadHeader) == 8, "SprpPayloadHeader must be 8 bytes");

// FourCC value verification assertions
static_assert(static_cast<uint32_t>(HandshakeMagic::Ping) == 0x70696E67, "Ping magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Sync) == 0x73796E63, "Sync magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Asyn) == 0x6173796E, "Asyn magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Sprp) == 0x73707270, "Sprp magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Hbea) == 0x68626561, "Hbea magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Need) == 0x6E656564, "Need magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Clok) == 0x636C6F6B, "Clok magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Time) == 0x74696D65, "Time magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Cwpa) == 0x63777061, "Cwpa magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Afmt) == 0x61666D74, "Afmt magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Vfmt) == 0x76666D74, "Vfmt magic mismatch");

} // namespace ios::session::wire
