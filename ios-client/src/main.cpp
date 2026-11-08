#include "session/ios_device_session.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_shutdown_requested{false};

void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_shutdown_requested.store(true, std::memory_order_release);
    }
}
} // namespace

int main(int argc, char* argv[]) {
    std::string target_udid;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-u" || arg == "--udid") && i + 1 < argc) {
            target_udid = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [-u|--udid <device_udid>]\n"
                      << "Options:\n"
                      << "  -u, --udid <udid>  Specify target iOS device UDID (default: auto-detect)\n"
                      << "  -h, --help         Show this help message\n";
            return 0;
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    std::cout << "========================================" << std::endl;
    std::cout << "      Unified Mirror - iOS Client       " << std::endl;
    std::cout << "========================================" << std::endl;

    if (!target_udid.empty()) {
        std::cout << "[INFO] Target device UDID: " << target_udid << std::endl;
    } else {
        std::cout << "[INFO] Auto-detecting connected iOS device..." << std::endl;
    }

    ios::session::IosDeviceSession session(target_udid);

    std::cout << "[INFO] Initializing device pairing, SDL window, and decoder..." << std::endl;
    auto init_res = session.Initialize();
    if (init_res.is_err()) {
        std::cerr << "[ERROR] Session initialization failed (error code: "
                  << static_cast<int>(init_res.error()) << ")." << std::endl;
        std::cerr << "Hints:\n"
                  << "  - Ensure your iPhone is unlocked and you tapped 'Trust This Computer'\n"
                  << "  - Verify usbmuxd is running (e.g. systemctl status usbmuxd)\n"
                  << "  - Check USB cable connection (e.g. idevice_id -l)\n";
        return 1;
    }

    std::cout << "[INFO] Connected to iOS device: " << session.GetDeviceId() << std::endl;
    std::cout << "[INFO] Starting CoreMedia screen capture stream..." << std::endl;

    auto stream_res = session.StartStreaming();
    if (stream_res.is_err()) {
        std::cerr << "[ERROR] Failed to start streaming (error code: "
                  << static_cast<int>(stream_res.error()) << ")." << std::endl;
        session.Stop();
        return 1;
    }

    std::cout << "[INFO] Streaming active! Presentation window is running." << std::endl;
    std::cout << "[INFO] Close window or press Ctrl+C to stop." << std::endl;

    // Main presentation loop
    while (session.IsRunning() && !g_shutdown_requested.load(std::memory_order_acquire)) {
        session.ProcessPresentation();
        // Give UI thread a tiny breather when waiting for next vsync frame
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "\n[INFO] Shutting down session..." << std::endl;
    session.Stop();
    std::cout << "[INFO] Teardown complete. Goodbye!" << std::endl;
    return 0;
}
