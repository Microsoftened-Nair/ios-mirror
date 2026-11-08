#include "session/feed_session.hpp"

#include <utility>

namespace ios::session {

FeedSession::FeedSession(std::string target_udid)
    : target_udid_(std::move(target_udid)) {
#ifndef IOS_MIRROR_STANDALONE_TEST
    usbmuxd_ = std::make_unique<transport::UsbmuxdClient>();
    transport_ = std::make_unique<transport::LibUsbTransport>();
#endif
}

FeedSession::FeedSession(std::unique_ptr<transport::UsbmuxdClient> usbmuxd,
                         std::unique_ptr<transport::IUsbTransport> transport,
                         std::unique_ptr<protocol::StreamDemuxer> demuxer,
                         std::unique_ptr<DeviceHandshake> handshake,
                         std::string target_udid)
    : target_udid_(std::move(target_udid)),
      usbmuxd_(std::move(usbmuxd)),
      transport_(std::move(transport)),
      demuxer_(std::move(demuxer)),
      handshake_(std::move(handshake)) {}

FeedSession::~FeedSession() noexcept {
    Stop();
}

shared::Result<void, SessionError> FeedSession::Connect() {
    state_.store(SessionState::VerifyingPairing, std::memory_order_release);

    if (!usbmuxd_ || !transport_) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::DeviceNotConnected;
    }

    auto conn_res = usbmuxd_->Connect();
    if (conn_res.is_err()) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::DeviceNotConnected;
    }

    auto dev_res = usbmuxd_->EnumerateDevices();
    if (dev_res.is_err() || dev_res.value().empty()) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::DeviceNotConnected;
    }

    const auto& devices = dev_res.value();
    uint32_t target_pid = 0;
    bool found = false;

    if (target_udid_.empty()) {
        resolved_udid_ = devices.front().serial_number;
        target_pid = devices.front().product_id;
        found = true;
    } else {
        for (const auto& dev : devices) {
            if (dev.serial_number == target_udid_) {
                resolved_udid_ = dev.serial_number;
                target_pid = dev.product_id;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::DeviceNotConnected;
    }

    auto pair_res = usbmuxd_->VerifyDevicePairing(resolved_udid_);
    if (pair_res.is_err() || !pair_res.value()) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::PairingNotTrusted;
    }

    state_.store(SessionState::SwitchingCoreMediaMode, std::memory_order_release);

    auto init_res = transport_->Initialize();
    if (init_res.is_err()) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::DeviceNotConnected;
    }

    auto open_res = transport_->OpenDevice(0x05AC, static_cast<uint16_t>(target_pid), resolved_udid_);
    if (open_res.is_err()) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::DeviceNotConnected;
    }

    auto mode_res = transport_->TriggerCoreMediaMode();
    if (mode_res.is_err()) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::ModeSwitchFailed;
    }

    state_.store(SessionState::ClaimingInterface, std::memory_order_release);

    auto claim_res = transport_->ClaimCoreMediaInterface();
    if (claim_res.is_err()) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::InterfaceClaimFailed;
    }

    if (!demuxer_) {
        demuxer_ = std::make_unique<protocol::StreamDemuxer>(*transport_);
    }

    if (!handshake_) {
        handshake_ = std::make_unique<DeviceHandshake>(*transport_, demuxer_->GetClockSynchronizer());
    }

    return {};
}

shared::Result<void, SessionError> FeedSession::Start(const StreamConfiguration& config, FrameAvailableCallback on_frame) {
    if (state_.load(std::memory_order_acquire) != SessionState::ClaimingInterface) {
        return SessionError::UnexpectedDisconnection;
    }

    if (!demuxer_ || !transport_ || !handshake_) {
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::UnexpectedDisconnection;
    }

    on_frame_callback_ = std::move(on_frame);

    demuxer_->Initialize([this](const protocol::FormatDescriptor& desc) {
        OnFormatChanged(desc);
    });
    demuxer_->Start();

    auto stream_res = transport_->StartStreaming([this](shared::ReadOnlyBuffer chunk) {
        OnRawUsbDataReceived(chunk);
    });
    if (stream_res.is_err()) {
        demuxer_->Stop();
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::TransportStreamingFailed;
    }

    state_.store(SessionState::NegotiatingHandshake, std::memory_order_release);

    auto hs_res = handshake_->ExecuteHandshake(config);
    if (hs_res.is_err()) {
        demuxer_->Stop();
        transport_->StopStreaming();
        state_.store(SessionState::Error, std::memory_order_release);
        return SessionError::HandshakeFailed;
    }

    state_.store(SessionState::Streaming, std::memory_order_release);
    return {};
}

bool FeedSession::PollSample(std::function<void(const protocol::VideoSample&)> consumer) {
    if (!demuxer_) {
        return false;
    }
    return demuxer_->GetOutputQueue().PopWith(std::move(consumer));
}

void FeedSession::Stop() noexcept {
    SessionState expected = state_.load(std::memory_order_acquire);
    while (expected != SessionState::Disconnected && expected != SessionState::TearingDown) {
        if (state_.compare_exchange_weak(expected, SessionState::TearingDown,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            break;
        }
    }

    if (expected == SessionState::Disconnected || expected == SessionState::TearingDown) {
        return;
    }

    if (handshake_) {
        handshake_->SetStreamEnabled(false);
    }
    if (demuxer_) {
        demuxer_->Stop();
    }
    if (transport_) {
        transport_->StopStreaming();
    }

    state_.store(SessionState::Disconnected, std::memory_order_release);
}

SessionState FeedSession::GetState() const noexcept {
    return state_.load(std::memory_order_acquire);
}

void FeedSession::SetFormatListener(FormatListener listener) {
    format_listener_ = std::move(listener);
}

const protocol::FormatDescriptor& FeedSession::GetActiveFormat() const noexcept {
    return active_format_;
}

const std::string& FeedSession::GetResolvedUdid() const noexcept {
    return resolved_udid_;
}

void FeedSession::OnRawUsbDataReceived(shared::ReadOnlyBuffer chunk) {
    SessionState s = state_.load(std::memory_order_acquire);
    if (s != SessionState::Streaming && s != SessionState::NegotiatingHandshake) {
        return;
    }

    if (demuxer_) {
        demuxer_->IngestRawData(chunk);
        if (on_frame_callback_ && !demuxer_->GetOutputQueue().Empty()) {
            on_frame_callback_();
        }
    }
}

void FeedSession::OnFormatChanged(const protocol::FormatDescriptor& desc) {
    active_format_ = desc;
    if (format_listener_) {
        format_listener_(desc);
    }
}

} // namespace ios::session
