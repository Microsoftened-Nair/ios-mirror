#include "session/device_handshake.hpp"

#include <cstring>
#include <vector>

namespace ios::session {

namespace {

#pragma pack(push, 1)

/**
 * @brief 32-byte SYNC clok packet establishing CoreMedia clock reference.
 */
struct SyncClokPacket {
    wire::HandshakeHeader header{32, wire::HandshakeMagic::Sync, 1};
    uint32_t sub_type{static_cast<uint32_t>(wire::HandshakeMagic::Clok)};
    uint32_t clock_id{1};
    uint64_t correlation_id{1};

    constexpr SyncClokPacket() = default;
    constexpr explicit SyncClokPacket(uint64_t corr_id, uint32_t clk_id = 1) noexcept
        : header(sizeof(SyncClokPacket), wire::HandshakeMagic::Sync, corr_id),
          sub_type(static_cast<uint32_t>(wire::HandshakeMagic::Clok)),
          clock_id(clk_id),
          correlation_id(corr_id) {}
};

/**
 * @brief 28-byte SPRP need packet defining framerate and quality configuration.
 */
struct SprpNeedPacket {
    wire::HandshakeHeader header{28, wire::HandshakeMagic::Sprp, 2};
    uint32_t sub_type{static_cast<uint32_t>(wire::HandshakeMagic::Need)};
    uint32_t target_fps{60};
    uint32_t quality{1};

    constexpr SprpNeedPacket() = default;
    constexpr SprpNeedPacket(uint64_t corr_id, uint32_t fps, uint32_t qual) noexcept
        : header(sizeof(SprpNeedPacket), wire::HandshakeMagic::Sprp, corr_id),
          sub_type(static_cast<uint32_t>(wire::HandshakeMagic::Need)),
          target_fps(fps),
          quality(qual) {}
};

#pragma pack(pop)

static_assert(sizeof(SyncClokPacket) == 32, "SyncClokPacket must be 32 bytes");
static_assert(sizeof(SprpNeedPacket) == 28, "SprpNeedPacket must be 28 bytes");

} // namespace

DeviceHandshake::DeviceHandshake(transport::IUsbTransport& transport, protocol::ClockSynchronizer& clock_sync)
    : transport_(transport), clock_sync_(clock_sync) {}

shared::Result<void, HandshakeError> DeviceHandshake::ExecuteHandshake(const StreamConfiguration& config) {
    // Step 1: Send PING keep-alive packet (timeout 1000 ms)
    auto ping = clock_sync_.CreatePingPacket();
    shared::ReadOnlyBuffer ping_buf(reinterpret_cast<const uint8_t*>(&ping), sizeof(ping));
    auto ping_res = transport_.SendBulkData(ping_buf, 1000);
    if (ping_res.is_err()) {
        return HandshakeError::TransportError;
    }

    // Step 2: Send Clock Parameters (32-byte SYNC clok packet, timeout 1000 ms)
    auto clock_res = SendClockParameters();
    if (clock_res.is_err()) {
        return clock_res.error();
    }

    // Step 3: Send Stream Parameters (28-byte SPRP need packet, timeout 1000 ms)
    auto stream_res = SendStreamParameters(config);
    if (stream_res.is_err()) {
        return stream_res.error();
    }

    // Step 4: Set Stream Enabled (SPRP vfmt XML property list, timeout 2000 ms)
    auto enable_res = SetStreamEnabled(true);
    if (enable_res.is_err()) {
        return enable_res.error();
    }

    return {};
}

shared::Result<void, HandshakeError> DeviceHandshake::SendClockParameters() {
    SyncClokPacket packet(++correlation_counter_, 1);
    shared::ReadOnlyBuffer buf(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    auto res = transport_.SendBulkData(buf, 1000);
    if (res.is_err()) {
        return HandshakeError::TransportError;
    }
    return {};
}

shared::Result<void, HandshakeError> DeviceHandshake::SendStreamParameters(const StreamConfiguration& config) {
    uint32_t quality = config.request_high_quality ? 1u : 0u;
    SprpNeedPacket packet(++correlation_counter_, config.target_fps, quality);
    shared::ReadOnlyBuffer buf(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    auto res = transport_.SendBulkData(buf, 1000);
    if (res.is_err()) {
        return HandshakeError::TransportError;
    }
    return {};
}

shared::Result<void, HandshakeError> DeviceHandshake::SetStreamEnabled(bool enabled) {
    const std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>Valuer</key>\n\t" +
        std::string(enabled ? "<true/>" : "<false/>") +
        "\n</dict>\n"
        "</plist>\n";

    uint32_t total_length = static_cast<uint32_t>(sizeof(wire::HandshakeHeader) + sizeof(uint32_t) + plist.size());
    std::vector<uint8_t> packet_data(total_length);

    wire::HandshakeHeader header(total_length, wire::HandshakeMagic::Sprp, ++correlation_counter_);
    uint32_t sub_type = static_cast<uint32_t>(wire::HandshakeMagic::Vfmt);

    std::memcpy(packet_data.data(), &header, sizeof(header));
    std::memcpy(packet_data.data() + sizeof(header), &sub_type, sizeof(sub_type));
    std::memcpy(packet_data.data() + sizeof(header) + sizeof(sub_type), plist.data(), plist.size());

    shared::ReadOnlyBuffer buf(packet_data.data(), packet_data.size());
    auto res = transport_.SendBulkData(buf, 2000);
    if (res.is_err()) {
        return HandshakeError::TransportError;
    }
    return {};
}

} // namespace ios::session
