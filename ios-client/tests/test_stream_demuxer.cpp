#include "protocol/stream_demuxer.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

class MockTransport : public ios::transport::IUsbTransport {
public:
    shared::Result<void, ios::transport::UsbTransportError> Initialize() override {
        return shared::Result<void, ios::transport::UsbTransportError>::ok();
    }

    shared::Result<void, ios::transport::UsbTransportError> OpenDevice(
        uint16_t /*vid*/, uint16_t /*pid*/, std::string_view /*serial*/) override {
        return shared::Result<void, ios::transport::UsbTransportError>::ok();
    }

    shared::Result<void, ios::transport::UsbTransportError> TriggerCoreMediaMode() override {
        return shared::Result<void, ios::transport::UsbTransportError>::ok();
    }

    shared::Result<void, ios::transport::UsbTransportError> ClaimCoreMediaInterface() override {
        return shared::Result<void, ios::transport::UsbTransportError>::ok();
    }

    shared::Result<void, ios::transport::UsbTransportError> StartStreaming(
        ios::transport::UsbDataCallback /*callback*/) override {
        return shared::Result<void, ios::transport::UsbTransportError>::ok();
    }

    void StopStreaming() noexcept override {}

    shared::Result<size_t, ios::transport::UsbTransportError> SendBulkData(
        shared::ReadOnlyBuffer data, uint32_t /*timeout_ms*/) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint8_t> copy(data.begin(), data.end());
        sent_packets_.push_back(std::move(copy));
        return shared::Result<size_t, ios::transport::UsbTransportError>::ok(data.size());
    }

    bool IsConnected() const noexcept override {
        return true;
    }

    ios::transport::UsbEndpointPair GetActiveEndpoints() const noexcept override {
        return {0x86, 0x05};
    }

    size_t GetSentCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_packets_.size();
    }

    std::vector<uint8_t> GetLastSentPacket() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sent_packets_.empty()) return {};
        return sent_packets_.back();
    }

    void ClearSent() {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_packets_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::vector<uint8_t>> sent_packets_;
};

void TestSampleRingBuffer() {
    std::cout << "[Test] Running SampleRingBuffer unit tests..." << std::endl;
    ios::protocol::SampleRingBuffer<int, 4> rb;

    assert(rb.Empty());
    assert(!rb.Full());
    assert(rb.Size() == 0);

    // Push 4 items
    for (int i = 1; i <= 4; ++i) {
        bool ok = rb.PushWith([i](int& slot) { slot = i * 10; });
        assert(ok);
    }

    assert(rb.Full());
    assert(!rb.Empty());
    assert(rb.Size() == 4);

    // 5th push must fail (bounded capacity)
    bool ok5 = rb.PushWith([](int& slot) { slot = 999; });
    assert(!ok5);

    // Pop 2 items
    int val = 0;
    assert(rb.PopWith([&val](int slot) { val = slot; }));
    assert(val == 10);
    assert(rb.PopWith([&val](int slot) { val = slot; }));
    assert(val == 20);
    assert(rb.Size() == 2);

    // Push 2 more items to verify power-of-two wrap-around
    assert(rb.PushWith([](int& slot) { slot = 50; }));
    assert(rb.PushWith([](int& slot) { slot = 60; }));
    assert(rb.Full());

    // Drain remaining
    assert(rb.PopWith([&val](int slot) { val = slot; }));
    assert(val == 30);
    assert(rb.PopWith([&val](int slot) { val = slot; }));
    assert(val == 40);
    assert(rb.PopWith([&val](int slot) { val = slot; }));
    assert(val == 50);
    assert(rb.PopWith([&val](int slot) { val = slot; }));
    assert(val == 60);

    assert(rb.Empty());
    assert(rb.Size() == 0);
    assert(!rb.PopWith([](int) {}));
    std::cout << "[Test] SampleRingBuffer unit tests passed." << std::endl;
}

void TestPacketReassemblyAndSyncReplies() {
    std::cout << "[Test] Running Packet Reassembly and SYNC Handling..." << std::endl;
    MockTransport transport;
    ios::protocol::StreamDemuxer demuxer(transport);
    demuxer.Initialize(nullptr);

    // Construct a SYNC CWPA packet
    ios::protocol::wire::PacketHeader hdr;
    hdr.length = sizeof(ios::protocol::wire::PacketHeader) + sizeof(ios::protocol::wire::SyncCwpaPayload);
    hdr.magic = static_cast<uint32_t>(ios::protocol::wire::PacketMagic::Sync);
    hdr.correlation_id = 12345;

    ios::protocol::wire::SyncCwpaPayload payload;
    payload.sub_type = static_cast<uint32_t>(ios::protocol::wire::SubTypeMagic::Cwpa);
    payload.clock_id = 0xAA55;
    payload.correlation_id = 12345;

    std::vector<uint8_t> packet_data(hdr.length);
    std::memcpy(packet_data.data(), &hdr, sizeof(hdr));
    std::memcpy(packet_data.data() + sizeof(hdr), &payload, sizeof(payload));

    // Fragment packet across two chunks: chunk 1 = 10 bytes, chunk 2 = remaining
    shared::ReadOnlyBuffer chunk1(packet_data.data(), 10);
    demuxer.IngestRawData(chunk1);
    assert(transport.GetSentCount() == 0); // Incomplete, should not trigger reply yet

    shared::ReadOnlyBuffer chunk2(packet_data.data() + 10, packet_data.size() - 10);
    demuxer.IngestRawData(chunk2);

    // Complete packet reassembled! Should have sent CWPA reply
    assert(transport.GetSentCount() == 1);
    auto reply_bytes = transport.GetLastSentPacket();
    assert(reply_bytes.size() == sizeof(ios::protocol::wire::SyncCwpaReplyPacket));

    const auto* reply = reinterpret_cast<const ios::protocol::wire::SyncCwpaReplyPacket*>(reply_bytes.data());
    assert(reply->header.magic == static_cast<uint32_t>(ios::protocol::wire::PacketMagic::Sync));
    assert(reply->header.correlation_id == 12345);
    assert(reply->host_nanos > 0);

    // Test SYNC TIME packet
    transport.ClearSent();
    ios::protocol::wire::PacketHeader time_hdr;
    time_hdr.length = sizeof(ios::protocol::wire::PacketHeader) + sizeof(ios::protocol::wire::SyncTimePayload);
    time_hdr.magic = static_cast<uint32_t>(ios::protocol::wire::PacketMagic::Sync);
    time_hdr.correlation_id = 99999;

    ios::protocol::wire::SyncTimePayload time_payload;
    time_payload.sub_type = static_cast<uint32_t>(ios::protocol::wire::SubTypeMagic::Time);
    time_payload.clock_id = 0xAA55;
    time_payload.device_time_raw = 1'000'000'000ULL;

    std::vector<uint8_t> time_packet(time_hdr.length);
    std::memcpy(time_packet.data(), &time_hdr, sizeof(time_hdr));
    std::memcpy(time_packet.data() + sizeof(time_hdr), &time_payload, sizeof(time_payload));

    demuxer.IngestRawData(shared::ReadOnlyBuffer(time_packet.data(), time_packet.size()));
    assert(transport.GetSentCount() == 1);
    auto time_reply_bytes = transport.GetLastSentPacket();
    assert(time_reply_bytes.size() == sizeof(ios::protocol::wire::SyncTimeReplyPacket));

    const auto* time_reply = reinterpret_cast<const ios::protocol::wire::SyncTimeReplyPacket*>(time_reply_bytes.data());
    assert(time_reply->header.correlation_id == 99999);
    assert(time_reply->sub_type == static_cast<uint32_t>(ios::protocol::wire::SubTypeMagic::Time));
    assert(time_reply->device_time_raw == 1'000'000'000ULL);
    assert(time_reply->host_nanos > 0);
    assert(demuxer.GetClockSynchronizer().IsCalibrated());

    std::cout << "[Test] Packet Reassembly and SYNC Handling passed." << std::endl;
}

void TestFeedTranscoding() {
    std::cout << "[Test] Running ASYN Feed AVCC-to-Annex-B transcoding test..." << std::endl;
    MockTransport transport;
    ios::protocol::StreamDemuxer demuxer(transport);
    demuxer.Initialize(nullptr);

    // First calibrate clock so DeviceTimeToHostTime produces accurate offset
    ios::protocol::wire::SyncTimePayload time_payload;
    time_payload.sub_type = static_cast<uint32_t>(ios::protocol::wire::SubTypeMagic::Time);
    time_payload.clock_id = 1;
    time_payload.device_time_raw = 1'000'000'000ULL; // 1 sec

    shared::ReadOnlyBuffer pbuf(reinterpret_cast<const uint8_t*>(&time_payload), sizeof(time_payload));
    auto cal_res = demuxer.GetClockSynchronizer().HandleTimeRequest(1, pbuf);
    assert(cal_res.is_ok());

    // Build ASYN feed packet
    // Audio packet (stream_index = 1) -> must be discarded
    {
        ios::protocol::wire::PacketHeader audio_hdr;
        audio_hdr.magic = static_cast<uint32_t>(ios::protocol::wire::PacketMagic::Asyn);
        audio_hdr.correlation_id = 1;
        ios::protocol::wire::AsynFeedHeader audio_feed{1, 0}; // stream 1 = audio
        ios::protocol::wire::CMTimeWire audio_pts{1'000'000'000LL, 1'000'000'000, 1, 0};

        audio_hdr.length = sizeof(audio_hdr) + sizeof(audio_feed) + sizeof(audio_pts);
        std::vector<uint8_t> audio_pkt(audio_hdr.length);
        std::memcpy(audio_pkt.data(), &audio_hdr, sizeof(audio_hdr));
        std::memcpy(audio_pkt.data() + sizeof(audio_hdr), &audio_feed, sizeof(audio_feed));
        std::memcpy(audio_pkt.data() + sizeof(audio_hdr) + sizeof(audio_feed), &audio_pts, sizeof(audio_pts));

        demuxer.IngestRawData(shared::ReadOnlyBuffer(audio_pkt.data(), audio_pkt.size()));
        assert(demuxer.GetOutputQueue().Empty()); // Must be discarded!
    }

    // Video packet (stream_index = 0) with two AVCC slices
    // Slice 1: NALU type 7 (SPS) [0x67, 0x42, 0x00, 0x1E] length 4
    // Slice 2: NALU type 5 (IDR) [0x65, 0x88, 0x84, 0x00, 0x10] length 5
    uint8_t nalu1[] = {0x67, 0x42, 0x00, 0x1E};
    uint8_t nalu2[] = {0x65, 0x88, 0x84, 0x00, 0x10};

    uint32_t len1_be = ios::protocol::wire::AvccHeader::from_nalu_length(sizeof(nalu1)).nalu_length_be;
    uint32_t len2_be = ios::protocol::wire::AvccHeader::from_nalu_length(sizeof(nalu2)).nalu_length_be;

    std::vector<uint8_t> avcc_slices;
    avcc_slices.resize(sizeof(uint32_t) + sizeof(nalu1) + sizeof(uint32_t) + sizeof(nalu2));
    size_t o = 0;
    std::memcpy(avcc_slices.data() + o, &len1_be, 4); o += 4;
    std::memcpy(avcc_slices.data() + o, nalu1, sizeof(nalu1)); o += sizeof(nalu1);
    std::memcpy(avcc_slices.data() + o, &len2_be, 4); o += 4;
    std::memcpy(avcc_slices.data() + o, nalu2, sizeof(nalu2)); o += sizeof(nalu2);

    ios::protocol::wire::PacketHeader video_hdr;
    video_hdr.magic = static_cast<uint32_t>(ios::protocol::wire::PacketMagic::Asyn);
    video_hdr.correlation_id = 2;

    ios::protocol::wire::AsynFeedHeader video_feed{0, 0}; // stream 0 = video, flag 0 = keyframe
    ios::protocol::wire::CMTimeWire video_pts{1'033'333'333LL, 1'000'000'000, 1, 0}; // +33.33ms

    video_hdr.length = static_cast<uint32_t>(sizeof(video_hdr) + sizeof(video_feed) + sizeof(video_pts) + avcc_slices.size());
    std::vector<uint8_t> video_pkt(video_hdr.length);
    size_t po = 0;
    std::memcpy(video_pkt.data() + po, &video_hdr, sizeof(video_hdr)); po += sizeof(video_hdr);
    std::memcpy(video_pkt.data() + po, &video_feed, sizeof(video_feed)); po += sizeof(video_feed);
    std::memcpy(video_pkt.data() + po, &video_pts, sizeof(video_pts)); po += sizeof(video_pts);
    std::memcpy(video_pkt.data() + po, avcc_slices.data(), avcc_slices.size());

    demuxer.IngestRawData(shared::ReadOnlyBuffer(video_pkt.data(), video_pkt.size()));

    // Queue must now contain 1 sample
    assert(!demuxer.GetOutputQueue().Empty());
    assert(demuxer.GetOutputQueue().Size() == 1);

    bool popped = demuxer.GetOutputQueue().PopWith([&](const ios::protocol::VideoSample& sample) {
        assert(sample.is_keyframe);
        assert(sample.pts_nanos > 0);

        // Expected size: 4 (start code) + 4 (nalu1) + 4 (start code) + 5 (nalu2) = 17 bytes
        assert(sample.size == 17);

        // Verify Annex-B start codes {0x00, 0x00, 0x00, 0x01}
        uint8_t expected_prefix[] = {0x00, 0x00, 0x00, 0x01};
        assert(std::memcmp(sample.data, expected_prefix, 4) == 0);
        assert(std::memcmp(sample.data + 4, nalu1, sizeof(nalu1)) == 0);

        assert(std::memcmp(sample.data + 8, expected_prefix, 4) == 0);
        assert(std::memcmp(sample.data + 12, nalu2, sizeof(nalu2)) == 0);
    });

    assert(popped);
    assert(demuxer.GetOutputQueue().Empty());
    std::cout << "[Test] ASYN Feed AVCC-to-Annex-B transcoding test passed." << std::endl;
}

void TestFormatDescriptorParsing() {
    std::cout << "[Test] Running VFMT Format Descriptor parsing test..." << std::endl;
    MockTransport transport;
    ios::protocol::StreamDemuxer demuxer(transport);

    // H.264 Baseline SPS for 1920x1080 (Profile 66)
    std::vector<uint8_t> sps = {
        0x67, 0x42, 0xC0, 0x28, 0xD9, 0x00, 0x78, 0x02, 0x27, 0xE5,
        0x84, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00,
        0xF0, 0x36, 0x56, 0x01
    };
    std::vector<uint8_t> pps = {0x68, 0xCE, 0x3C, 0x80};

    std::vector<uint8_t> avcc_record;
    avcc_record.push_back(0x01);
    avcc_record.push_back(0x42);
    avcc_record.push_back(0xC0);
    avcc_record.push_back(0x28);
    avcc_record.push_back(0xFF);
    avcc_record.push_back(0xE1); // 1 SPS
    avcc_record.push_back(static_cast<uint8_t>(sps.size() >> 8));
    avcc_record.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    avcc_record.insert(avcc_record.end(), sps.begin(), sps.end());
    avcc_record.push_back(0x01); // 1 PPS
    avcc_record.push_back(static_cast<uint8_t>(pps.size() >> 8));
    avcc_record.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    avcc_record.insert(avcc_record.end(), pps.begin(), pps.end());

    // Build VFMT packet with 'avcC' atom tag
    std::vector<uint8_t> vfmt_payload;
    uint32_t vfmt_subtype = static_cast<uint32_t>(ios::protocol::wire::SubTypeMagic::Vfmt);
    vfmt_payload.resize(sizeof(uint32_t) + 4 + avcc_record.size());
    std::memcpy(vfmt_payload.data(), &vfmt_subtype, sizeof(uint32_t));
    std::memcpy(vfmt_payload.data() + 4, "avcC", 4);
    std::memcpy(vfmt_payload.data() + 8, avcc_record.data(), avcc_record.size());

    ios::protocol::wire::PacketHeader vfmt_hdr;
    vfmt_hdr.magic = static_cast<uint32_t>(ios::protocol::wire::PacketMagic::Asyn);
    vfmt_hdr.length = static_cast<uint32_t>(sizeof(vfmt_hdr) + vfmt_payload.size());
    vfmt_hdr.correlation_id = 42;

    std::vector<uint8_t> vfmt_pkt(vfmt_hdr.length);
    std::memcpy(vfmt_pkt.data(), &vfmt_hdr, sizeof(vfmt_hdr));
    std::memcpy(vfmt_pkt.data() + sizeof(vfmt_hdr), vfmt_payload.data(), vfmt_payload.size());

    bool callback_called = false;
    ios::protocol::FormatDescriptor received_desc;

    demuxer.Initialize([&](const ios::protocol::FormatDescriptor& desc) {
        callback_called = true;
        received_desc = desc;
    });

    demuxer.IngestRawData(shared::ReadOnlyBuffer(vfmt_pkt.data(), vfmt_pkt.size()));

    assert(callback_called);
    assert(received_desc.is_valid());
    assert(received_desc.sps == sps);
    assert(received_desc.pps == pps);
    assert(received_desc.width == 1920);
    assert(received_desc.height == 1080);
    std::cout << "[Test] VFMT Format Descriptor parsing passed (1920x1080)." << std::endl;
}

void TestPingHeartbeat() {
    std::cout << "[Test] Running Heartbeat Ping Thread test..." << std::endl;
    MockTransport transport;
    ios::protocol::StreamDemuxer demuxer(transport);
    demuxer.Initialize(nullptr);

    demuxer.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    demuxer.Stop();

    // At least one PING packet should have been transmitted
    assert(transport.GetSentCount() >= 1);
    auto ping_pkt = transport.GetLastSentPacket();
    assert(ping_pkt.size() == sizeof(ios::protocol::wire::PacketHeader));
    const auto* hdr = reinterpret_cast<const ios::protocol::wire::PacketHeader*>(ping_pkt.data());
    assert(hdr->magic == static_cast<uint32_t>(ios::protocol::wire::PacketMagic::Ping));
    std::cout << "[Test] Heartbeat Ping Thread test passed." << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Module 2 Task 3 Verification Harness ===" << std::endl;
    TestSampleRingBuffer();
    TestPacketReassemblyAndSyncReplies();
    TestFeedTranscoding();
    TestFormatDescriptorParsing();
    TestPingHeartbeat();
    std::cout << "=== All M2_T3 tests successfully passed! ===" << std::endl;
    return 0;
}
