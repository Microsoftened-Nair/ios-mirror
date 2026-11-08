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

# Aggregated test target
add_custom_target(ios_transport_tests ALL
    DEPENDS test_usbmuxd_client test_usb_transport
)
