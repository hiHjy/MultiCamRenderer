cmake_minimum_required(VERSION 3.16)

project(MultiCamRenderer LANGUAGES C CXX)

option(MCR_BUILD_DEMOS "构建非 Qt 的 demo 与 DRM demo" ON)
option(MCR_BUILD_QT "构建 Qt 应用与 Qt/RGA demo" OFF)
option(MCR_BUILD_IPC_APP "构建 RV1126B IPC 双路 RTSP 应用" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

set(MCR_SYSROOT "${CMAKE_SYSROOT}")
if(NOT MCR_SYSROOT)
    message(FATAL_ERROR "需要通过目标板 toolchain 文件配置 CMAKE_SYSROOT")
endif()

set(MCR_LIVE555_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/live555")
set(MCR_LIVE555_TARGET "rk3568" CACHE STRING "live555 静态库目标平台（rk3568 或 rv1126b）")
set_property(CACHE MCR_LIVE555_TARGET PROPERTY STRINGS rk3568 rv1126b)
set(MCR_LIVE555_LIBRARY_DIR "${MCR_LIVE555_ROOT}/lib/aarch64-${MCR_LIVE555_TARGET}")

# RK3568 的媒体库位于 sysroot；RV1126B SDK 的媒体发布包则位于独立 media_out 目录。
# MCR_MEDIA_ROOT 仅由 RV1126B IPC 构建脚本设置，默认构建行为保持不变。
set(MCR_MEDIA_ROOT "" CACHE PATH "Rockchip media_out 根目录（含 include/ 与 lib/）")
if(MCR_MEDIA_ROOT)
    if(NOT EXISTS "${MCR_MEDIA_ROOT}/include/rockchip/mpp_frame.h" OR
       NOT EXISTS "${MCR_MEDIA_ROOT}/include/rga/im2d.h" OR
       NOT EXISTS "${MCR_MEDIA_ROOT}/include/rkaiq/uAPI2/rk_aiq_user_api2_sysctl.h" OR
       NOT EXISTS "${MCR_MEDIA_ROOT}/lib/librockchip_mpp.so" OR
       NOT EXISTS "${MCR_MEDIA_ROOT}/lib/librkaiq.so")
        message(FATAL_ERROR "MCR_MEDIA_ROOT 不完整: ${MCR_MEDIA_ROOT}")
    endif()
    set(MCR_MPP_INCLUDE_DIR "${MCR_MEDIA_ROOT}/include/rockchip")
    set(MCR_DRM_INCLUDE_DIR "${MCR_MEDIA_ROOT}/include/libdrm")
    set(MCR_RGA_INCLUDE_DIR "${MCR_MEDIA_ROOT}/include/rga")
    set(MCR_MEDIA_COMMON_INCLUDE_DIR "${MCR_MEDIA_ROOT}/include")
    set(MCR_RKAIQ_INCLUDE_DIR "${MCR_MEDIA_ROOT}/include/rkaiq")
    # media_out 中是 SDK 已发布的明确库路径，不依赖交叉 CMake 的 sysroot 搜索规则。
    set(MCR_MPP_LIBRARY "${MCR_MEDIA_ROOT}/lib/librockchip_mpp.so")
    set(MCR_RGA_LIBRARY "${MCR_MEDIA_ROOT}/lib/librga.so")
    set(MCR_DRM_LIBRARY "${MCR_MEDIA_ROOT}/lib/libdrm.so")
    set(MCR_RKAIQ_LIBRARY "${MCR_MEDIA_ROOT}/lib/librkaiq.so")
else()
    set(MCR_MPP_INCLUDE_DIR "${MCR_SYSROOT}/usr/include/rockchip")
    set(MCR_DRM_INCLUDE_DIR "${MCR_SYSROOT}/usr/include/libdrm")
    set(MCR_RGA_INCLUDE_DIR "${MCR_SYSROOT}/usr/include/rga")
    set(MCR_MEDIA_COMMON_INCLUDE_DIR "")
    find_library(MCR_MPP_LIBRARY NAMES rockchip_mpp REQUIRED)
    find_library(MCR_RGA_LIBRARY NAMES rga REQUIRED)
    find_library(MCR_DRM_LIBRARY NAMES drm REQUIRED)
endif()

foreach(live555_library IN ITEMS liveMedia groupsock BasicUsageEnvironment UsageEnvironment)
    if(NOT EXISTS "${MCR_LIVE555_LIBRARY_DIR}/lib${live555_library}.a")
        message(FATAL_ERROR "缺少 ${MCR_LIVE555_TARGET} live555 库: lib${live555_library}.a")
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
        "${MCR_MPP_INCLUDE_DIR}"
        "${MCR_DRM_INCLUDE_DIR}"
        "${MCR_RGA_INCLUDE_DIR}"
        "${MCR_MEDIA_COMMON_INCLUDE_DIR}"
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
        "${MCR_LIVE555_LIBRARY_DIR}/libliveMedia.a"
        "${MCR_LIVE555_LIBRARY_DIR}/libgroupsock.a"
        "${MCR_LIVE555_LIBRARY_DIR}/libBasicUsageEnvironment.a"
        "${MCR_LIVE555_LIBRARY_DIR}/libUsageEnvironment.a"
        "-Wl,--end-group"
        Threads::Threads
    )
endfunction()

# 项目自己的 RTSP 发布服务。一个 Server 可在同一 live555 event-loop 中注册多路
# ServerMediaSession，不会为每一路复制端口或事件线程。
set(MCR_RTSP_SERVER_SOURCES
    src/AnnexBFrameQueue.cpp
    src/AnnexBSource.cpp
    src/VideoSubsession.cpp
    src/Live555RtspServer.cpp
)
add_library(mcr_rtsp_server STATIC ${MCR_RTSP_SERVER_SOURCES})
target_include_directories(mcr_rtsp_server
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${MCR_LIVE555_ROOT}/include/liveMedia"
        "${MCR_LIVE555_ROOT}/include/groupsock"
        "${MCR_LIVE555_ROOT}/include/BasicUsageEnvironment"
        "${MCR_LIVE555_ROOT}/include/UsageEnvironment"
)
target_compile_options(mcr_rtsp_server PRIVATE -Wall -Wextra)
mcr_link_live555(mcr_rtsp_server)
if(MCR_MEDIA_ROOT)
    # RV1126B rootfs 未部署 OpenSSL；当前静态 live555 库本身已按无 TLS 模式构建，
    # 使用同一宏让公开头文件不再包含 openssl/ssl.h。
    target_compile_definitions(mcr_rtsp_server PUBLIC NO_OPENSSL)
endif()

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

if(MCR_BUILD_IPC_APP)
    mcr_add_executable(ipc_app
        src/IpcApp.cpp
        src/IspController.cpp
        src/RtspPublishSink.cpp
        src/CamManager.cpp
        src/DmaAllocator.cpp
        src/DmaBufferPool.cpp
        src/FrameHub.cpp
        src/V4L2CameraSource.cpp
        src/MppEncoder.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    target_link_libraries(ipc_app PRIVATE mcr_rtsp_server)
    target_include_directories(ipc_app PRIVATE "${MCR_RKAIQ_INCLUDE_DIR}")
    target_link_libraries(ipc_app PRIVATE "${MCR_RKAIQ_LIBRARY}")
    mcr_link_media(ipc_app)
    mcr_link_live555(ipc_app)
    # RV1126B 量产 rootfs 将 Rockchip 媒体运行库部署在此目录；交叉 SDK 中的
    # media_out 仅用于构建，不能把主机 SDK 的绝对路径写进目标程序。
    set_target_properties(ipc_app PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "/oem/usr/lib"
    )
endif()

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

    # 临时诊断工具：把 live555 已重组的完整 Annex-B NALU 原样落盘，便于区分
    # RTP/服务端问题和播放器兼容性问题。
    mcr_add_executable(rtsp_annexb_record_demo
        demo/RtspAnnexBRecordDemo.cpp
        src/Live555RtspClient.cpp
        src/AnnexBSink.cpp
    )
    mcr_link_live555(rtsp_annexb_record_demo)

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
