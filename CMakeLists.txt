cmake_minimum_required(VERSION 3.16)
project(MiniMp3Player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(NOMINMAX)

    set(CMAKE_CXX_FLAGS_RELEASE )
endif()

# 1. 查找 vcpkg 中的 imgui 和 miniaudio 头文件
find_package(imgui CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)

# 2. 查找 vcpkg 中的 FFmpeg
find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h REQUIRED)
find_library(AVFORMAT_LIBRARY avformat REQUIRED)
find_library(AVCODEC_LIBRARY avcodec REQUIRED)
find_library(SWRESAMPLE_LIBRARY swresample REQUIRED)
find_library(AVUTIL_LIBRARY avutil REQUIRED)

add_executable(MiniMp3Player
        src/main.cpp
        src/Mp3Player.cpp
        third_party/miniaudio.h
        src/AudioRingBuffer.h
)

target_include_directories(MiniMp3Player PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MINIAUDIO_INCLUDE_DIR}
        ${AVFORMAT_INCLUDE_DIR}
)

# 链接 imgui、FFmpeg 以及 Windows 系统自带的 DirectX 11 库
target_link_libraries(MiniMp3Player PRIVATE
        imgui::imgui
        d3d11
        dxgi
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${AVUTIL_LIBRARY}
)
