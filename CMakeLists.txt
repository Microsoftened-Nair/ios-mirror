cmake_minimum_required(VERSION 3.20)
project(ios-mirror CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local")

enable_testing()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ios-client/include
    "$ENV{HOME}/.local/include"
    "$ENV{HOME}/.local/include/libusb-1.0"
)

link_directories(
    "$ENV{HOME}/.local/lib"
)

# Common transport library
add_library(ios_transport STATIC
    ios-client/src/transport/usbmuxd_client.cpp
    ios-client/src/transport/usb_transport.cpp
)

find_library(LIBUSB_LIBRARY NAMES usb-1.0 usb PATHS "$ENV{HOME}/.local/lib")
if(LIBUSB_LIBRARY)
    target_link_libraries(ios_transport PUBLIC ${LIBUSB_LIBRARY} pthread)
else()
    target_link_libraries(ios_transport PUBLIC usb-1.0 pthread)
endif()

# Executable test_usbmuxd_client
add_executable(test_usbmuxd_client
    ios-client/tests/test_usbmuxd_client.cpp
)
target_link_libraries(test_usbmuxd_client PRIVATE ios_transport)

add_test(NAME test_usbmuxd_client COMMAND test_usbmuxd_client)

# Executable test_usb_transport
add_executable(test_usb_transport
    ios-client/tests/test_usb_transport.cpp
)
target_link_libraries(test_usb_transport PRIVATE ios_transport)

add_test(NAME test_usb_transport COMMAND test_usb_transport)

# Protocol library
add_library(ios_protocol STATIC
    ios-client/src/protocol/clock_synchronizer.cpp
    ios-client/src/protocol/stream_demuxer.cpp
)
target_link_libraries(ios_protocol PUBLIC ios_transport)

# Executable test_stream_demuxer
add_executable(test_stream_demuxer
    ios-client/tests/test_stream_demuxer.cpp
)
target_link_libraries(test_stream_demuxer PRIVATE ios_protocol)
add_test(NAME test_stream_demuxer COMMAND test_stream_demuxer)

# Presentation library
add_library(ios_presentation STATIC
    ios-client/src/presentation/sdl_renderer.cpp
)
target_link_libraries(ios_presentation PUBLIC ios_video SDL2)

# Session library
add_library(ios_session STATIC
    ios-client/src/session/device_handshake.cpp
    ios-client/src/session/feed_session.cpp
    ios-client/src/session/ios_device_session.cpp
)
target_link_libraries(ios_session PUBLIC ios_protocol ios_transport ios_video ios_presentation)

# Executable test_feed_session
add_executable(test_feed_session
    ios-client/tests/test_feed_session.cpp
)
target_compile_definitions(test_feed_session PRIVATE IOS_MIRROR_CMAKE_BUILD=1)
target_link_libraries(test_feed_session PRIVATE ios_session)

add_test(NAME test_feed_session COMMAND test_feed_session)

# Video library
add_library(ios_video STATIC
    ios-client/src/video/decoded_frame_pool.cpp
    ios-client/src/video/hardware_decoder.cpp
)
target_link_libraries(ios_video PUBLIC ios_protocol avcodec avutil)

# Executable test_hardware_decoder
add_executable(test_hardware_decoder
    ios-client/tests/test_hardware_decoder.cpp
)
target_link_libraries(test_hardware_decoder PRIVATE ios_video)
add_test(NAME test_hardware_decoder COMMAND test_hardware_decoder)

# Executable test_presentation_lifecycle
add_executable(test_presentation_lifecycle
    ios-client/tests/test_presentation_lifecycle.cpp
)
target_link_libraries(test_presentation_lifecycle PRIVATE ios_presentation ios_video)
add_test(NAME test_presentation_lifecycle COMMAND test_presentation_lifecycle)

# Main executable
add_executable(ios_mirror
    ios-client/src/main.cpp
)
target_link_libraries(ios_mirror PRIVATE ios_session)

# Aggregated test target
add_custom_target(ios_transport_tests ALL
    DEPENDS test_usbmuxd_client test_usb_transport test_stream_demuxer test_feed_session test_hardware_decoder test_presentation_lifecycle
)




