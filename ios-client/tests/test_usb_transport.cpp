#include "transport/usb_transport.hpp"
#include <cassert>
#include <iostream>

int main() {
    // Structural and alignment static assertions
    static_assert(sizeof(ios::transport::RawUsbBlock) == (256 * 1024) + 64, "RawUsbBlock layout mismatch");
    static_assert(alignof(ios::transport::RawUsbBlock) == 64, "RawUsbBlock must be 64-byte cacheline aligned");

    // Instantiation
    ios::transport::LibUsbTransport transport;
    assert(!transport.IsConnected());
    assert(!transport.GetActiveEndpoints().is_valid());

    // Operations on uninitialized / disconnected transport return clean errors
    uint8_t dummy[4] = {1, 2, 3, 4};
    auto send_res = transport.SendBulkData(shared::ReadOnlyBuffer(dummy, 4), 100);
    assert(send_res.is_err());
    assert(send_res.error() == ios::transport::UsbTransportError::NotConnected);

    auto trigger_res = transport.TriggerCoreMediaMode();
    assert(trigger_res.is_err());
    assert(trigger_res.error() == ios::transport::UsbTransportError::NotConnected);

    auto claim_res = transport.ClaimCoreMediaInterface();
    assert(claim_res.is_err());
    assert(claim_res.error() == ios::transport::UsbTransportError::NotConnected);

    // StopStreaming is safe when not streaming
    transport.StopStreaming();

    // Move semantics
    ios::transport::LibUsbTransport transport2(std::move(transport));
    assert(!transport2.IsConnected());

    std::cout << "[TEST] All usb_transport unit tests passed successfully!\n";
    return 0;
}
