cmake_minimum_required(VERSION 3.22.1)
project("thermaledge")

cmake_policy(SET CMP0169 OLD)
include(FetchContent)

FetchContent_Declare(
    libusb_src
    GIT_REPOSITORY https://github.com/libusb/libusb.git
    GIT_TAG v1.0.27
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(libusb_src)
if(NOT libusb_src_POPULATED)
    FetchContent_Populate(libusb_src)
endif()

add_library(usb STATIC
    ${libusb_src_SOURCE_DIR}/libusb/core.c
    ${libusb_src_SOURCE_DIR}/libusb/descriptor.c
    ${libusb_src_SOURCE_DIR}/libusb/hotplug.c
    ${libusb_src_SOURCE_DIR}/libusb/io.c
    ${libusb_src_SOURCE_DIR}/libusb/sync.c
    ${libusb_src_SOURCE_DIR}/libusb/strerror.c
    ${libusb_src_SOURCE_DIR}/libusb/os/linux_usbfs.c
    ${libusb_src_SOURCE_DIR}/libusb/os/events_posix.c
    ${libusb_src_SOURCE_DIR}/libusb/os/threads_posix.c
    ${libusb_src_SOURCE_DIR}/libusb/os/linux_netlink.c
)
target_include_directories(usb PUBLIC
    ${libusb_src_SOURCE_DIR}/android
    ${libusb_src_SOURCE_DIR}/libusb
)
target_compile_options(usb PRIVATE -fvisibility=hidden -Wno-error -Wno-unused-result)
target_link_libraries(usb PRIVATE log)

FetchContent_Declare(
    libuvc_src
    GIT_REPOSITORY https://github.com/libuvc/libuvc.git
    GIT_TAG 428b7fc495ba0637e31f6ad5c6af479aecb21163
)
FetchContent_GetProperties(libuvc_src)
if(NOT libuvc_src_POPULATED)
    FetchContent_Populate(libuvc_src)
endif()

set(libuvc_VERSION_MAJOR 0)
set(libuvc_VERSION_MINOR 0)
set(libuvc_VERSION_PATCH 7)
set(libuvc_VERSION "0.0.7")
configure_file(
    ${libuvc_src_SOURCE_DIR}/include/libuvc/libuvc_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/libuvc/libuvc_config.h
    @ONLY
)

add_library(uvc STATIC
    ${libuvc_src_SOURCE_DIR}/src/ctrl.c
    ${libuvc_src_SOURCE_DIR}/src/ctrl-gen.c
    ${libuvc_src_SOURCE_DIR}/src/device.c
    ${libuvc_src_SOURCE_DIR}/src/diag.c
    ${libuvc_src_SOURCE_DIR}/src/frame.c
    ${libuvc_src_SOURCE_DIR}/src/init.c
    ${libuvc_src_SOURCE_DIR}/src/stream.c
    ${libuvc_src_SOURCE_DIR}/src/misc.c
)
target_include_directories(uvc PUBLIC
    ${libuvc_src_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)
target_link_libraries(uvc PUBLIC usb PRIVATE log)

add_library(thermaledge SHARED native-lib.cpp)
target_link_options(thermaledge PRIVATE "-Wl,-z,max-page-size=16384")
target_link_libraries(thermaledge PRIVATE uvc usb log android)
