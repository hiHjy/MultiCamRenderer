cmake_minimum_required(VERSION 3.16)

project(MultiCamRenderer LANGUAGES C CXX)

option(MCR_BUILD_DEMOS "构建非 Qt 的 demo 与 DRM demo" ON)
option(MCR_BUILD_QT "构建 Qt 应用与 Qt/RGA demo" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

set(MCR_SYSROOT "${CMAKE_SYSROOT}")
if(NOT MCR_SYSROOT)
    message(FATAL_ERROR "需要通过 cmake/wsl-aarch64-toolchain.cmake 配置 RK3568 sysroot")
endif()

set(MCR_LIVE555_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/live555")

find_library(MCR_MPP_LIBRARY NAMES rockchip_mpp REQUIRED)
find_library(MCR_RGA_LIBRARY NAMES rga REQUIRED)
find_library(MCR_DRM_LIBRARY NAMES drm REQUIRED)

foreach(live555_library IN ITEMS liveMedia groupsock BasicUsageEnvironment UsageEnvironment)
    if(NOT EXISTS "${MCR_LIVE555_ROOT}/lib/aarch64/lib${live555_library}.a")
        message(FATAL_ERROR "缺少 aarch64 live555 库: lib${live555_library}.a")
    endif()
endforeach()

function(mcr_add_executable target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/drm"
        "${MCR_LIVE555_ROOT}/include/liveMedia"
        "${MCR_LIVE555_ROOT}/include/groupsock"
        "${MCR_LIVE555_ROOT}/include/BasicUsageEnvironment"
        "${MCR_LIVE555_ROOT}/include/UsageEnvironment"
        "${MCR_SYSROOT}/usr/include/rockchip"
        "${MCR_SYSROOT}/usr/include/libdrm"
        "${MCR_SYSROOT}/usr/include/rga"
    )
    target_compile_options(${target} PRIVATE -Wall -Wextra)
endfunction()

function(mcr_link_media target)
    target_link_libraries(${target} PRIVATE
        "${MCR_MPP_LIBRARY}"
        "${MCR_RGA_LIBRARY}"
        "${MCR_DRM_LIBRARY}"
        Threads::Threads
    )
endfunction()

function(mcr_link_drm target)
    target_link_libraries(${target} PRIVATE
        "${MCR_DRM_LIBRARY}"
        Threads::Threads
    )
endfunction()

function(mcr_link_live555 target)
    target_link_libraries(${target} PRIVATE
        "-Wl,--start-group"
        "${MCR_LIVE555_ROOT}/lib/aarch64/libliveMedia.a"
        "${MCR_LIVE555_ROOT}/lib/aarch64/libgroupsock.a"
        "${MCR_LIVE555_ROOT}/lib/aarch64/libBasicUsageEnvironment.a"
        "${MCR_LIVE555_ROOT}/lib/aarch64/libUsageEnvironment.a"
        "-Wl,--end-group"
        Threads::Threads
    )
endfunction()

set(MCR_MPP_DECODER_SOURCES
    src/MppTypes.cpp
    src/VideoFrame.cpp
    src/MppDecoder.cpp
    src/mpp_simple.c
    src/mpp_advance.c
)
set(MCR_MPP_ENCODER_SOURCES
    src/MppEncoder.cpp
    ${MCR_MPP_DECODER_SOURCES}
)
set_source_files_properties(
    src/mpp_simple.c
    src/mpp_advance.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-function"
)

if(MCR_BUILD_DEMOS)
    mcr_add_executable(mpp_decoder_demo
        demo/mpp_decoder_demo.cpp
        src/DmaAllocator.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(mpp_decoder_demo)

    mcr_add_executable(mpp_encoder_demo
        demo/mpp_encoder_demo.cpp
        src/DmaAllocator.cpp
        ${MCR_MPP_ENCODER_SOURCES}
    )
    mcr_link_media(mpp_encoder_demo)

    mcr_add_executable(v4l2_probe_demo
        demo/v4l2_probe_demo.cpp
        src/V4L2DeviceProbe.cpp
    )

    mcr_add_executable(dma_allocator_demo
        demo/dma_allocator_demo.cpp
        src/DmaAllocator.cpp
    )
    mcr_link_drm(dma_allocator_demo)

    mcr_add_executable(camera_capture_demo
        demo/camera_capture_demo.cpp
        src/DmaAllocator.cpp
        src/V4L2CameraSource.cpp
    )
    mcr_link_drm(camera_capture_demo)

    mcr_add_executable(frame_lease_demo demo/frame_lease/main.cpp)

    mcr_add_executable(rtsp_mpp_decoder_demo
        demo/RtspMppDecoderDemo.cpp
        src/Live555RtspClient.cpp
        src/AnnexBSink.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(rtsp_mpp_decoder_demo)
    mcr_link_live555(rtsp_mpp_decoder_demo)

    mcr_add_executable(rtsp_stream_demo
        demo/RtspStreamDemo.cpp
        src/Live555RtspClient.cpp
        src/AnnexBSink.cpp
        src/RtspStream.cpp
        src/Stream.cpp
        src/DmaAllocator.cpp
        src/DmaBufferPool.cpp
        src/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(rtsp_stream_demo)
    mcr_link_live555(rtsp_stream_demo)

    mcr_add_executable(stream_manager_demo
        demo/StreamManagerDemo.cpp
        src/StreamManager.cpp
        src/Live555RtspClient.cpp
        src/AnnexBSink.cpp
        src/RtspStream.cpp
        src/Stream.cpp
        src/FrameHub.cpp
        src/DmaAllocator.cpp
        src/DmaBufferPool.cpp
        src/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(stream_manager_demo)
    mcr_link_live555(stream_manager_demo)

    mcr_add_executable(drm_test
        drm/main.c
        drm/drm_display.c
    )
    mcr_link_drm(drm_test)

    mcr_add_executable(cam_drm_sink_demo
        drm/cam_drm_sink_demo.cpp
        drm/drm_display.c
        src/AppRuntime.cpp
        src/CamManager.cpp
        src/DmaAllocator.cpp
        src/DmaBufferPool.cpp
        src/FrameHub.cpp
        src/V4L2CameraSource.cpp
        src/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(cam_drm_sink_demo)

    mcr_add_executable(rtsp_drm_sink_demo
        drm/rtsp_drm_sink_demo.cpp
        drm/drm_display.c
        src/Live555RtspClient.cpp
        src/AnnexBSink.cpp
        src/DmaAllocator.cpp
        src/DmaBufferPool.cpp
        src/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(rtsp_drm_sink_demo)
    mcr_link_live555(rtsp_drm_sink_demo)
endif()

if(MCR_BUILD_QT)
    add_subdirectory(qt)

    mcr_add_executable(rga_ops_demo
        demo/rga/main.cpp
        src/VideoFrame.cpp
        src/DmaAllocator.cpp
        src/RgaEngine.cpp
    )
    find_package(Qt6 REQUIRED COMPONENTS Gui)
    target_link_libraries(rga_ops_demo PRIVATE Qt6::Gui "${MCR_RGA_LIBRARY}")
endif()
