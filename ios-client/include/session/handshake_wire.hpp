#pragma once

#include <cstdint>

namespace ios::session::wire {

/**
 * @brief Constructs a 32-bit FourCC integer from four character literals in Little-Endian byte order.
 */
constexpr uint32_t MakeFourCC(char a, char b, char c, char d) noexcept {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

/**
 * @brief Primary FourCC magic identifiers utilized across QuickTime CoreMedia handshake phases.
 */
enum class HandshakeMagic : uint32_t {
    Ping = MakeFourCC('P', 'I', 'N', 'G'), // 0x474E4950
    Sync = MakeFourCC('S', 'Y', 'N', 'C'), // 0x434E5953
    Asyn = MakeFourCC('A', 'S', 'Y', 'N'), // 0x4E595341
    Sprp = MakeFourCC('S', 'P', 'R', 'P'), // 0x50525053
    Hbea = MakeFourCC('H', 'B', 'E', 'A'), // 0x41454248
    Need = MakeFourCC('N', 'E', 'E', 'D'), // 0x4445454E
    Clok = MakeFourCC('c', 'l', 'o', 'k'), // 0x6B6F6C63
    Time = MakeFourCC('T', 'I', 'M', 'E'), // 0x454D4954
    Cwpa = MakeFourCC('C', 'W', 'P', 'A'), // 0x41505743
    Afmt = MakeFourCC('A', 'F', 'M', 'T'), // 0x544D4641
    Vfmt = MakeFourCC('V', 'F', 'M', 'T'), // 0x544D4656
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
static_assert(static_cast<uint32_t>(HandshakeMagic::Ping) == 0x474E4950, "Ping magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Sync) == 0x434E5953, "Sync magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Asyn) == 0x4E595341, "Asyn magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Sprp) == 0x50525053, "Sprp magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Hbea) == 0x41454248, "Hbea magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Need) == 0x4445454E, "Need magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Clok) == 0x6B6F6C63, "Clok magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Time) == 0x454D4954, "Time magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Cwpa) == 0x41505743, "Cwpa magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Afmt) == 0x544D4641, "Afmt magic mismatch");
static_assert(static_cast<uint32_t>(HandshakeMagic::Vfmt) == 0x544D4656, "Vfmt magic mismatch");

} // namespace ios::session::wire
