#pragma once

#include <cstdint>

namespace ios::protocol::wire {

/**
 * @brief Builds a CoreMedia FourCC as the big-endian integer of its (lowercase) ASCII characters,
 * e.g. 'p','i','n','g' -> 0x70696E67.
 *
 * The device writes these 32-bit values little-endian, so on the wire the bytes appear reversed
 * ("gnip"). Earlier versions used uppercase letters composed little-endian ("PING" -> 0x474E4950),
 * which never matches what the iPhone sends.
 */
constexpr uint32_t MakeFourCC(char a, char b, char c, char d) noexcept {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(d));
}

/**
 * @brief Primary CoreMedia packet magic values.
 */
enum class PacketMagic : uint32_t {
    Ping = MakeFourCC('p', 'i', 'n', 'g'), // 0x70696E67
    Sync = MakeFourCC('s', 'y', 'n', 'c'), // 0x73796E63
    Asyn = MakeFourCC('a', 's', 'y', 'n'), // 0x6173796E
    Sprp = MakeFourCC('s', 'p', 'r', 'p'), // 0x73707270
    Hbea = MakeFourCC('h', 'b', 'e', 'a'), // 0x68626561 (not seen in the QuickTime flow; unverified)
    Rply = MakeFourCC('r', 'p', 'l', 'y'), // 0x72706C79 (Sync Reply)
};

/**
 * @brief CoreMedia message subtype magic values.
 */
enum class SubTypeMagic : uint32_t {
    Cwpa = MakeFourCC('c', 'w', 'p', 'a'), // 0x63777061 (Clock Wait Packet Acknowledge)
    Cvrp = MakeFourCC('c', 'v', 'r', 'p'), // 0x63767270 (CoreMedia Video Request Packet)
    Time = MakeFourCC('t', 'i', 'm', 'e'), // 0x74696D65 (Clock Time Synchronization)
    Feed = MakeFourCC('f', 'e', 'e', 'd'), // 0x66656564 (Asynchronous Media Stream Feed)
    Afmt = MakeFourCC('a', 'f', 'm', 't'), // 0x61666D74 (Audio Format Descriptor)
    Vfmt = MakeFourCC('v', 'f', 'm', 't'), // 0x76666D74 (Video Format Descriptor)
    Need = MakeFourCC('n', 'e', 'e', 'd'), // 0x6E656564 (Buffer Request / Flow Control)
    Clok = MakeFourCC('c', 'l', 'o', 'k'), // 0x636C6F6B (Clock Reference)
    Hpd1 = MakeFourCC('h', 'p', 'd', '1'), // 0x68706431 (Host Packet Display 1)
    Hpa1 = MakeFourCC('h', 'p', 'a', '1'), // 0x68706131 (Host Packet Audio 1)
};

/**
 * @brief Flags for Apple CMTime representations.
 */
enum class CMTimeFlags : uint32_t {
    Valid = 1u << 0,
    HasBeenRounded = 1u << 1,
    PositiveInfinity = 1u << 2,
    NegativeInfinity = 1u << 3,
    Indefinite = 1u << 4,
};

#pragma pack(push, 1)

/**
 * @brief 16-byte common envelope header prefixing all CoreMedia packets.
 */
struct PacketHeader {
    uint32_t length{0};
    uint32_t magic{0};
    uint64_t correlation_id{0};

    constexpr PacketHeader() = default;
    constexpr PacketHeader(uint32_t len, uint32_t mag, uint64_t corr_id) noexcept
        : length(len), magic(mag), correlation_id(corr_id) {}
    constexpr PacketHeader(uint32_t len, PacketMagic mag, uint64_t corr_id) noexcept
        : length(len), magic(static_cast<uint32_t>(mag)), correlation_id(corr_id) {}
};

/**
 * @brief 24-byte wire representation of Apple CoreMedia CMTime structure.
 */
struct CMTimeWire {
    int64_t value{0};
    int32_t timescale{0};
    uint32_t flags{0};
    int64_t epoch{0};

    constexpr CMTimeWire() = default;
    constexpr CMTimeWire(int64_t val, int32_t scale, uint32_t fl = 1, int64_t ep = 0) noexcept
        : value(val), timescale(scale), flags(fl), epoch(ep) {}

    constexpr double to_seconds() const noexcept {
        return timescale != 0 ? static_cast<double>(value) / static_cast<double>(timescale) : 0.0;
    }

    constexpr bool is_valid() const noexcept {
        return (flags & static_cast<uint32_t>(CMTimeFlags::Valid)) != 0;
    }
};

/**
 * @brief 16-byte payload for SYNC CWPA clock synchronization requests.
 */
struct SyncCwpaPayload {
    uint32_t sub_type{static_cast<uint32_t>(SubTypeMagic::Cwpa)};
    uint32_t clock_id{0};
    uint64_t correlation_id{0};

    constexpr SyncCwpaPayload() = default;
    constexpr SyncCwpaPayload(uint32_t clk_id, uint64_t corr_id) noexcept
        : sub_type(static_cast<uint32_t>(SubTypeMagic::Cwpa)),
          clock_id(clk_id),
          correlation_id(corr_id) {}
};

/**
 * @brief 28-byte clock reference reply packet (RPLY) used for CWPA and CVRP replies.
 */
struct SyncClockRefReplyPacket {
    uint32_t length{28};
    uint32_t magic{static_cast<uint32_t>(PacketMagic::Rply)};
    uint64_t correlation_id{0};
    uint32_t status{0};
    uint64_t clock_ref{0};

    constexpr SyncClockRefReplyPacket() = default;
    constexpr SyncClockRefReplyPacket(uint64_t corr_id, uint64_t clk_ref, uint32_t st = 0) noexcept
        : length(sizeof(SyncClockRefReplyPacket)),
          magic(static_cast<uint32_t>(PacketMagic::Rply)),
          correlation_id(corr_id),
          status(st),
          clock_ref(clk_ref) {}
};

/**
 * @brief 20-byte ASYN NEED flow control packet sent to device to request video frames.
 */
struct AsynNeedPacket {
    uint32_t length{20};
    uint32_t magic{static_cast<uint32_t>(PacketMagic::Asyn)};
    uint64_t clock_ref{0};
    uint32_t sub_type{static_cast<uint32_t>(SubTypeMagic::Need)};

    constexpr AsynNeedPacket() = default;
    constexpr explicit AsynNeedPacket(uint64_t clk_ref) noexcept
        : length(sizeof(AsynNeedPacket)),
          magic(static_cast<uint32_t>(PacketMagic::Asyn)),
          clock_ref(clk_ref),
          sub_type(static_cast<uint32_t>(SubTypeMagic::Need)) {}
};

/**
 * @brief 24-byte reply packet sent by host acknowledging CWPA clock synchronization.
 */
struct SyncCwpaReplyPacket {
    PacketHeader header{};
    uint64_t host_nanos{0};

    constexpr SyncCwpaReplyPacket() = default;
    constexpr SyncCwpaReplyPacket(uint64_t corr_id, uint64_t nanos) noexcept
        : header(sizeof(SyncCwpaReplyPacket), PacketMagic::Sync, corr_id),
          host_nanos(nanos) {}
};

/**
 * @brief 16-byte payload for SYNC TIME clock synchronization requests.
 */
struct SyncTimePayload {
    uint32_t sub_type{static_cast<uint32_t>(SubTypeMagic::Time)};
    uint32_t clock_id{0};
    uint64_t device_time_raw{0};

    constexpr SyncTimePayload() = default;
    constexpr SyncTimePayload(uint32_t clk_id, uint64_t dev_time) noexcept
        : sub_type(static_cast<uint32_t>(SubTypeMagic::Time)),
          clock_id(clk_id),
          device_time_raw(dev_time) {}
};

/**
 * @brief 48-byte reply packet returning host timestamp correlation for SYNC TIME.
 */
struct SyncTimeReplyPacket {
    PacketHeader header{};
    uint32_t sub_type{static_cast<uint32_t>(SubTypeMagic::Time)};
    uint32_t clock_id{0};
    uint64_t device_time_raw{0};
    uint64_t host_nanos{0};
    uint64_t reserved{0};

    constexpr SyncTimeReplyPacket() = default;
    constexpr SyncTimeReplyPacket(uint64_t corr_id,
                                  uint32_t clk_id,
                                  uint64_t dev_time,
                                  uint64_t nanos) noexcept
        : header(sizeof(SyncTimeReplyPacket), PacketMagic::Sync, corr_id),
          sub_type(static_cast<uint32_t>(SubTypeMagic::Time)),
          clock_id(clk_id),
          device_time_raw(dev_time),
          host_nanos(nanos),
          reserved(0) {}
};

/**
 * @brief 16-byte header preceding asynchronous CoreMedia media sample buffers (ASYN 'feed').
 */
struct AsynFeedHeader {
    uint32_t sub_type{static_cast<uint32_t>(SubTypeMagic::Feed)};
    uint32_t stream_index{0};
    uint64_t sample_flags{0};

    constexpr AsynFeedHeader() = default;
    constexpr AsynFeedHeader(uint32_t stream_idx, uint64_t flags) noexcept
        : sub_type(static_cast<uint32_t>(SubTypeMagic::Feed)),
          stream_index(stream_idx),
          sample_flags(flags) {}
};

/**
 * @brief 4-byte Big-Endian NALU length header used in AVCC H.264 streams.
 */
struct AvccHeader {
    uint32_t nalu_length_be{0};

    constexpr AvccHeader() = default;
    constexpr explicit AvccHeader(uint32_t be_len) noexcept : nalu_length_be(be_len) {}

    /**
     * @brief Decodes the Big-Endian length field into host integer byte order.
     */
    constexpr uint32_t get_nalu_length() const noexcept {
        return ((nalu_length_be & 0xFF000000u) >> 24) |
               ((nalu_length_be & 0x00FF0000u) >> 8) |
               ((nalu_length_be & 0x0000FF00u) << 8) |
               ((nalu_length_be & 0x000000FFu) << 24);
    }

    /**
     * @brief Encodes host integer byte order into Big-Endian wire representation.
     */
    static constexpr AvccHeader from_nalu_length(uint32_t len) noexcept {
        uint32_t be = ((len & 0xFF000000u) >> 24) |
                      ((len & 0x00FF0000u) >> 8) |
                      ((len & 0x0000FF00u) << 8) |
                      ((len & 0x000000FFu) << 24);
        return AvccHeader(be);
    }
};

#pragma pack(pop)

// Compile-time wire structure size assertions
static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be 16 bytes");
static_assert(sizeof(CMTimeWire) == 24, "CMTimeWire must be 24 bytes");
static_assert(sizeof(SyncCwpaPayload) == 16, "SyncCwpaPayload must be 16 bytes");
static_assert(sizeof(SyncCwpaReplyPacket) == 24, "SyncCwpaReplyPacket must be 24 bytes");
static_assert(sizeof(SyncTimePayload) == 16, "SyncTimePayload must be 16 bytes");
static_assert(sizeof(SyncTimeReplyPacket) == 48, "SyncTimeReplyPacket must be 48 bytes");
static_assert(sizeof(AsynFeedHeader) == 16, "AsynFeedHeader must be 16 bytes");
static_assert(sizeof(AvccHeader) == 4, "AvccHeader must be 4 bytes");

// FourCC value verification
static_assert(static_cast<uint32_t>(PacketMagic::Ping) == 0x70696E67, "Ping magic mismatch");
static_assert(static_cast<uint32_t>(PacketMagic::Sync) == 0x73796E63, "Sync magic mismatch");
static_assert(static_cast<uint32_t>(PacketMagic::Asyn) == 0x6173796E, "Asyn magic mismatch");
static_assert(static_cast<uint32_t>(PacketMagic::Sprp) == 0x73707270, "Sprp magic mismatch");
static_assert(static_cast<uint32_t>(PacketMagic::Hbea) == 0x68626561, "Hbea magic mismatch");

static_assert(static_cast<uint32_t>(SubTypeMagic::Cwpa) == 0x63777061, "Cwpa subtype mismatch");
static_assert(static_cast<uint32_t>(SubTypeMagic::Time) == 0x74696D65, "Time subtype mismatch");
static_assert(static_cast<uint32_t>(SubTypeMagic::Feed) == 0x66656564, "Feed subtype mismatch");
static_assert(static_cast<uint32_t>(SubTypeMagic::Afmt) == 0x61666D74, "Afmt subtype mismatch");
static_assert(static_cast<uint32_t>(SubTypeMagic::Vfmt) == 0x76666D74, "Vfmt subtype mismatch");
static_assert(static_cast<uint32_t>(SubTypeMagic::Need) == 0x6E656564, "Need subtype mismatch");
static_assert(static_cast<uint32_t>(SubTypeMagic::Clok) == 0x636C6F6B, "Clok subtype mismatch");

} // namespace ios::protocol::wire
