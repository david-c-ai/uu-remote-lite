cmake_minimum_required(VERSION 3.18)
project(DirRemote VERSION 1.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(ANDROID OR IOS)
    option(DIR_BUILD_TOOLS "Build server and benchmark executables" OFF)
else()
    option(DIR_BUILD_TOOLS "Build server and benchmark executables" ON)
endif()

find_package(Threads REQUIRED)

# 1. DirRemote Rendezvous (hbbs) + Zero-Copy Relay (hbbr) Server
if(DIR_BUILD_TOOLS)
    add_executable(dir-server
        server-lite/src/server_runtime.cpp
        server-lite/src/main.cpp
    )
    target_include_directories(dir-server PRIVATE server-lite/include)
    target_link_libraries(dir-server PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(dir-server PRIVATE ws2_32)
    endif()
endif()

# 2. Cross-Platform Core Engine Shared Library (for Flutter dart:ffi on Win/Mac/iOS/Android/Linux)
set(DIRCORE_SOURCES
    server-lite/src/server_runtime.cpp
    core/src/crypto_e2ee.cpp
    core/src/fec_qos.cpp
    core/src/capture_hal.cpp
    core/src/codec_hal.cpp
    core/src/keymap_hal.cpp
    core/src/audio_hal.cpp
    core/src/hid_clipboard_file.cpp
    core/src/session_engine.cpp
    core/src/dircore_ffi.cpp
)

add_library(dircore SHARED ${DIRCORE_SOURCES})
target_include_directories(dircore PUBLIC core/include server-lite/include)

if(APPLE)
    find_library(FOUNDATION_FW Foundation)
    find_library(COREGRAPHICS_FW CoreGraphics)
    find_library(COREVIDEO_FW CoreVideo)
    find_library(COREMEDIA_FW CoreMedia)
    find_library(IMAGEIO_FW ImageIO)
    find_library(VIDEOTOOLBOX_FW VideoToolbox)
    find_library(AUDIOTOOLBOX_FW AudioToolbox)
    find_library(COREFOUNDATION_FW CoreFoundation)
    set_source_files_properties(core/src/capture_hal.cpp core/src/audio_hal.cpp PROPERTIES COMPILE_FLAGS "-ObjC++ -fobjc-arc")
    target_link_libraries(dircore PRIVATE
        ${FOUNDATION_FW}
        ${COREFOUNDATION_FW}
        ${COREVIDEO_FW}
        ${COREMEDIA_FW}
        ${IMAGEIO_FW}
        ${VIDEOTOOLBOX_FW}
        ${AUDIOTOOLBOX_FW}
        compression
    )
    if(NOT IOS)
        find_library(APPSERVICES_FW ApplicationServices)
        find_library(SCK_FW ScreenCaptureKit)
        target_link_libraries(dircore PRIVATE ${COREGRAPHICS_FW} ${APPSERVICES_FW} ${SCK_FW})
    endif()
elseif(WIN32)
    target_link_libraries(dircore PRIVATE ws2_32 d3d11 dxgi user32 gdi32 windowscodecs ole32 winmm)
elseif(ANDROID)
    target_link_libraries(dircore PRIVATE mediandk aaudio log)
else()
    target_link_libraries(dircore PRIVATE Threads::Threads)
endif()

# 3. End-to-End Benchmark & Verification Suite
if(DIR_BUILD_TOOLS)
    add_executable(dir-e2e-bench core/tools/uu_e2e_bench.cpp)
    target_include_directories(dir-e2e-bench PRIVATE core/include server-lite/include)
    target_link_libraries(dir-e2e-bench PRIVATE dircore)
endif()
