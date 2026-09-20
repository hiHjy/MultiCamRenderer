cmake_minimum_required(VERSION 3.16)

project(MultiCamRenderer LANGUAGES C CXX)

option(MCR_BUILD_DEMOS "构建非 Qt 的 demo 与 DRM demo" ON)
option(MCR_BUILD_QT "构建 Qt 应用与 Qt/RGA demo" OFF)
option(MCR_BUILD_IPC_APP "构建 RV1126B IPC 双路 RTSP 应用" OFF)
option(MCR_BUILD_RKLLM_AGENT_SERVER "构建 RV1126B RKLLM Unix socket 推理服务" OFF)
# FreeType 目前只用于 OSD 的独立学习 demo。它依赖 RV1126B media_out 提供的动态库，
# 因此不默认开启，避免没有 FreeType 的 RK3568 sysroot 受到影响。
option(MCR_BUILD_FREETYPE_BITMAP_DEMO "构建 FreeType 字形 bitmap demo" OFF)

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

# 保持项目内 #include 仅使用文件名；各能力模块的头文件搜索路径由 target 统一提供。
set(MCR_PROJECT_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/cam"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/stream"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/memory"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/hw/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/hw/rga"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/osd"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ai"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/rtsp/client"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/rtsp/server"
)

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
    set(MCR_ALSA_INCLUDE_DIR "${MCR_MEDIA_ROOT}/include")
    set(MCR_ALSA_LIBRARY "${MCR_MEDIA_ROOT}/root/usr/lib/libasound.so.2.0.0")
    if(NOT EXISTS "${MCR_ALSA_INCLUDE_DIR}/alsa/asoundlib.h" OR
       NOT EXISTS "${MCR_ALSA_LIBRARY}")
        message(FATAL_ERROR "MCR_MEDIA_ROOT 缺少 ALSA 开发文件或动态库")
    endif()
else()
    set(MCR_MPP_INCLUDE_DIR "${MCR_SYSROOT}/usr/include/rockchip")
    set(MCR_DRM_INCLUDE_DIR "${MCR_SYSROOT}/usr/include/libdrm")
    set(MCR_RGA_INCLUDE_DIR "${MCR_SYSROOT}/usr/include/rga")
    set(MCR_MEDIA_COMMON_INCLUDE_DIR "")
    find_library(MCR_MPP_LIBRARY NAMES rockchip_mpp REQUIRED)
    find_library(MCR_RGA_LIBRARY NAMES rga REQUIRED)
    find_library(MCR_DRM_LIBRARY NAMES drm REQUIRED)
    set(MCR_ALSA_INCLUDE_DIR "${MCR_SYSROOT}/usr/include")
    find_library(MCR_ALSA_LIBRARY NAMES asound REQUIRED)
endif()

# Opus 与 WebRTC APM 都以"头文件 + 预编译静态库"的形式入库（同 third_party/live555）。
# 源码在 ~/third_party-src/ 下，用 tools/build-third-party.sh 重新生成。
set(MCR_THIRD_PARTY_ARCH "${MCR_LIVE555_TARGET}" CACHE STRING "第三方预编译库的目标板（rk3568 / rv1126b）")
set_property(CACHE MCR_THIRD_PARTY_ARCH PROPERTY STRINGS rk3568 rv1126b)

set(MCR_OPUS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/include")
set(MCR_OPUS_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/lib/aarch64-${MCR_THIRD_PARTY_ARCH}/libopus.a")
set(MCR_WEBRTC_APM_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webrtc-audio-processing/include")
set(MCR_WEBRTC_APM_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webrtc-audio-processing/lib/aarch64-${MCR_THIRD_PARTY_ARCH}/libmcr_webrtc_apm.a")

foreach(mcr_prebuilt IN ITEMS "${MCR_OPUS_LIBRARY}" "${MCR_WEBRTC_APM_LIBRARY}")
    if(NOT EXISTS "${mcr_prebuilt}")
        message(FATAL_ERROR
            "缺少预编译库 ${mcr_prebuilt}\n"
            "请先运行 tools/build-third-party.sh 生成。")
    endif()
endforeach()

foreach(live555_library IN ITEMS liveMedia groupsock BasicUsageEnvironment UsageEnvironment)
    if(NOT EXISTS "${MCR_LIVE555_LIBRARY_DIR}/lib${live555_library}.a")
        message(FATAL_ERROR "缺少 ${MCR_LIVE555_TARGET} live555 库: lib${live555_library}.a")
    endif()
endforeach()

function(mcr_add_executable target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE
        ${MCR_PROJECT_INCLUDE_DIRS}
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

function(mcr_link_alsa target)
    target_include_directories(${target} PRIVATE "${MCR_ALSA_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE "${MCR_ALSA_LIBRARY}" Threads::Threads)
endfunction()

# Opus 与 WebRTC APM 的预编译静态库。用法同 mcr_link_live555 / mcr_link_freetype：
# third_party 下只放头文件和库，链接细节收在这里。
function(mcr_link_audio_deps target)
    target_include_directories(${target} PRIVATE
        "${MCR_OPUS_INCLUDE_DIR}"
        "${MCR_WEBRTC_APM_INCLUDE_DIR}"
    )
    target_link_libraries(${target} PRIVATE
        "${MCR_WEBRTC_APM_LIBRARY}"
        "${MCR_OPUS_LIBRARY}"
        Threads::Threads
        m
        rt
    )
    # WebRTC 2.1 / Abseil 要求 C++17；RK3568 的 GCC 10.4 默认还是 C++14，
    # 不显式声明会在 std::optional 上成片报错。
    target_compile_features(${target} PRIVATE cxx_std_17)
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

# FreeType 的头文件固定随项目放在 third_party；动态库随 RV1126B SDK 的 media_out 发布。
# 以后 core/osd 接入时仍复用这个函数，不把 SDK 路径散落到业务 CMake 中。
function(mcr_link_freetype target)
    set(mcr_freetype_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype/include")
    if(NOT EXISTS "${mcr_freetype_include_dir}/ft2build.h")
        message(FATAL_ERROR "缺少项目内 FreeType 头文件: ${mcr_freetype_include_dir}/ft2build.h")
    endif()

    if(MCR_MEDIA_ROOT)
        set(mcr_freetype_library "${MCR_MEDIA_ROOT}/lib/libfreetype.so")
        if(NOT EXISTS "${mcr_freetype_library}")
            message(FATAL_ERROR "MCR_MEDIA_ROOT 缺少 FreeType 动态库: ${mcr_freetype_library}")
        endif()
    else()
        # 保留非 RV1126B 平台的可用性；当前 RK3568 sysroot 没有该库，所以仅在显式
        # 开启此 demo 时才会走到这里并清晰报错。
        find_library(mcr_freetype_library NAMES freetype REQUIRED)
    endif()

    target_include_directories(${target} PRIVATE "${mcr_freetype_include_dir}")
    target_link_libraries(${target} PRIVATE "${mcr_freetype_library}")
endfunction()

function(mcr_link_png target)
    if(MCR_MEDIA_ROOT)
        set(mcr_png_library "${MCR_MEDIA_ROOT}/lib/libpng.so")
        set(mcr_zlib_library "${MCR_MEDIA_ROOT}/lib/libz.so")
        if(NOT EXISTS "${mcr_png_library}")
            message(FATAL_ERROR "MCR_MEDIA_ROOT 缺少 libpng 动态库: ${mcr_png_library}")
        endif()
        if(NOT EXISTS "${mcr_zlib_library}")
            message(FATAL_ERROR "MCR_MEDIA_ROOT 缺少 libz 动态库: ${mcr_zlib_library}")
        endif()
    else()
        find_library(mcr_png_library NAMES png png16 REQUIRED)
        find_library(mcr_zlib_library NAMES z REQUIRED)
    endif()
    # libpng 的 SDK 动态库没有把 zlib 作为可自动传递的链接依赖，显式写出。
    target_link_libraries(${target} PRIVATE "${mcr_png_library}" "${mcr_zlib_library}")
endfunction()

# AI 编排核心不依赖 RKLLM、Python 或任何硬件库。真实模型适配器和设备工具实现都通过
# 小接口注入，便于先做确定性 demo，再接入板端运行时。
add_library(mcr_ai_agent STATIC
    src/ai/LocalAgent.cpp
)
target_include_directories(mcr_ai_agent PUBLIC ${MCR_PROJECT_INCLUDE_DIRS})
target_compile_options(mcr_ai_agent PRIVATE -Wall -Wextra)

# RKLLM 只在 RV1126B SDK 中发布，不应影响默认 RK3568 构建。明确开启后，由调用方提供
# 与模型 runtime 相同版本的 librkllm_api 根目录。
set(MCR_RKLLM_API_ROOT "" CACHE PATH "RKLLM librkllm_api 根目录（含 include/ 与 aarch64/）")
if(MCR_BUILD_RKLLM_AGENT_SERVER)
    if(NOT EXISTS "${MCR_RKLLM_API_ROOT}/include/rkllm.h" OR
       NOT EXISTS "${MCR_RKLLM_API_ROOT}/aarch64/librkllmrt.so")
        message(FATAL_ERROR "MCR_RKLLM_API_ROOT 缺少 RKLLM 头文件或 AArch64 运行库")
    endif()
    mcr_add_executable(rkllm_agent_server src/ai/RkllmAgentServer.cpp)
    target_include_directories(rkllm_agent_server PRIVATE "${MCR_RKLLM_API_ROOT}/include")
    target_link_libraries(rkllm_agent_server PRIVATE "${MCR_RKLLM_API_ROOT}/aarch64/librkllmrt.so")
endif()

# 项目自己的 RTSP 发布服务。一个 Server 可在同一 live555 event-loop 中注册多路
# ServerMediaSession，不会为每一路复制端口或事件线程。
set(MCR_RTSP_SERVER_SOURCES
    src/rtsp/server/AnnexBFrameQueue.cpp
    src/rtsp/server/AnnexBSource.cpp
    src/rtsp/server/VideoSubsession.cpp
    src/rtsp/server/Live555RtspServer.cpp
)
add_library(mcr_rtsp_server STATIC ${MCR_RTSP_SERVER_SOURCES})
target_include_directories(mcr_rtsp_server
    PUBLIC
        ${MCR_PROJECT_INCLUDE_DIRS}
    PRIVATE
        "${MCR_LIVE555_ROOT}/include/liveMedia"
        "${MCR_LIVE555_ROOT}/include/groupsock"
        "${MCR_LIVE555_ROOT}/include/BasicUsageEnvironment"
        "${MCR_LIVE555_ROOT}/include/UsageEnvironment"
        "${MCR_MPP_INCLUDE_DIR}"
        "${MCR_RGA_INCLUDE_DIR}"
        "${MCR_MEDIA_COMMON_INCLUDE_DIR}"
)
target_compile_options(mcr_rtsp_server PRIVATE -Wall -Wextra)
mcr_link_live555(mcr_rtsp_server)
if(MCR_MEDIA_ROOT)
    # RV1126B rootfs 未部署 OpenSSL；当前静态 live555 库本身已按无 TLS 模式构建，
    # 使用同一宏让公开头文件不再包含 openssl/ssl.h。
    target_compile_definitions(mcr_rtsp_server PUBLIC NO_OPENSSL)
endif()

set(MCR_MPP_DECODER_SOURCES
    src/core/hw/codec/MppTypes.cpp
    src/core/common/VideoFrame.cpp
    src/core/hw/codec/MppDecoder.cpp
    src/core/hw/codec/mpp_simple.c
    src/core/hw/codec/mpp_advance.c
)
set(MCR_MPP_ENCODER_SOURCES
    src/core/hw/codec/MppEncoder.cpp
    ${MCR_MPP_DECODER_SOURCES}
)
set_source_files_properties(
    src/core/hw/codec/mpp_simple.c
    src/core/hw/codec/mpp_advance.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-function"
)

# C 层的通用音频采集链：ALSA PCM 采集 -> 可替换编码器接口。当前实现 Opus，后续
# G.711/AAC 只需要新增 AudioEncoderOps，不需要把采集线程或 RTSP 逻辑绑定到具体编码器。
add_library(mcr_audio_core STATIC
    src/core/audio/AudioCapture.c
    src/core/audio/AudioApm.cpp
    src/core/audio/AudioCodec.c
    src/core/audio/OpusEncoder.c
    src/core/audio/AudioCaptureManager.c
    src/core/audio/AudioDecoder.c
    src/core/audio/OpusDecoder.c
    src/core/audio/AudioPlayback.c
    src/core/audio/AudioPlaybackManager.c
    src/core/audio/AudioPacketFile.c
)
target_include_directories(mcr_audio_core
    PUBLIC ${MCR_PROJECT_INCLUDE_DIRS}
    PRIVATE "${MCR_ALSA_INCLUDE_DIR}"
)
target_compile_options(mcr_audio_core PRIVATE -Wall -Wextra)
mcr_link_audio_deps(mcr_audio_core)
target_link_libraries(mcr_audio_core PUBLIC
    "${MCR_ALSA_LIBRARY}"
    Threads::Threads
)

if(MCR_BUILD_IPC_APP)
    mcr_add_executable(ipc_app
        app/IpcApp.cpp
        src/core/cam/IspController.cpp
        src/rtsp/server/RtspPublishSink.cpp
        src/core/cam/CamManager.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/common/FrameHub.cpp
        src/core/cam/V4L2CameraSource.cpp
        src/core/osd/FreeTypeTextRenderer.cpp
        src/core/osd/OsdRenderer.cpp
        src/core/hw/codec/MppEncoder.cpp
        src/core/hw/rga/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    target_link_libraries(ipc_app PRIVATE mcr_rtsp_server)
    target_include_directories(ipc_app PRIVATE "${MCR_RKAIQ_INCLUDE_DIR}")
    target_link_libraries(ipc_app PRIVATE "${MCR_RKAIQ_LIBRARY}")
    mcr_link_freetype(ipc_app)
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
    # 不加载任何模型：用假模型和假工具把 LocalAgent 的两次推理、严格校验和真实结果总结
    # 跑完整，方便先理解 Agent 的职责边界。
    mcr_add_executable(ai_agent_flow_demo demo/ai_agent_flow_demo.cpp)
    target_link_libraries(ai_agent_flow_demo PRIVATE mcr_ai_agent)

    # 不需要摄像头、MPP 或 RTSP：用于板端验证自动发现 ALSA 采集设备、PCM 时钟和
    # 20ms Opus 编码包。按 Ctrl+C 正常 flush/释放设备。
    mcr_add_executable(audio_capture_opus_demo demo/AudioCaptureOpusDemo.c)
    target_link_libraries(audio_capture_opus_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_capture_opus_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 采集 -> APM(AGC) -> 写 WAV，不经过 Opus。用于直接听/量 AGC 处理后的效果，
    # 例如判断板端麦克风电平偏低时 AGC 究竟能补回多少 dB。
    mcr_add_executable(audio_capture_apm_pcm_demo demo/AudioCaptureApmPcmDemo.c)
    target_link_libraries(audio_capture_apm_pcm_demo PRIVATE mcr_audio_core m)
    set_target_properties(audio_capture_apm_pcm_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 读取 audio_capture_opus_demo 写出的 packet 文件，完成 Opus -> PCM -> ALSA 播放闭环。
    mcr_add_executable(audio_opus_playback_demo demo/AudioOpusPlaybackDemo.c)
    target_link_libraries(audio_opus_playback_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_opus_playback_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 同一份 Opus packet 文件走正式 PlaybackManager：有界队列 -> 单播放线程 ->
    # 解码 -> 10ms PCM -> ALSA。它不引入 jitter buffer，网络时序由调用方负责。
    mcr_add_executable(audio_playback_manager_demo demo/AudioPlaybackManagerDemo.c)
    target_link_libraries(audio_playback_manager_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_playback_manager_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 不依赖 ALSA 设备的 WebRTC AGC 自检：用固定低电平 PCM 验证处理后的实际增益。
    mcr_add_executable(audio_apm_gain_demo demo/AudioApmGainDemo.c)
    target_link_libraries(audio_apm_gain_demo PRIVATE mcr_audio_core m)
    set_target_properties(audio_apm_gain_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 不依赖声卡的 AEC3 调用自检：验证 10ms reverse reference 与 capture 能被同一 APM
    # 实例正确处理；不用于评价真实声学消回声效果。
    mcr_add_executable(audio_apm_aec_smoke_demo demo/AudioApmAecSmokeDemo.c)
    target_link_libraries(audio_apm_aec_smoke_demo PRIVATE mcr_audio_core m)
    set_target_properties(audio_apm_aec_smoke_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 板端诊断工具：不修改 mixer，只列出 codec 实际提供的采集/播放控件与当前值。
    mcr_add_executable(audio_mixer_inspect_demo demo/AudioMixerInspectDemo.c)
    mcr_link_alsa(audio_mixer_inspect_demo)
    set_target_properties(audio_mixer_inspect_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    mcr_add_executable(mpp_decoder_demo
        demo/mpp_decoder_demo.cpp
        src/core/memory/DmaAllocator.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(mpp_decoder_demo)

    mcr_add_executable(mpp_encoder_demo
        demo/mpp_encoder_demo.cpp
        src/core/memory/DmaAllocator.cpp
        ${MCR_MPP_ENCODER_SOURCES}
    )
    mcr_link_media(mpp_encoder_demo)

    mcr_add_executable(v4l2_probe_demo
        demo/v4l2_probe_demo.cpp
        src/core/cam/V4L2DeviceProbe.cpp
    )

    mcr_add_executable(dma_allocator_demo
        demo/dma_allocator_demo.cpp
        src/core/memory/DmaAllocator.cpp
    )
    mcr_link_drm(dma_allocator_demo)

    mcr_add_executable(camera_capture_demo
        demo/camera_capture_demo.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/cam/V4L2CameraSource.cpp
    )
    mcr_link_drm(camera_capture_demo)

    # 使用真实 V4L2 设备验证 CamManager 的轻恢复、完整重建、以及 lease 未归还时的
    # stop/start 与自动恢复行为；physical 参数模式可额外验证 USB 摄像头真实拔插。
    mcr_add_executable(cam_recovery_stress_demo
        demo/CamRecoveryStressDemo.cpp
        src/core/cam/CamManager.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/common/FrameHub.cpp
        src/core/cam/V4L2CameraSource.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(cam_recovery_stress_demo)

    mcr_add_executable(frame_lease_demo demo/frame_lease/main.cpp)

    mcr_add_executable(rtsp_mpp_decoder_demo
        demo/RtspMppDecoderDemo.cpp
        src/rtsp/client/Live555RtspClient.cpp
        src/rtsp/client/AnnexBSink.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(rtsp_mpp_decoder_demo)
    mcr_link_live555(rtsp_mpp_decoder_demo)

    # 临时诊断工具：把 live555 已重组的完整 Annex-B NALU 原样落盘，便于区分
    # RTP/服务端问题和播放器兼容性问题。
    mcr_add_executable(rtsp_annexb_record_demo
        demo/RtspAnnexBRecordDemo.cpp
        src/rtsp/client/Live555RtspClient.cpp
        src/rtsp/client/AnnexBSink.cpp
    )
    mcr_link_live555(rtsp_annexb_record_demo)

    mcr_add_executable(rtsp_stream_demo
        demo/RtspStreamDemo.cpp
        src/rtsp/client/Live555RtspClient.cpp
        src/rtsp/client/AnnexBSink.cpp
        src/rtsp/client/RtspStream.cpp
        src/core/stream/Stream.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/hw/rga/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(rtsp_stream_demo)
    mcr_link_live555(rtsp_stream_demo)

    mcr_add_executable(stream_manager_demo
        demo/StreamManagerDemo.cpp
        src/core/stream/StreamManager.cpp
        src/rtsp/client/Live555RtspClient.cpp
        src/rtsp/client/AnnexBSink.cpp
        src/rtsp/client/RtspStream.cpp
        src/core/stream/Stream.cpp
        src/core/common/FrameHub.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/hw/rga/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(stream_manager_demo)
    mcr_link_live555(stream_manager_demo)

    # 不依赖网络或摄像头，直接验证 Stream 的恢复门控、超时上报、合并 Annex-B 恢复边界
    # 与 StreamManager Error -> Streaming 状态回转。
    mcr_add_executable(stream_recovery_demo
        demo/StreamRecoveryDemo.cpp
        src/core/stream/StreamManager.cpp
        src/core/stream/Stream.cpp
        src/core/common/FrameHub.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/hw/rga/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(stream_recovery_demo)

    mcr_add_executable(drm_test
        drm/main.c
        drm/drm_display.c
    )
    mcr_link_drm(drm_test)

    mcr_add_executable(cam_drm_sink_demo
        drm/cam_drm_sink_demo.cpp
        drm/drm_display.c
        src/core/runtime/AppRuntime.cpp
        src/core/cam/CamManager.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/common/FrameHub.cpp
        src/core/cam/V4L2CameraSource.cpp
        src/core/hw/rga/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(cam_drm_sink_demo)

    mcr_add_executable(rtsp_drm_sink_demo
        drm/rtsp_drm_sink_demo.cpp
        drm/drm_display.c
        src/rtsp/client/Live555RtspClient.cpp
        src/rtsp/client/AnnexBSink.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/hw/rga/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(rtsp_drm_sink_demo)
    mcr_link_live555(rtsp_drm_sink_demo)
endif()

if(MCR_BUILD_FREETYPE_BITMAP_DEMO)
    # 独立验证 OSD 的完整路径：FreeType 小 RGBA DMA-BUF -> RGA 合成/画框 -> PNG。
    # 后续 RtspPublishSink 只复用 core/osd，不再散落原生 RGA 调用。
    mcr_add_executable(freetype_bitmap_demo
        demo/FreeTypeBitmapDemo.cpp
        src/core/osd/FreeTypeTextRenderer.cpp
        src/core/osd/OsdRenderer.cpp
        src/core/common/VideoFrame.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/hw/rga/RgaEngine.cpp
    )
    mcr_link_freetype(freetype_bitmap_demo)
    mcr_link_png(freetype_bitmap_demo)
    mcr_link_media(freetype_bitmap_demo)
    if(MCR_MEDIA_ROOT)
        set_target_properties(freetype_bitmap_demo PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "/oem/usr/lib"
        )
    endif()
endif()

if(MCR_BUILD_QT)
    add_subdirectory(qt)

    mcr_add_executable(rga_ops_demo
        demo/rga/main.cpp
        src/core/common/VideoFrame.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/hw/rga/RgaEngine.cpp
    )
    find_package(Qt6 REQUIRED COMPONENTS Gui)
    target_link_libraries(rga_ops_demo PRIVATE Qt6::Gui "${MCR_RGA_LIBRARY}")
endif()
