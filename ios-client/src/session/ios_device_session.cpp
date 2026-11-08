#include "session/ios_device_session.hpp"

#include <chrono>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace ios::session {

IosDeviceSession::IosDeviceSession(
    std::string udid,
    std::unique_ptr<FeedSession> feed_session,
    std::unique_ptr<video::DecodedFramePool> frame_pool,
    std::unique_ptr<video::HardwareDecoder> decoder,
    std::unique_ptr<presentation::SdlRenderer> renderer)
    : udid_(std::move(udid)),
      feed_session_(std::move(feed_session)),
      frame_pool_(std::move(frame_pool)),
      decoder_(std::move(decoder)),
      renderer_(std::move(renderer)) {}

IosDeviceSession::~IosDeviceSession() noexcept {
    Stop();
}

shared::Result<void, shared::DeviceSessionError> IosDeviceSession::Initialize() {
    if (is_running_.load(std::memory_order_acquire)) {
        return {};
    }

    if (!renderer_) {
        renderer_ = std::make_unique<presentation::SdlRenderer>();
    }
    if (!frame_pool_) {
        frame_pool_ = std::make_unique<video::DecodedFramePool>();
    }
    if (!decoder_) {
        decoder_ = std::make_unique<video::HardwareDecoder>(*frame_pool_);
    }
    if (!feed_session_) {
        feed_session_ = std::make_unique<FeedSession>(udid_);
    }

    presentation::RendererConfig rcfg{
        .window_title = "Unified Mirror - iOS Client",
        .initial_width = 1170 / 2,
        .initial_height = 2532 / 2,
        .vsync_enabled = true,
        .resizable = true,
    };

    auto r_res = renderer_->Initialize(rcfg);
    if (r_res.is_err()) {
        return shared::DeviceSessionError::PresentationFailed;
    }

    renderer_->SetEventCallback([this](const presentation::WindowEvent& event) {
        if (event.code == presentation::WindowEventCode::CloseRequested) {
            Stop();
        }
    });

    auto p_res = frame_pool_->Initialize(1170, 2532, AV_PIX_FMT_YUV420P);
    if (p_res.is_err()) {
        renderer_->Release();
        return shared::DeviceSessionError::InitializationFailed;
    }

    feed_session_->SetFormatListener([this](const protocol::FormatDescriptor& desc) {
        if (desc.is_valid() && decoder_) {
            video::DecoderConfig dcfg{};
            decoder_->Initialize(
                shared::ReadOnlyBuffer(desc.sps.data(), desc.sps.size()),
                shared::ReadOnlyBuffer(desc.pps.data(), desc.pps.size()),
                dcfg
            );
        }
    });

    auto connect_res = feed_session_->Connect();
    if (connect_res.is_err()) {
        frame_pool_->Release();
        renderer_->Release();
        switch (connect_res.error()) {
            case SessionError::DeviceNotConnected:
                return shared::DeviceSessionError::DeviceNotFound;
            case SessionError::PairingNotTrusted:
                return shared::DeviceSessionError::DeviceNotPaired;
            default:
                return shared::DeviceSessionError::InitializationFailed;
        }
    }

    if (udid_.empty()) {
        udid_ = feed_session_->GetResolvedUdid();
    }

    return {};
}

shared::Result<void, shared::DeviceSessionError> IosDeviceSession::StartStreaming() {
    if (is_running_.load(std::memory_order_acquire)) {
        return {};
    }

    if (!feed_session_) {
        return shared::DeviceSessionError::InitializationFailed;
    }

    is_running_.store(true, std::memory_order_release);

    decode_worker_ = std::jthread([this](std::stop_token stop_token) {
        DecodeWorkerLoop(stop_token);
    });

    StreamConfiguration stream_cfg{
        .target_fps = 60,
        .request_high_quality = true,
        .screen_width = 0,
        .screen_height = 0,
    };

    auto start_res = feed_session_->Start(stream_cfg, [this]() {
        // CoreMedia frame packet ingested into demuxer ring buffer
    });

    if (start_res.is_err()) {
        Stop();
        return shared::DeviceSessionError::CaptureStartFailed;
    }

    return {};
}

void IosDeviceSession::DecodeWorkerLoop(std::stop_token stop_token) {
    while (!stop_token.stop_requested() && is_running_.load(std::memory_order_acquire)) {
        bool sample_polled = feed_session_->PollSample([this](const protocol::VideoSample& sample) {
            if (!decoder_->IsInitialized()) {
                const auto& fmt = feed_session_->GetActiveFormat();
                if (fmt.is_valid()) {
                    video::DecoderConfig dcfg{};
                    decoder_->Initialize(
                        shared::ReadOnlyBuffer(fmt.sps.data(), fmt.sps.size()),
                        shared::ReadOnlyBuffer(fmt.pps.data(), fmt.pps.size()),
                        dcfg
                    );
                } else if (sample.is_keyframe) {
                    ExtractAndInitDecoder(sample);
                }
            }

            if (decoder_->IsInitialized()) {
                decoder_->IngestSample(sample);
            }
        });

        if (!sample_polled) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

void IosDeviceSession::ExtractAndInitDecoder(const protocol::VideoSample& sample) {
    if (sample.size == 0 || !decoder_) {
        return;
    }

    shared::ReadOnlyBuffer buf = sample.as_span();
    const uint8_t* sps_ptr = nullptr;
    size_t sps_len = 0;
    const uint8_t* pps_ptr = nullptr;
    size_t pps_len = 0;

    size_t i = 0;
    std::vector<size_t> start_positions;
    while (i + 3 < buf.size()) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
            start_positions.push_back(i + 4);
            i += 4;
        } else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            start_positions.push_back(i + 3);
            i += 3;
        } else {
            i++;
        }
    }

    for (size_t idx = 0; idx < start_positions.size(); ++idx) {
        size_t start = start_positions[idx];
        size_t end = (idx + 1 < start_positions.size())
                         ? (start_positions[idx + 1] >= 4 && buf[start_positions[idx + 1] - 4] == 0
                                ? start_positions[idx + 1] - 4
                                : start_positions[idx + 1] - 3)
                         : buf.size();
        if (start < end && end <= buf.size()) {
            uint8_t nal_type = buf[start] & 0x1F;
            if (nal_type == 7 && !sps_ptr) {
                sps_ptr = buf.data() + start;
                sps_len = end - start;
            } else if (nal_type == 8 && !pps_ptr) {
                pps_ptr = buf.data() + start;
                pps_len = end - start;
            }
        }
    }

    if (sps_ptr && pps_ptr && sps_len > 0 && pps_len > 0) {
        video::DecoderConfig cfg{};
        decoder_->Initialize(
            shared::ReadOnlyBuffer(sps_ptr, sps_len),
            shared::ReadOnlyBuffer(pps_ptr, pps_len),
            cfg
        );
    }
}

void IosDeviceSession::ProcessPresentation() {
    if (!is_running_.load(std::memory_order_acquire)) {
        return;
    }

    if (renderer_) {
        renderer_->PollEvents();
        if (!renderer_->IsWindowOpen()) {
            Stop();
            return;
        }

        if (frame_pool_) {
            renderer_->RenderFrame(*frame_pool_);
        }
    }
}

void IosDeviceSession::Stop() noexcept {
    if (!is_running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // 1. Request stop on decode_worker_ and join thread
    if (decode_worker_.joinable()) {
        decode_worker_.request_stop();
        decode_worker_.join();
    }

    // 2. Stop capture feed via feed_session_->Stop() to release USB endpoints
    if (feed_session_) {
        feed_session_->Stop();
    }

    // 3. Close FFmpeg decoder descriptors
    if (decoder_) {
        decoder_->Close();
    }

    // 4. Release SDL renderer and window resources on main thread
    if (renderer_) {
        renderer_->Release();
    }

    if (frame_pool_) {
        frame_pool_->Release();
    }
}

} // namespace ios::session
