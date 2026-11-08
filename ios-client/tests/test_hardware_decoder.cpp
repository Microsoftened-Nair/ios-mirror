#include "video/hardware_decoder.hpp"
#include "video/decoded_frame_pool.hpp"
#include "protocol/stream_demuxer.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

// Minimal valid 16x16 H.264 Baseline bitstream components
static const uint8_t kSpsBytes[] = {
    0x67, 0x42, 0xc0, 0x1e, 0xd9, 0x1e, 0xc0, 0x44,
    0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03,
    0x00, 0x08, 0x3c, 0x58, 0xb9, 0x20
};

static const uint8_t kPpsBytes[] = {
    0x68, 0xcb, 0x83, 0xcb, 0x20
};

static const uint8_t kIdrSliceBytes[] = {
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x5f, 0xb0, 0x5e, 0x81, 0x20, 0xd8,
    0x38, 0x00, 0x3b, 0x22, 0xdf, 0xe2, 0xc2, 0x53, 0x43, 0x44, 0x7f, 0x75,
    0x2c, 0x32, 0x0b, 0x13, 0x0d, 0x20, 0x47, 0x13, 0x78, 0x26, 0x20, 0x9d,
    0x3f, 0x64, 0x12, 0xd5, 0x7f, 0x89, 0xe0, 0x76, 0xa2, 0x19, 0xcf, 0xc7,
    0x2c, 0x4e, 0x85, 0x0d, 0xda, 0xe0, 0x4a, 0x2a, 0x95, 0xf1, 0xcb, 0x52,
    0x12, 0x39, 0xf4, 0xbf, 0xef, 0xf3, 0x26, 0xc2, 0x2b, 0x17, 0x0b, 0x5f,
    0xc4, 0x68, 0xb0, 0x78, 0x00, 0x15, 0xa0, 0x92, 0x5b, 0x42, 0x81, 0xf0,
    0xf7, 0xf0, 0x51, 0x62, 0x42, 0xa1, 0x4c, 0x0f, 0xd9, 0x06, 0x30, 0x1d,
    0xb8, 0x1d, 0x9f, 0x61, 0x07, 0xcb, 0x76, 0x76, 0x9e, 0xc8, 0x98, 0x60,
    0x11, 0xbb, 0x06, 0x00, 0x02, 0x00, 0x00, 0x06, 0x90, 0x23, 0x3c, 0x00,
    0x5d, 0x08, 0xa6, 0x6e, 0x86, 0x0a, 0x78, 0x48, 0x6a, 0x8b, 0x11, 0x89,
    0xed, 0x20, 0x42, 0x82, 0x56, 0xe6, 0x40, 0x07, 0xc5, 0xb6, 0x25, 0x40,
    0xa7, 0x5b, 0x4e, 0x38, 0x10, 0x37, 0xaa, 0x56, 0xd6, 0x82, 0x55, 0x00,
    0x03, 0xc3, 0x80, 0xe0, 0x99, 0x62, 0xe4, 0xae, 0x84, 0xb5, 0xbf, 0x5c,
    0x01, 0xc0, 0x71, 0xf2, 0xa4, 0xf1, 0x63, 0x56, 0xe5, 0xf7, 0xfd, 0x34,
    0x60, 0x00, 0x20, 0x08, 0xa3, 0x5f, 0xc0, 0xe2, 0xf2, 0x9a, 0x0f, 0xfd,
    0xa1, 0xa8, 0x04, 0xe6, 0xe2, 0xb4, 0x74, 0xb1, 0x7b, 0x19, 0x68
};

int main() {
    std::cout << "[RUN] Testing Module 4 Hardware Decoder & Pipeline..." << std::endl;

    // 1. DecodedFramePool Initialization
    ios::video::DecodedFramePool pool;
    auto pool_init = pool.Initialize(16, 16, AV_PIX_FMT_YUV420P);
    assert(pool_init.is_ok());
    assert(pool.IsInitialized());

    // 2. HardwareDecoder Lifecycle & Parameter Guards
    ios::video::HardwareDecoder decoder(pool);
    assert(!decoder.IsInitialized());

    // Ingesting on uninitialized decoder should fail
    {
        ios::protocol::VideoSample sample;
        sample.size = sizeof(kIdrSliceBytes);
        std::memcpy(sample.data, kIdrSliceBytes, sample.size);
        auto err_res = decoder.IngestSample(sample);
        assert(err_res.is_err());
    }

    // Initialize with software fallback mode
    ios::video::DecoderConfig config{
        .hw_accel = ios::video::HardwareAccelerationType::Software,
        .thread_count = 2,
    };
    auto init_res = decoder.Initialize(
        shared::ReadOnlyBuffer(kSpsBytes, sizeof(kSpsBytes)),
        shared::ReadOnlyBuffer(kPpsBytes, sizeof(kPpsBytes)),
        config
    );
    assert(init_res.is_ok());
    assert(decoder.IsInitialized());
    assert(decoder.GetActiveAccelerationType() == ios::video::HardwareAccelerationType::Software);

    // Ingesting empty sample should fail with ParametersInvalid
    {
        auto err_res = decoder.IngestSample(shared::ReadOnlyBuffer{}, 0, false);
        assert(err_res.is_err());
        assert(err_res.error() == ios::video::DecoderError::ParametersInvalid);
    }

    // 3. Ingestion & Surface Retrieval via VideoSample
    constexpr uint64_t kTestPts1 = 1'000'000'000ULL;
    ios::protocol::VideoSample sample1;
    std::memcpy(sample1.data, kIdrSliceBytes, sizeof(kIdrSliceBytes));
    sample1.size = sizeof(kIdrSliceBytes);
    sample1.pts_nanos = kTestPts1;
    sample1.is_keyframe = true;

    auto ingest1 = decoder.IngestSample(sample1);
    assert(ingest1.is_ok());

    // Verify surface was published and can be acquired with matching metadata
    bool acquired1 = pool.AcquireLatestFrame([&](const AVFrame* frame, const ios::video::FrameMetadata& meta) {
        assert(frame != nullptr);
        assert(frame->width == 16);
        assert(frame->height == 16);
        assert(meta.pts_nanos == kTestPts1);
        assert(meta.is_keyframe == true);
        assert(meta.sequence_number == 1);
    });
    assert(acquired1);

    // 4. Ingestion via ReadOnlyBuffer overload
    constexpr uint64_t kTestPts2 = 1'033'333'333ULL;
    auto ingest2 = decoder.IngestSample(
        shared::ReadOnlyBuffer(kIdrSliceBytes, sizeof(kIdrSliceBytes)),
        kTestPts2,
        false
    );
    assert(ingest2.is_ok());

    bool acquired2 = pool.AcquireLatestFrame([&](const AVFrame* frame, const ios::video::FrameMetadata& meta) {
        assert(frame != nullptr);
        assert(meta.pts_nanos == kTestPts2);
        assert(meta.sequence_number == 2);
    });
    assert(acquired2);

    // 5. Codec Flush & Buffer Reset Verification
    decoder.Flush();
    // Subsequent frame ingestion continues properly after flush
    constexpr uint64_t kTestPts3 = 1'066'666'666ULL;
    sample1.pts_nanos = kTestPts3;
    auto ingest3 = decoder.IngestSample(sample1);
    assert(ingest3.is_ok());

    bool acquired3 = pool.AcquireLatestFrame([&](const AVFrame* frame, const ios::video::FrameMetadata& meta) {
        assert(frame != nullptr);
        assert(meta.pts_nanos == kTestPts3);
        assert(meta.sequence_number == 3);
    });
    assert(acquired3);

    // 6. Graceful Teardown
    decoder.Close();
    assert(!decoder.IsInitialized());

    // Post-close ingestion should fail
    auto ingest_post_close = decoder.IngestSample(sample1);
    assert(ingest_post_close.is_err());

    std::cout << "[PASS] All Module 4 Hardware Decoder tests completed successfully!" << std::endl;
    return 0;
}
