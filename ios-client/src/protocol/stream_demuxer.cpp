#include "protocol/stream_demuxer.hpp"

#include <bit>
#include <chrono>
#include <cstring>

namespace ios::protocol {

namespace {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ReadBit(uint32_t& bit) {
        if (byte_offset_ >= size_) {
            return false;
        }
        bit = (static_cast<uint32_t>(data_[byte_offset_]) >> (7 - bit_offset_)) & 1u;
        bit_offset_++;
        if (bit_offset_ == 8) {
            bit_offset_ = 0;
            byte_offset_++;
        }
        return true;
    }

    bool ReadBits(size_t n, uint32_t& out) {
        out = 0;
        for (size_t i = 0; i < n; ++i) {
            uint32_t b = 0;
            if (!ReadBit(b)) {
                return false;
            }
            out = (out << 1) | b;
        }
        return true;
    }

    bool ReadUe(uint32_t& out) {
        size_t zero_count = 0;
        uint32_t b = 0;
        while (ReadBit(b) && b == 0) {
            zero_count++;
            if (zero_count > 31) {
                return false;
            }
        }
        if (zero_count == 0) {
            out = 0;
            return true;
        }
        uint32_t val = 0;
        if (!ReadBits(zero_count, val)) {
            return false;
        }
        out = (1u << zero_count) - 1u + val;
        return true;
    }

    bool ReadSe(int32_t& out) {
        uint32_t code_num = 0;
        if (!ReadUe(code_num)) {
            return false;
        }
        if (code_num % 2 == 0) {
            out = -static_cast<int32_t>(code_num / 2);
        } else {
            out = static_cast<int32_t>((code_num + 1) / 2);
        }
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t byte_offset_{0};
    size_t bit_offset_{0};
};

bool ParseSpsDimensions(const std::vector<uint8_t>& sps, uint32_t& out_width, uint32_t& out_height) {
    if (sps.size() <= 4) {
        return false;
    }

    // Strip emulation prevention bytes (0x00 0x00 0x03 -> 0x00 0x00)
    std::vector<uint8_t> rbsp;
    rbsp.reserve(sps.size());
    for (size_t i = 0; i < sps.size(); ++i) {
        if (i + 2 < sps.size() && sps[i] == 0x00 && sps[i + 1] == 0x00 && sps[i + 2] == 0x03) {
            rbsp.push_back(sps[i]);
            rbsp.push_back(sps[i + 1]);
            i += 2;
        } else {
            rbsp.push_back(sps[i]);
        }
    }

    // Skip NAL header (byte 0)
    BitReader reader(rbsp.data() + 1, rbsp.size() - 1);
    uint32_t profile_idc = 0, constraint = 0, level_idc = 0, sps_id = 0;
    if (!reader.ReadBits(8, profile_idc) ||
        !reader.ReadBits(8, constraint) ||
        !reader.ReadBits(8, level_idc) ||
        !reader.ReadUe(sps_id)) {
        return false;
    }

    uint32_t chroma_format_idc = 1;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
        if (!reader.ReadUe(chroma_format_idc)) return false;
        if (chroma_format_idc == 3) {
            uint32_t separate_colour_plane = 0;
            if (!reader.ReadBit(separate_colour_plane)) return false;
        }
        uint32_t bit_depth_luma = 0, bit_depth_chroma = 0, qpprime = 0, seq_scaling = 0;
        if (!reader.ReadUe(bit_depth_luma) ||
            !reader.ReadUe(bit_depth_chroma) ||
            !reader.ReadBit(qpprime) ||
            !reader.ReadBit(seq_scaling)) {
            return false;
        }
        if (seq_scaling) {
            size_t count = (chroma_format_idc != 3) ? 8 : 12;
            for (size_t i = 0; i < count; ++i) {
                uint32_t scaling_list_present = 0;
                if (!reader.ReadBit(scaling_list_present)) return false;
                if (scaling_list_present) {
                    size_t list_size = (i < 6) ? 16 : 64;
                    int32_t last_scale = 8, next_scale = 8;
                    for (size_t j = 0; j < list_size; ++j) {
                        if (next_scale != 0) {
                            int32_t delta_scale = 0;
                            if (!reader.ReadSe(delta_scale)) return false;
                            next_scale = (last_scale + delta_scale + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    }

    uint32_t log2_max_frame_num = 0, pic_order_cnt_type = 0;
    if (!reader.ReadUe(log2_max_frame_num) ||
        !reader.ReadUe(pic_order_cnt_type)) {
        return false;
    }
    if (pic_order_cnt_type == 0) {
        uint32_t log2_max_pic_order_cnt_lsb = 0;
        if (!reader.ReadUe(log2_max_pic_order_cnt_lsb)) return false;
    } else if (pic_order_cnt_type == 1) {
        uint32_t delta_pic_order_always_zero = 0;
        int32_t offset_for_non_ref = 0, offset_for_top_to_bottom = 0;
        uint32_t num_ref_frames_in_pic_order = 0;
        if (!reader.ReadBit(delta_pic_order_always_zero) ||
            !reader.ReadSe(offset_for_non_ref) ||
            !reader.ReadSe(offset_for_top_to_bottom) ||
            !reader.ReadUe(num_ref_frames_in_pic_order)) {
            return false;
        }
        for (size_t i = 0; i < num_ref_frames_in_pic_order; ++i) {
            int32_t offset = 0;
            if (!reader.ReadSe(offset)) return false;
        }
    }

    uint32_t max_num_ref_frames = 0, gaps_in_frame_num = 0;
    if (!reader.ReadUe(max_num_ref_frames) ||
        !reader.ReadBit(gaps_in_frame_num)) {
        return false;
    }

    uint32_t pic_width_in_mbs_minus1 = 0, pic_height_in_map_units_minus1 = 0;
    if (!reader.ReadUe(pic_width_in_mbs_minus1) ||
        !reader.ReadUe(pic_height_in_map_units_minus1)) {
        return false;
    }

    uint32_t frame_mbs_only_flag = 0;
    if (!reader.ReadBit(frame_mbs_only_flag)) return false;
    if (!frame_mbs_only_flag) {
        uint32_t mb_adaptive = 0;
        if (!reader.ReadBit(mb_adaptive)) return false;
    }

    uint32_t direct_8x8 = 0, frame_cropping = 0;
    if (!reader.ReadBit(direct_8x8) ||
        !reader.ReadBit(frame_cropping)) {
        return false;
    }

    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (frame_cropping) {
        if (!reader.ReadUe(crop_left) ||
            !reader.ReadUe(crop_right) ||
            !reader.ReadUe(crop_top) ||
            !reader.ReadUe(crop_bottom)) {
            return false;
        }
    }

    uint32_t crop_unit_x = 2; // For 4:2:0
    uint32_t crop_unit_y = 2 * (2 - frame_mbs_only_flag);

    out_width = ((pic_width_in_mbs_minus1 + 1) * 16) - (crop_left + crop_right) * crop_unit_x;
    out_height = ((2 - frame_mbs_only_flag) * (pic_height_in_map_units_minus1 + 1) * 16) - (crop_top + crop_bottom) * crop_unit_y;

    return true;
}

// 219-byte ASYN HPD1 packet (Display properties dict: Valeria=true, HEVCDecoderSupports444=true, Width=1920, Height=1200)
constexpr uint8_t kAsynHpd1Packet[] = {
    0xdb, 0x00, 0x00, 0x00, 0x6e, 0x79, 0x73, 0x61, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x31, 0x64, 0x70, 0x68, 0xc7, 0x00, 0x00, 0x00, 0x74, 0x63, 0x69, 0x64, 0x20, 0x00, 0x00, 0x00,
    0x76, 0x79, 0x65, 0x6b, 0x0f, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x56, 0x61, 0x6c, 0x65,
    0x72, 0x69, 0x61, 0x09, 0x00, 0x00, 0x00, 0x76, 0x6c, 0x75, 0x62, 0x01, 0x2f, 0x00, 0x00, 0x00,
    0x76, 0x79, 0x65, 0x6b, 0x1e, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x48, 0x45, 0x56, 0x43,
    0x44, 0x65, 0x63, 0x6f, 0x64, 0x65, 0x72, 0x53, 0x75, 0x70, 0x70, 0x6f, 0x72, 0x74, 0x73, 0x34,
    0x34, 0x34, 0x09, 0x00, 0x00, 0x00, 0x76, 0x6c, 0x75, 0x62, 0x01, 0x70, 0x00, 0x00, 0x00, 0x76,
    0x79, 0x65, 0x6b, 0x13, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x44, 0x69, 0x73, 0x70, 0x6c,
    0x61, 0x79, 0x53, 0x69, 0x7a, 0x65, 0x55, 0x00, 0x00, 0x00, 0x74, 0x63, 0x69, 0x64, 0x26, 0x00,
    0x00, 0x00, 0x76, 0x79, 0x65, 0x6b, 0x0d, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x57, 0x69,
    0x64, 0x74, 0x68, 0x11, 0x00, 0x00, 0x00, 0x76, 0x62, 0x6d, 0x6e, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x9e, 0x40, 0x27, 0x00, 0x00, 0x00, 0x76, 0x79, 0x65, 0x6b, 0x0e, 0x00, 0x00, 0x00,
    0x6b, 0x72, 0x74, 0x73, 0x48, 0x65, 0x69, 0x67, 0x68, 0x74, 0x11, 0x00, 0x00, 0x00, 0x76, 0x62,
    0x6d, 0x6e, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x92, 0x40
};

// 337-byte ASYN HPA1 template (Audio format dict)
constexpr uint8_t kAsynHpa1Template[] = {
    0x51, 0x01, 0x00, 0x00, 0x6e, 0x79, 0x73, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x31, 0x61, 0x70, 0x68, 0x3d, 0x01, 0x00, 0x00, 0x74, 0x63, 0x69, 0x64, 0x34, 0x00, 0x00, 0x00,
    0x76, 0x79, 0x65, 0x6b, 0x1b, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x42, 0x75, 0x66, 0x66,
    0x65, 0x72, 0x41, 0x68, 0x65, 0x61, 0x64, 0x49, 0x6e, 0x74, 0x65, 0x72, 0x76, 0x61, 0x6c, 0x11,
    0x00, 0x00, 0x00, 0x76, 0x62, 0x6d, 0x6e, 0x06, 0xe4, 0xa5, 0x9b, 0xc4, 0x20, 0xb0, 0xb2, 0x3f,
    0x28, 0x00, 0x00, 0x00, 0x76, 0x79, 0x65, 0x6b, 0x11, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73,
    0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x55, 0x49, 0x44, 0x0f, 0x00, 0x00, 0x00, 0x76, 0x72, 0x74,
    0x73, 0x56, 0x61, 0x6c, 0x65, 0x72, 0x69, 0x61, 0x2e, 0x00, 0x00, 0x00, 0x76, 0x79, 0x65, 0x6b,
    0x15, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x53, 0x63, 0x72, 0x65, 0x65, 0x6e, 0x4c, 0x61,
    0x74, 0x65, 0x6e, 0x63, 0x79, 0x11, 0x00, 0x00, 0x00, 0x76, 0x62, 0x6d, 0x6e, 0x06, 0x7b, 0x14,
    0xae, 0x47, 0xe1, 0x7a, 0xa4, 0x3f, 0x57, 0x00, 0x00, 0x00, 0x76, 0x79, 0x65, 0x6b, 0x0f, 0x00,
    0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x73, 0x40, 0x00, 0x00,
    0x00, 0x76, 0x74, 0x61, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xe7, 0x40, 0x6d, 0x63, 0x70,
    0x6c, 0x0c, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x70, 0xe7, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xe7, 0x40, 0x2b, 0x00, 0x00,
    0x00, 0x76, 0x79, 0x65, 0x6b, 0x16, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x45, 0x44, 0x49,
    0x44, 0x41, 0x43, 0x33, 0x53, 0x75, 0x70, 0x70, 0x6f, 0x72, 0x74, 0x0d, 0x00, 0x00, 0x00, 0x76,
    0x62, 0x6d, 0x6e, 0x03, 0x00, 0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x76, 0x79, 0x65, 0x6b,
    0x12, 0x00, 0x00, 0x00, 0x6b, 0x72, 0x74, 0x73, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x4e, 0x61,
    0x6d, 0x65, 0x0f, 0x00, 0x00, 0x00, 0x76, 0x72, 0x74, 0x73, 0x56, 0x61, 0x6c, 0x65, 0x72, 0x69,
    0x61
};

} // namespace

StreamDemuxer::StreamDemuxer(transport::IUsbTransport& transport)
    : transport_(transport),
      reassembly_buffer_(std::make_unique<uint8_t[]>(kReassemblyCapacity)) {}

StreamDemuxer::~StreamDemuxer() noexcept {
    Stop();
}

void StreamDemuxer::Initialize(FormatChangeCallback format_callback) {
    format_callback_ = std::move(format_callback);
    reassembly_offset_ = 0;
    clock_sync_.Reset();
    output_queue_.Reset();
    device_ping_count_.store(0, std::memory_order_release);
    video_clock_ref_.store(0, std::memory_order_release);
}

void StreamDemuxer::Start() {
    Stop();
    ping_thread_ = std::jthread([this](std::stop_token stop_token) {
        // Do not write anything until the device has sent its own PING (see Start() docs).
        while (!stop_token.stop_requested() && !HasSeenDevicePing()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        while (!stop_token.stop_requested()) {
            // The reply to the device's own PING has just gone out; the first keep-alive is due 1 s later.
            for (int i = 0; i < 20 && !stop_token.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (stop_token.stop_requested()) {
                break;
            }

            wire::PacketHeader ping = clock_sync_.CreatePingPacket();
            shared::ReadOnlyBuffer ping_buf(reinterpret_cast<const uint8_t*>(&ping), sizeof(ping));
            transport_.SendBulkData(ping_buf, 1000);
        }
    });
}

bool StreamDemuxer::WaitForDevicePing(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!HasSeenDevicePing()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void StreamDemuxer::Stop() noexcept {
    if (ping_thread_.joinable()) {
        ping_thread_.request_stop();
        ping_thread_.join();
    }
}

void StreamDemuxer::IngestRawData(shared::ReadOnlyBuffer chunk) {
    if (chunk.empty()) {
        return;
    }

    if (reassembly_offset_ + chunk.size() > kReassemblyCapacity) {
        reassembly_offset_ = 0;
    }

    if (chunk.size() <= kReassemblyCapacity) {
        std::memcpy(reassembly_buffer_.get() + reassembly_offset_, chunk.data(), chunk.size());
        reassembly_offset_ += chunk.size();
    }

    ParsePackets();
}

void StreamDemuxer::ParsePackets() {
    while (reassembly_offset_ >= sizeof(wire::PacketHeader)) {
        wire::PacketHeader header;
        std::memcpy(&header, reassembly_buffer_.get(), sizeof(wire::PacketHeader));

        if (header.length < sizeof(wire::PacketHeader) || header.length > kReassemblyCapacity) {
            std::memmove(reassembly_buffer_.get(), reassembly_buffer_.get() + 1, reassembly_offset_ - 1);
            reassembly_offset_ -= 1;
            continue;
        }

        if (reassembly_offset_ < header.length) {
            break;
        }

        shared::ReadOnlyBuffer packet_span(reassembly_buffer_.get(), header.length);
        ProcessCompletePacket(packet_span);

        size_t remaining = reassembly_offset_ - header.length;
        if (remaining > 0) {
            std::memmove(reassembly_buffer_.get(), reassembly_buffer_.get() + header.length, remaining);
        }
        reassembly_offset_ = remaining;
    }
}

void StreamDemuxer::ProcessCompletePacket(shared::ReadOnlyBuffer packet) {
    if (packet.size() < sizeof(wire::PacketHeader)) {
        return;
    }

    wire::PacketHeader header;
    std::memcpy(&header, packet.data(), sizeof(wire::PacketHeader));
    shared::ReadOnlyBuffer payload = packet.subspan(sizeof(wire::PacketHeader));

    uint32_t norm_magic = header.magic | 0x20202020;
    constexpr uint32_t kSyncNorm = static_cast<uint32_t>(wire::PacketMagic::Sync) | 0x20202020;
    constexpr uint32_t kAsynNorm = static_cast<uint32_t>(wire::PacketMagic::Asyn) | 0x20202020;
    constexpr uint32_t kPingNorm = static_cast<uint32_t>(wire::PacketMagic::Ping) | 0x20202020;

    if (header.magic == static_cast<uint32_t>(wire::PacketMagic::Sync) || norm_magic == kSyncNorm) {
        ProcessSyncPacket(header, payload);
    } else if (header.magic == static_cast<uint32_t>(wire::PacketMagic::Asyn) || norm_magic == kAsynNorm) {
        ProcessAsynPacket(header, payload);
    } else if (header.magic == static_cast<uint32_t>(wire::PacketMagic::Ping) || norm_magic == kPingNorm) {
        // Device-initiated PING: answer it, and only then mark the device as ready.
        wire::PacketHeader ping = clock_sync_.CreatePingPacket();
        shared::ReadOnlyBuffer ping_buf(reinterpret_cast<const uint8_t*>(&ping), sizeof(ping));
        transport_.SendBulkData(ping_buf, 1000);
        device_ping_count_.fetch_add(1, std::memory_order_acq_rel);
    }
}

void StreamDemuxer::ProcessSyncPacket(const wire::PacketHeader& header, shared::ReadOnlyBuffer payload) {
    if (payload.size() < sizeof(uint32_t)) {
        return;
    }

    uint32_t sub_type = 0;
    std::memcpy(&sub_type, payload.data(), sizeof(uint32_t));

    uint32_t norm_sub = sub_type | 0x20202020;
    constexpr uint32_t kCwpaNorm = static_cast<uint32_t>(wire::SubTypeMagic::Cwpa) | 0x20202020;
    constexpr uint32_t kCvrpNorm = static_cast<uint32_t>(wire::SubTypeMagic::Cvrp) | 0x20202020;
    constexpr uint32_t kTimeNorm = static_cast<uint32_t>(wire::SubTypeMagic::Time) | 0x20202020;
    constexpr uint32_t kClokNorm = static_cast<uint32_t>(wire::SubTypeMagic::Clok) | 0x20202020;
    constexpr uint32_t kAfmtNorm = static_cast<uint32_t>(wire::SubTypeMagic::Afmt) | 0x20202020;

    if (sub_type == static_cast<uint32_t>(wire::SubTypeMagic::Cwpa) || norm_sub == kCwpaNorm) {
        auto res = clock_sync_.HandleCwpaRequest(header.correlation_id, payload);
        if (payload.size() >= 20) {
            // Real iOS QuickTime exchange:
            // 1. Send ASYN HPD1 twice (signals display mirroring support)
            // 2. Send CWPA-RPLY (status 0, clock_ref = device_clock_ref + 1000)
            // 3. Send ASYN HPA1 (signals audio pipeline support)
            uint64_t dev_clk = 0;
            std::memcpy(&dev_clk, payload.data() + 12, sizeof(uint64_t));
            if (dev_clk == 0) {
                dev_clk = header.correlation_id;
            }

            SendHpd1();
            SendHpd1();
            SendClockRefReply(header.correlation_id, dev_clk + 1000);
            SendHpa1(dev_clk);
        } else if (res.is_ok()) {
            const auto& reply = res.value();
            shared::ReadOnlyBuffer reply_buf(reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));
            transport_.SendBulkData(reply_buf, 1000);
        }
    } else if (sub_type == static_cast<uint32_t>(wire::SubTypeMagic::Cvrp) || norm_sub == kCvrpNorm) {
        uint64_t dev_clk = 0;
        if (payload.size() >= 20) {
            std::memcpy(&dev_clk, payload.data() + 12, sizeof(uint64_t));
        } else if (payload.size() >= 8) {
            std::memcpy(&dev_clk, payload.data() + 4, sizeof(uint64_t));
        }
        if (dev_clk == 0) {
            dev_clk = header.correlation_id;
        }
        video_clock_ref_.store(dev_clk, std::memory_order_release);

        // Send ASYN NEED to request video stream
        SendNeed(dev_clk);

        // Send CVRP reply (status 0, clock_ref = device_clock_ref + 0x1000AF)
        SendClockRefReply(header.correlation_id, dev_clk + 0x1000AF);
    } else if (sub_type == static_cast<uint32_t>(wire::SubTypeMagic::Clok) || norm_sub == kClokNorm) {
        uint64_t clk_ref = 0;
        if (payload.size() >= 12) {
            std::memcpy(&clk_ref, payload.data() + 4, sizeof(uint64_t));
        }
        SendClockRefReply(header.correlation_id, clk_ref + 0x10000);
    } else if (sub_type == static_cast<uint32_t>(wire::SubTypeMagic::Afmt) || norm_sub == kAfmtNorm) {
        SendClockRefReply(header.correlation_id, 0);
    } else if (sub_type == 0x676F2120) { // 'og! '
        SendClockRefReply(header.correlation_id, 0);
    } else if (sub_type == static_cast<uint32_t>(wire::SubTypeMagic::Time) || norm_sub == kTimeNorm) {
        auto res = clock_sync_.HandleTimeRequest(header.correlation_id, payload);
        if (res.is_ok()) {
            const auto& reply = res.value();
            shared::ReadOnlyBuffer reply_buf(reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));
            transport_.SendBulkData(reply_buf, 1000);
        }
    }
}

void StreamDemuxer::ProcessAsynPacket(const wire::PacketHeader& /*header*/, shared::ReadOnlyBuffer payload) {
    if (payload.size() < sizeof(uint32_t)) {
        return;
    }

    uint32_t sub_type = 0;
    std::memcpy(&sub_type, payload.data(), sizeof(uint32_t));

    uint32_t norm_sub = sub_type | 0x20202020;
    constexpr uint32_t kFeedNorm = static_cast<uint32_t>(wire::SubTypeMagic::Feed) | 0x20202020;
    constexpr uint32_t kVfmtNorm = static_cast<uint32_t>(wire::SubTypeMagic::Vfmt) | 0x20202020;

    if (sub_type == static_cast<uint32_t>(wire::SubTypeMagic::Feed) || norm_sub == kFeedNorm) {
        if (payload.size() < sizeof(wire::AsynFeedHeader) + sizeof(wire::CMTimeWire)) {
            return;
        }

        wire::AsynFeedHeader feed_hdr;
        std::memcpy(&feed_hdr, payload.data(), sizeof(feed_hdr));

        if (feed_hdr.stream_index != 0) {
            return;
        }

        bool is_keyframe = (feed_hdr.sample_flags & 0x01) == 0;

        wire::CMTimeWire pts_wire;
        std::memcpy(&pts_wire, payload.data() + sizeof(wire::AsynFeedHeader), sizeof(wire::CMTimeWire));

        uint64_t pts_nanos = 0;
        if (pts_wire.timescale > 0 && pts_wire.value >= 0) {
            const uint64_t uval = static_cast<uint64_t>(pts_wire.value);
            const uint64_t uscale = static_cast<uint64_t>(pts_wire.timescale);
            const uint64_t whole = uval / uscale;
            const uint64_t rem = uval % uscale;
            const uint64_t device_nanos = (whole * 1'000'000'000ULL) + ((rem * 1'000'000'000ULL) / uscale);
            pts_nanos = clock_sync_.DeviceTimeToHostTime(device_nanos);
        }

        uint64_t duration_nanos = 0;
        size_t slice_start_offset = sizeof(wire::AsynFeedHeader) + sizeof(wire::CMTimeWire);

        if (payload.size() >= slice_start_offset + sizeof(wire::CMTimeWire) + sizeof(uint32_t)) {
            uint32_t candidate_len_be = 0;
            std::memcpy(&candidate_len_be, payload.data() + slice_start_offset, sizeof(uint32_t));
            uint32_t cand_len = wire::AvccHeader(candidate_len_be).get_nalu_length();

            if (slice_start_offset + sizeof(uint32_t) + cand_len > payload.size()) {
                size_t offset2 = slice_start_offset + sizeof(wire::CMTimeWire);
                uint32_t cand_len2_be = 0;
                std::memcpy(&cand_len2_be, payload.data() + offset2, sizeof(uint32_t));
                uint32_t cand_len2 = wire::AvccHeader(cand_len2_be).get_nalu_length();

                if (offset2 + sizeof(uint32_t) + cand_len2 <= payload.size()) {
                    wire::CMTimeWire dur_wire;
                    std::memcpy(&dur_wire, payload.data() + slice_start_offset, sizeof(wire::CMTimeWire));
                    if (dur_wire.timescale > 0 && dur_wire.value >= 0) {
                        const uint64_t duval = static_cast<uint64_t>(dur_wire.value);
                        const uint64_t duscale = static_cast<uint64_t>(dur_wire.timescale);
                        const uint64_t dwhole = duval / duscale;
                        const uint64_t drem = duval % duscale;
                        duration_nanos = (dwhole * 1'000'000'000ULL) + ((drem * 1'000'000'000ULL) / duscale);
                    }
                    slice_start_offset = offset2;
                }
            }
        }

        shared::ReadOnlyBuffer slice_data = payload.subspan(slice_start_offset);
        ExtractAvccSlices(slice_data, pts_nanos, duration_nanos, is_keyframe);

        // Send flow control NEED to request subsequent frame from iOS
        uint64_t vclk = video_clock_ref_.load(std::memory_order_acquire);
        if (vclk != 0) {
            SendNeed(vclk);
        }

    } else if (sub_type == static_cast<uint32_t>(wire::SubTypeMagic::Vfmt) || norm_sub == kVfmtNorm) {
        FormatDescriptor desc;
        if (ParseAvccFormatDescriptor(payload, desc)) {
            if (format_callback_) {
                format_callback_(desc);
            }
        }
    }
}

bool StreamDemuxer::ExtractAvccSlices(shared::ReadOnlyBuffer avcc_data,
                                      uint64_t pts_nanos,
                                      uint64_t duration_nanos,
                                      bool is_keyframe) {
    if (avcc_data.empty()) {
        return false;
    }

    return output_queue_.PushWith([&](VideoSample& sample) -> bool {
        sample.pts_nanos = pts_nanos;
        sample.duration_nanos = duration_nanos;
        sample.is_keyframe = is_keyframe;

        size_t in_offset = 0;
        size_t out_offset = 0;
        constexpr uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

        while (in_offset + sizeof(uint32_t) <= avcc_data.size()) {
            uint32_t slice_len_be = 0;
            std::memcpy(&slice_len_be, avcc_data.data() + in_offset, sizeof(uint32_t));
#if defined(__cpp_lib_byteswap) && __cpp_lib_byteswap >= 202110L
            uint32_t slice_len = std::byteswap(slice_len_be);
#else
            uint32_t slice_len = __builtin_bswap32(slice_len_be);
#endif
            in_offset += sizeof(uint32_t);

            if (slice_len == 0 || in_offset + slice_len > avcc_data.size()) {
                break;
            }

            uint8_t nalu_type = avcc_data.data()[in_offset] & 0x1Fu;
            if (nalu_type == 5) {
                sample.is_keyframe = true;
            }

            if (out_offset + sizeof(kAnnexBStartCode) + slice_len > VideoSample::kMaxPayloadCapacity) {
                break;
            }

            std::memcpy(sample.data + out_offset, kAnnexBStartCode, sizeof(kAnnexBStartCode));
            out_offset += sizeof(kAnnexBStartCode);

            std::memcpy(sample.data + out_offset, avcc_data.data() + in_offset, slice_len);
            out_offset += slice_len;
            in_offset += slice_len;
        }

        if (out_offset == 0) {
            return false;
        }

        sample.size = out_offset;
        return true;
    });
}

bool StreamDemuxer::ParseAvccFormatDescriptor(shared::ReadOnlyBuffer vfmt_payload, FormatDescriptor& out_desc) {
    if (vfmt_payload.size() < 8) {
        return false;
    }

    const uint8_t* avcc_record = nullptr;
    size_t avcc_len = 0;

    constexpr uint8_t kAvccTag[4] = {'a', 'v', 'c', 'C'};
    for (size_t i = 0; i + 4 <= vfmt_payload.size(); ++i) {
        if (std::memcmp(vfmt_payload.data() + i, kAvccTag, 4) == 0) {
            avcc_record = vfmt_payload.data() + i + 4;
            avcc_len = vfmt_payload.size() - (i + 4);
            break;
        }
    }

    if (!avcc_record) {
        for (size_t i = 0; i + 7 <= vfmt_payload.size(); ++i) {
            if (vfmt_payload.data()[i] == 1 && (vfmt_payload.data()[i + 4] & 0xFCu) == 0xFCu) {
                avcc_record = vfmt_payload.data() + i;
                avcc_len = vfmt_payload.size() - i;
                break;
            }
        }
    }

    if (!avcc_record || avcc_len < 7) {
        return false;
    }

    uint8_t num_sps = avcc_record[5] & 0x1Fu;
    size_t offset = 6;

    for (uint8_t i = 0; i < num_sps && offset + 2 <= avcc_len; ++i) {
        uint16_t sps_len = static_cast<uint16_t>(
            (static_cast<uint16_t>(avcc_record[offset]) << 8) |
            static_cast<uint16_t>(avcc_record[offset + 1]));
        offset += 2;
        if (offset + sps_len <= avcc_len) {
            out_desc.sps.assign(avcc_record + offset, avcc_record + offset + sps_len);
            offset += sps_len;
        }
    }

    if (offset < avcc_len) {
        uint8_t num_pps = avcc_record[offset];
        offset += 1;
        for (uint8_t i = 0; i < num_pps && offset + 2 <= avcc_len; ++i) {
            uint16_t pps_len = static_cast<uint16_t>(
                (static_cast<uint16_t>(avcc_record[offset]) << 8) |
                static_cast<uint16_t>(avcc_record[offset + 1]));
            offset += 2;
            if (offset + pps_len <= avcc_len) {
                out_desc.pps.assign(avcc_record + offset, avcc_record + offset + pps_len);
                offset += pps_len;
            }
        }
    }

    if (!out_desc.sps.empty()) {
        ParseSpsDimensions(out_desc.sps, out_desc.width, out_desc.height);
    }

    return out_desc.is_valid();
}

void StreamDemuxer::SendHpd1() {
    shared::ReadOnlyBuffer buf(kAsynHpd1Packet, sizeof(kAsynHpd1Packet));
    transport_.SendBulkData(buf, 1000);
}

void StreamDemuxer::SendHpa1(uint64_t device_clock_ref) {
    uint8_t hpa1[sizeof(kAsynHpa1Template)];
    std::memcpy(hpa1, kAsynHpa1Template, sizeof(hpa1));
    std::memcpy(hpa1 + 8, &device_clock_ref, sizeof(uint64_t));
    shared::ReadOnlyBuffer buf(hpa1, sizeof(hpa1));
    transport_.SendBulkData(buf, 1000);
}

void StreamDemuxer::SendNeed(uint64_t clock_ref) {
    wire::AsynNeedPacket need(clock_ref);
    shared::ReadOnlyBuffer buf(reinterpret_cast<const uint8_t*>(&need), sizeof(need));
    transport_.SendBulkData(buf, 1000);
}

void StreamDemuxer::SendClockRefReply(uint64_t correlation_id, uint64_t clock_ref) {
    wire::SyncClockRefReplyPacket rply(correlation_id, clock_ref);
    shared::ReadOnlyBuffer buf(reinterpret_cast<const uint8_t*>(&rply), sizeof(rply));
    transport_.SendBulkData(buf, 1000);
}

} // namespace ios::protocol
