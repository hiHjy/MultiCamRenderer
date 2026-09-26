cmake_minimum_required(VERSION 3.16)

project(MultiCamRenderer LANGUAGES C CXX)

option(MCR_BUILD_DEMOS "构建非 Qt 的 demo 与 DRM demo" ON)
option(MCR_BUILD_QT "构建 Qt 应用与 Qt/RGA demo" OFF)
option(MCR_BUILD_IPC_APP "构建 RV1126B IPC 双路 RTSP 应用" OFF)
# IPC 默认使用 AVBR 参数组；CBR 参数组仍完整保留，可用 -DMCR_MPP_ENCODER_USE_AVBR=OFF 显式切换。
# 两组参数彼此独立，调试 AVBR 不会意外改变 CBR 基线。
option(MCR_MPP_ENCODER_USE_AVBR "IPC H264/H265 使用 AVBR 参数组" ON)
option(MCR_MPP_ENCODER_DEBUG_IDR "编码实验：记录每组 MPP intra packet 的大小" OFF)
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
set(MCR_GSOAP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gsoap")
set(MCR_ONVIF_WSDD_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/onvif/ws-discovery")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/include/onvif"
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

# ONVIF 的 HTTP Digest 使用 gSOAP 官方 httpda 插件。即使设备当前允许匿名访问，
# 客户端也需要它才能连接开启 Digest 的第三方设备，因此 ONVIF binding 始终链接 OpenSSL。
# RV1126B 的媒体 SDK 不把 OpenSSL 放在 media_out，而是由完整 rootfs 提供，故 IPC
# 构建脚本传入一个显式根目录。
set(MCR_ONVIF_OPENSSL_ROOT "" CACHE PATH "ONVIF HTTP Digest 所用目标端 OpenSSL 根目录")
if(MCR_ONVIF_OPENSSL_ROOT)
    set(MCR_ONVIF_OPENSSL_INCLUDE_DIR "${MCR_ONVIF_OPENSSL_ROOT}/include")
    set(MCR_ONVIF_SSL_LIBRARY "${MCR_ONVIF_OPENSSL_ROOT}/lib/libssl.so")
    set(MCR_ONVIF_CRYPTO_LIBRARY "${MCR_ONVIF_OPENSSL_ROOT}/lib/libcrypto.so")
    if(NOT EXISTS "${MCR_ONVIF_OPENSSL_INCLUDE_DIR}/openssl/evp.h" OR
       NOT EXISTS "${MCR_ONVIF_SSL_LIBRARY}" OR
       NOT EXISTS "${MCR_ONVIF_CRYPTO_LIBRARY}")
        message(FATAL_ERROR "MCR_ONVIF_OPENSSL_ROOT 不完整：需要 include/openssl、lib/libssl.so、lib/libcrypto.so")
    endif()
else()
    find_path(MCR_ONVIF_OPENSSL_INCLUDE_DIR openssl/evp.h REQUIRED)
    find_library(MCR_ONVIF_SSL_LIBRARY NAMES ssl REQUIRED)
    find_library(MCR_ONVIF_CRYPTO_LIBRARY NAMES crypto REQUIRED)
endif()

# Opus 与 WebRTC APM 都以"头文件 + 预编译静态库"的形式入库（同 third_party/live555）。
# 源码在 ~/third_party-src/ 下，用 tools/build-third-party.sh 重新生成。
set(MCR_THIRD_PARTY_ARCH "${MCR_LIVE555_TARGET}" CACHE STRING "第三方预编译库的目标板（rk3568 / rv1126b）")
set_property(CACHE MCR_THIRD_PARTY_ARCH PROPERTY STRINGS rk3568 rv1126b)

set(MCR_OPUS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/include")
set(MCR_OPUS_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/lib/aarch64-${MCR_THIRD_PARTY_ARCH}/libopus.a")
set(MCR_FDK_AAC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fdk-aac/include")
set(MCR_FDK_AAC_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fdk-aac/lib/aarch64-${MCR_THIRD_PARTY_ARCH}/libfdk-aac.a")
set(MCR_WEBRTC_APM_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webrtc-audio-processing/include")
set(MCR_WEBRTC_APM_LIBRARY
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webrtc-audio-processing/lib/aarch64-${MCR_THIRD_PARTY_ARCH}/libmcr_webrtc_apm.a")

foreach(mcr_prebuilt IN ITEMS "${MCR_OPUS_LIBRARY}" "${MCR_FDK_AAC_LIBRARY}" "${MCR_WEBRTC_APM_LIBRARY}")
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
        "${MCR_FDK_AAC_INCLUDE_DIR}"
        "${MCR_WEBRTC_APM_INCLUDE_DIR}"
    )
    target_link_libraries(${target} PRIVATE
        "${MCR_WEBRTC_APM_LIBRARY}"
        "${MCR_OPUS_LIBRARY}"
        "${MCR_FDK_AAC_LIBRARY}"
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

# 自动生成检查：当本地缺少 gSOAP / ONVIF 自动绑定代码时，自动调用脚本生成。
# 这样 Git 仓库无需提交 20MB 的 generated/ 目录，克隆后初次 CMake 会自动补齐。
set(MCR_ONVIF_DEVICE_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/onvif/device")
set(MCR_ONVIF_WSDD_GENERATED_C "${MCR_ONVIF_WSDD_GENERATED_DIR}/wsddC.c")
set(MCR_ONVIF_DEVICE_GENERATED_C "${MCR_ONVIF_DEVICE_GENERATED_DIR}/onvifC.c")

if(NOT EXISTS "${MCR_ONVIF_WSDD_GENERATED_C}")
    message(STATUS "未检测到 ONVIF WS-Discovery 生成代码，正在自动生成...")
    execute_process(
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-onvif-wsdd.sh"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _wsdd_res
    )
    if(NOT _wsdd_res EQUAL 0)
        message(FATAL_ERROR "自动生成 ONVIF WS-Discovery 绑定失败，返回值: ${_wsdd_res}")
    endif()
endif()

if(NOT EXISTS "${MCR_ONVIF_DEVICE_GENERATED_C}")
    message(STATUS "未检测到 ONVIF Device/Media 生成代码，正在自动生成...")
    execute_process(
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-onvif-device-bindings.sh"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _device_res
    )
    if(NOT _device_res EQUAL 0)
        message(FATAL_ERROR "自动生成 ONVIF Device/Media 绑定失败，返回值: ${_device_res}")
    endif()
endif()

# ONVIF 的 WS-Discovery 是低频 UDP 控制面。gSOAP 的生成代码与 runtime 单独收为
# 一个静态库，避免污染现有 live555/媒体链路，也避免未来 Device/Media SOAP binding
# 与 WSDD 使用不同 soapH 文件时相互干扰。
add_library(mcr_gsoap_wsdd STATIC
    third_party/gsoap/src/stdsoap2.c
    third_party/gsoap/plugin/threads.c
    third_party/gsoap/plugin/wsaapi.c
    third_party/gsoap/plugin/wsddapi.c
    generated/onvif/ws-discovery/wsddC.c
    generated/onvif/ws-discovery/wsddClient.c
    generated/onvif/ws-discovery/wsddNsmap.c
)
target_include_directories(mcr_gsoap_wsdd
    PUBLIC
        "${MCR_GSOAP_ROOT}/include"
        "${MCR_GSOAP_ROOT}/plugin"
        "${MCR_ONVIF_WSDD_GENERATED_DIR}"
)
# wsaapi.h 内部会对 SOAP_H_FILE 做字符串化，因此这里必须传 token，不能传字符串。
target_compile_definitions(mcr_gsoap_wsdd PRIVATE SOAP_H_FILE=wsddH.h)
target_compile_options(mcr_gsoap_wsdd PRIVATE -Wall -Wextra)
target_link_libraries(mcr_gsoap_wsdd PUBLIC Threads::Threads)

# 纯 ONVIF 客户端 binding：含 Discovery、Device/Media/Media2 的客户端桩，刻意不含
# OnvifSoapService.c 与生成的 onvifServer.c。后两者拥有 Target Service 的全局
# wsdd_event_* 回调，不能与客户端 ProbeMatch 回调出现在同一进程。
add_library(mcr_onvif_client_gsoap_bindings STATIC
    third_party/gsoap/src/stdsoap2.c
    third_party/gsoap/plugin/threads.c
    third_party/gsoap/plugin/wsaapi.c
    third_party/gsoap/plugin/wsddapi.c
    third_party/gsoap/plugin/httpda.c
    third_party/gsoap/plugin/smdevp.c
    generated/onvif/device/onvifC.c
    generated/onvif/device/onvifClient.c
    generated/onvif/device/onvifNsmap.c
    src/onvif/OnvifWsddClient.c
    src/onvif/OnvifDeviceClient.c
)
target_include_directories(mcr_onvif_client_gsoap_bindings PUBLIC
    "${MCR_GSOAP_ROOT}/include"
    "${MCR_GSOAP_ROOT}/plugin"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/onvif/device"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/onvif"
)
target_compile_definitions(mcr_onvif_client_gsoap_bindings PRIVATE SOAP_H_FILE=onvifH.h)
target_compile_definitions(mcr_onvif_client_gsoap_bindings PRIVATE WITH_OPENSSL)
target_include_directories(mcr_onvif_client_gsoap_bindings PRIVATE "${MCR_ONVIF_OPENSSL_INCLUDE_DIR}")
target_link_libraries(mcr_onvif_client_gsoap_bindings PUBLIC
    "${MCR_ONVIF_SSL_LIBRARY}" "${MCR_ONVIF_CRYPTO_LIBRARY}")
target_compile_options(mcr_onvif_client_gsoap_bindings PRIVATE -Wall -Wextra)
target_link_libraries(mcr_onvif_client_gsoap_bindings PUBLIC Threads::Threads)

# ONVIF 客户端协议层是纯 C：不泄漏 gSOAP 类型；它与 OnvifServer 是两个可独立链接的
# 角色库，NVR/客户端进程链接前者，IPC Target Service 链接后者。
add_library(mcr_onvif_discovery STATIC
    src/onvif/OnvifDiscoveryClient.c
)
target_include_directories(mcr_onvif_discovery PUBLIC ${MCR_PROJECT_INCLUDE_DIRS})
target_link_libraries(mcr_onvif_discovery PUBLIC mcr_onvif_client_gsoap_bindings Threads::Threads)

# This is intentionally separate from mcr_gsoap_wsdd while the discovery demo
# remains standalone.  The future OnvifServer will link only this combined
# Device-Service + WS-Discovery binding, which owns one gSOAP runtime and one
# global XML namespace table.
add_library(mcr_onvif_gsoap_bindings STATIC
    third_party/gsoap/src/stdsoap2.c
    third_party/gsoap/plugin/threads.c
    third_party/gsoap/plugin/wsaapi.c
    third_party/gsoap/plugin/wsddapi.c
    third_party/gsoap/plugin/httpda.c
    third_party/gsoap/plugin/smdevp.c
    generated/onvif/device/onvifC.c
    generated/onvif/device/onvifClient.c
    generated/onvif/device/onvifServer.c
    generated/onvif/device/onvifNsmap.c
    generated/onvif/device/onvifUnimplementedOps.c
    src/onvif/OnvifSoapService.c
)
target_include_directories(mcr_onvif_gsoap_bindings PUBLIC
    "${MCR_GSOAP_ROOT}/include"
    "${MCR_GSOAP_ROOT}/plugin"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/onvif/device"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/onvif"
)
target_compile_definitions(mcr_onvif_gsoap_bindings PRIVATE SOAP_H_FILE=onvifH.h)
target_compile_definitions(mcr_onvif_gsoap_bindings PRIVATE WITH_OPENSSL)
target_include_directories(mcr_onvif_gsoap_bindings PRIVATE "${MCR_ONVIF_OPENSSL_INCLUDE_DIR}")
target_link_libraries(mcr_onvif_gsoap_bindings PUBLIC
    "${MCR_ONVIF_SSL_LIBRARY}" "${MCR_ONVIF_CRYPTO_LIBRARY}")
target_compile_options(mcr_onvif_gsoap_bindings PRIVATE -Wall -Wextra)
target_link_libraries(mcr_onvif_gsoap_bindings PUBLIC Threads::Threads)

# 应用层只依赖这个 C++ 对象；gSOAP 的全局 C handler 与完整 WSDL 生成代码都留在
# mcr_onvif_gsoap_bindings 内部，避免把协议生成细节扩散到 IpcApp。
add_library(mcr_onvif_server STATIC
    src/onvif/OnvifServer.cpp
)
target_include_directories(mcr_onvif_server PUBLIC ${MCR_PROJECT_INCLUDE_DIRS})
target_link_libraries(mcr_onvif_server PUBLIC mcr_onvif_gsoap_bindings Threads::Threads)

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

# 项目自己的 RTSP 发布服务。一个 Server 可在同一 live555 event-loop 中注册多路
# ServerMediaSession，不会为每一路复制端口或事件线程。
set(MCR_RTSP_SERVER_SOURCES
    src/rtsp/server/AnnexBFrameQueue.cpp
    src/rtsp/server/AnnexBSource.cpp
    src/rtsp/server/AudioAccessUnitQueue.cpp
    src/rtsp/server/AudioAccessUnitSource.cpp
    src/rtsp/server/AacAudioSubsession.cpp
    src/rtsp/server/VideoSubsession.cpp
    src/rtsp/server/Live555RtspServer.cpp
    src/rtsp/server/RtspAudioPublishSink.cpp
)

# 所有 RTSP 拉流 demo 共用同一组 source；AAC 接收 sink 在这里集中列出，避免某个
# demo 更新 Live555RtspClient 后遗漏链接实现。
set(MCR_RTSP_CLIENT_SOURCES
    src/rtsp/client/Live555RtspClient.cpp
    src/rtsp/client/AnnexBSink.cpp
    src/rtsp/client/AacAudioSink.cpp
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

# C 层只保留同步、单职责媒体工具；C++ AudioPipeline 在其上组织 Hub/Node 图。
add_library(mcr_audio_core STATIC
    src/core/audio/AudioCapture.c
    src/core/audio/AudioApm.cpp
    src/core/audio/AudioCodec.c
    src/core/audio/AacCodec.c
    src/core/audio/AacEncoder.c
    src/core/audio/OpusEncoder.c
    src/core/audio/AudioDecoder.c
    src/core/audio/AacDecoder.c
    src/core/audio/OpusDecoder.c
    src/core/audio/AudioPlayback.c
    src/core/audio/AudioPacketFile.c
    src/core/audio/AudioFrame.cpp
    src/core/audio/AudioPipeline.cpp
    src/core/audio/AudioDiagnosticCapture.cpp
    src/core/audio/AudioPlaybackPipeline.cpp
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

# RTSP AAC subsession 通过 AudioAac 的 AudioSpecificConfig 生成 SDP；声明为 PUBLIC 后，
# 凡是链接 mcr_rtsp_server 的 demo 都会自动带上该实现，不会出现静态库链接顺序遗漏。
target_link_libraries(mcr_rtsp_server PUBLIC mcr_audio_core)

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
    # mcr_rtsp_server 公开依赖 mcr_audio_core（AAC SDP 与 source 所需）；IPC 本身也通过
    # 这条传递依赖使用 AudioPipeline，避免在每个 app 重复列同一个音频库。
    target_link_libraries(ipc_app PRIVATE mcr_rtsp_server mcr_onvif_server)
    target_include_directories(ipc_app PRIVATE "${MCR_RKAIQ_INCLUDE_DIR}")
    target_link_libraries(ipc_app PRIVATE "${MCR_RKAIQ_LIBRARY}")
    mcr_link_freetype(ipc_app)
    mcr_link_media(ipc_app)
    mcr_link_live555(ipc_app)
    if(MCR_MPP_ENCODER_USE_AVBR)
        target_compile_definitions(ipc_app PRIVATE MCR_MPP_ENCODER_USE_AVBR=1)
    else()
        # 显式传 0，避免 mpp_simple.c 的独立编译默认值覆盖 CMake 的 CBR 选择。
        target_compile_definitions(ipc_app PRIVATE MCR_MPP_ENCODER_USE_AVBR=0)
    endif()
    if(MCR_MPP_ENCODER_DEBUG_IDR)
        # 默认关闭，正常实时路径不生成关键帧统计日志代码。
        target_compile_definitions(ipc_app PRIVATE MCR_MPP_ENCODER_DEBUG_IDR=1)
    endif()
    # RV1126B 量产 rootfs 将 Rockchip 媒体运行库部署在此目录；交叉 SDK 中的
    # media_out 仅用于构建，不能把主机 SDK 的绝对路径写进目标程序。
    set_target_properties(ipc_app PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "/oem/usr/lib"
    )
endif()

if(MCR_BUILD_DEMOS)

    # 第一阶段 ONVIF 验证：仅作为 WS-Discovery Target Service 回复 Probe。它不依赖
    # 摄像头、MPP 或 live555，先单独验证组播收包与单播 ProbeMatches 的协议链路。
    mcr_add_executable(onvif_ws_discovery_demo demo/onvif/OnvifWsDiscoveryDemo.c)
    target_include_directories(onvif_ws_discovery_demo PRIVATE
        "${MCR_GSOAP_ROOT}/include"
        "${MCR_GSOAP_ROOT}/plugin"
        "${MCR_ONVIF_WSDD_GENERATED_DIR}"
    )
    target_compile_definitions(onvif_ws_discovery_demo PRIVATE SOAP_H_FILE=wsddH.h)
    target_link_libraries(onvif_ws_discovery_demo PRIVATE mcr_gsoap_wsdd)

    mcr_add_executable(onvif_ws_discovery_probe_demo demo/onvif/OnvifWsDiscoveryProbeDemo.c)
    target_include_directories(onvif_ws_discovery_probe_demo PRIVATE
        "${MCR_GSOAP_ROOT}/include"
        "${MCR_GSOAP_ROOT}/plugin"
        "${MCR_ONVIF_WSDD_GENERATED_DIR}"
    )
    target_compile_definitions(onvif_ws_discovery_probe_demo PRIVATE SOAP_H_FILE=wsddH.h)
    target_link_libraries(onvif_ws_discovery_probe_demo PRIVATE mcr_gsoap_wsdd)

    mcr_add_executable(onvif_device_service_demo demo/onvif/OnvifDeviceServiceDemo.cpp)
    target_link_libraries(onvif_device_service_demo PRIVATE mcr_onvif_server)

    # 客户端第一阶段：只验证 WS-Discovery Probe/ProbeMatch，输出 Device Service XAddr。
    # 上层再以此 XAddr 调用 C 协议层请求 GetServices / Media Profiles。
    mcr_add_executable(onvif_discovery_client_demo demo/onvif/OnvifDiscoveryClientDemo.cpp)
    target_link_libraries(onvif_discovery_client_demo PRIVATE mcr_onvif_discovery)

    # C++ AudioPipeline 图验证：raw PCM 分发、raw->Opus 与 APM->Opus 并发；运行中取消
    # APM 支路，确认不会干扰 raw 编码支路。
    mcr_add_executable(audio_pipeline_demo demo/audio/AudioPipelineDemo.cpp)
    target_link_libraries(audio_pipeline_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_pipeline_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 反复挂载/释放 APM->Opus 支路，同时维持 raw->Opus。验证订阅 RAII 与 Node 生命周期。
    mcr_add_executable(audio_pipeline_stress_demo demo/audio/AudioPipelineStressDemo.cpp)
    target_link_libraries(audio_pipeline_stress_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_pipeline_stress_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 不依赖声卡：验证 PCM/压缩包池的满池丢弃与 payload buffer 复用语义。
    mcr_add_executable(audio_frame_pool_demo demo/audio/AudioFramePoolDemo.cpp)
    target_link_libraries(audio_frame_pool_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_frame_pool_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 不依赖 ALSA：验证 AAC/Opus 在 PCM 时间轴断裂时轻量 reset 后，不会跨缺口拼包。
    mcr_add_executable(audio_encoder_reset_demo demo/audio/AudioEncoderResetDemo.c)
    target_link_libraries(audio_encoder_reset_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_encoder_reset_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # C++ 唯一扬声器消费端：一条验证 PCM 直放，另一条验证 .mcropus -> 解码 -> ALSA。
    mcr_add_executable(audio_playback_pipeline_pcm_demo demo/audio/AudioPlaybackPipelinePcmDemo.cpp)
    target_link_libraries(audio_playback_pipeline_pcm_demo PRIVATE mcr_audio_core m)
    set_target_properties(audio_playback_pipeline_pcm_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )
    mcr_add_executable(audio_playback_pipeline_opus_demo demo/audio/AudioPlaybackPipelineOpusDemo.cpp)
    target_link_libraries(audio_playback_pipeline_opus_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_playback_pipeline_opus_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 用实际麦克风驱动 AudioPipeline 的 AAC-LC 支路，并异步写入 demo packet 文件。
    mcr_add_executable(audio_capture_aac_demo demo/audio/AudioCaptureAacDemo.cpp)
    target_link_libraries(audio_capture_aac_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_capture_aac_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 不经过 Opus/APM 的 raw PCM 文件闭环，用于隔离板级采集、播放与全双工问题。
    mcr_add_executable(audio_pcm_capture_demo demo/audio/AudioPcmCaptureDemo.cpp)
    target_link_libraries(audio_pcm_capture_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_pcm_capture_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )
    mcr_add_executable(audio_pcm_playback_demo demo/audio/AudioPcmPlaybackDemo.cpp)
    target_link_libraries(audio_pcm_playback_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_pcm_playback_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 采集与播放直通闭环：麦克风采集 PCM 并实时送入播放管线推至扬声器。
    mcr_add_executable(audio_capture_and_playback demo/audio/audio_capture_and_playback.cpp)
    target_link_libraries(audio_capture_and_playback PRIVATE mcr_audio_core)
    set_target_properties(audio_capture_and_playback PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )
    add_custom_target(audio_capture_and_playback_demo DEPENDS audio_capture_and_playback)

    # 采集 -> APM(AGC) -> 写 WAV，不经过 Opus。用于直接听/量 AGC 处理后的效果，
    # 例如判断板端麦克风电平偏低时 AGC 究竟能补回多少 dB。
    mcr_add_executable(audio_capture_apm_pcm_demo demo/audio/AudioCaptureApmPcmDemo.c)
    target_link_libraries(audio_capture_apm_pcm_demo PRIVATE mcr_audio_core m)
    set_target_properties(audio_capture_apm_pcm_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 读取已有 .mcropus packet 文件，完成 Opus -> PCM -> ALSA 播放闭环。
    mcr_add_executable(audio_opus_playback_demo demo/audio/AudioOpusPlaybackDemo.c)
    target_link_libraries(audio_opus_playback_demo PRIVATE mcr_audio_core)
    set_target_properties(audio_opus_playback_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 不依赖 ALSA 设备的 WebRTC AGC 自检：用固定低电平 PCM 验证处理后的实际增益。
    mcr_add_executable(audio_apm_gain_demo demo/audio/AudioApmGainDemo.c)
    target_link_libraries(audio_apm_gain_demo PRIVATE mcr_audio_core m)
    set_target_properties(audio_apm_gain_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 不依赖声卡的 AEC3 调用自检：验证 10ms reverse reference 与 capture 能被同一 APM
    # 实例正确处理；不用于评价真实声学消回声效果。
    mcr_add_executable(audio_apm_aec_smoke_demo demo/audio/AudioApmAecSmokeDemo.c)
    target_link_libraries(audio_apm_aec_smoke_demo PRIVATE mcr_audio_core m)
    set_target_properties(audio_apm_aec_smoke_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 板端诊断工具：不修改 mixer，只列出 codec 实际提供的采集/播放控件与当前值。
    mcr_add_executable(audio_mixer_inspect_demo demo/audio/AudioMixerInspectDemo.c)
    mcr_link_alsa(audio_mixer_inspect_demo)
    set_target_properties(audio_mixer_inspect_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    mcr_add_executable(mpp_decoder_demo
        demo/codec/mpp_decoder_demo.cpp
        src/core/memory/DmaAllocator.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(mpp_decoder_demo)

    mcr_add_executable(mpp_encoder_demo
        demo/codec/mpp_encoder_demo.cpp
        src/core/memory/DmaAllocator.cpp
        ${MCR_MPP_ENCODER_SOURCES}
    )
    mcr_link_media(mpp_encoder_demo)

    mcr_add_executable(v4l2_probe_demo
        demo/camera/v4l2_probe_demo.cpp
        src/core/cam/V4L2DeviceProbe.cpp
    )

    mcr_add_executable(dma_allocator_demo
        demo/memory/dma_allocator_demo.cpp
        src/core/memory/DmaAllocator.cpp
    )
    mcr_link_drm(dma_allocator_demo)

    mcr_add_executable(camera_capture_demo
        demo/camera/camera_capture_demo.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/cam/V4L2CameraSource.cpp
    )
    mcr_link_drm(camera_capture_demo)

    # 使用真实 V4L2 设备验证 CamManager 的轻恢复、完整重建、以及 lease 未归还时的
    # stop/start 与自动恢复行为；physical 参数模式可额外验证 USB 摄像头真实拔插。
    mcr_add_executable(cam_recovery_stress_demo
        demo/camera/CamRecoveryStressDemo.cpp
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
        demo/rtsp/RtspMppDecoderDemo.cpp
        ${MCR_RTSP_CLIENT_SOURCES}
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(rtsp_mpp_decoder_demo)
    mcr_link_live555(rtsp_mpp_decoder_demo)

    # 临时诊断工具：把 live555 已重组的完整 Annex-B NALU 原样落盘，便于区分
    # RTP/服务端问题和播放器兼容性问题。
    mcr_add_executable(rtsp_annexb_record_demo
        demo/rtsp/RtspAnnexBRecordDemo.cpp
        ${MCR_RTSP_CLIENT_SOURCES}
    )
    mcr_link_live555(rtsp_annexb_record_demo)

    mcr_add_executable(rtsp_stream_demo
        demo/rtsp/RtspStreamDemo.cpp
        ${MCR_RTSP_CLIENT_SOURCES}
        src/rtsp/client/RtspStream.cpp
        src/core/stream/Stream.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/hw/rga/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    mcr_link_media(rtsp_stream_demo)
    mcr_link_live555(rtsp_stream_demo)

    # 接收 IPC 发布的 AAC audio track，并经项目正式 AudioPlaybackPipeline 解码写 ALSA。
    mcr_add_executable(rtsp_audio_playback_demo
        demo/rtsp/RtspAudioPlaybackDemo.cpp
        ${MCR_RTSP_CLIENT_SOURCES}
        src/rtsp/client/RtspStream.cpp
        src/core/stream/Stream.cpp
        src/core/memory/DmaAllocator.cpp
        src/core/memory/DmaBufferPool.cpp
        src/core/hw/rga/RgaEngine.cpp
        ${MCR_MPP_DECODER_SOURCES}
    )
    target_link_libraries(rtsp_audio_playback_demo PRIVATE mcr_audio_core)
    mcr_link_media(rtsp_audio_playback_demo)
    mcr_link_live555(rtsp_audio_playback_demo)
    set_target_properties(rtsp_audio_playback_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    # 音频故障隔离工具：RTSP AAC -> AudioDecoder -> WAV，不经过 ALSA。可随后用 aplay
    # 回放同一文件，明确区分码流/解码问题与板端播放路由问题。
    mcr_add_executable(rtsp_aac_wav_record_demo
        demo/rtsp/RtspAacWavRecordDemo.cpp
        ${MCR_RTSP_CLIENT_SOURCES}
    )
    target_link_libraries(rtsp_aac_wav_record_demo PRIVATE mcr_audio_core)
    mcr_link_live555(rtsp_aac_wav_record_demo)
    set_target_properties(rtsp_aac_wav_record_demo PROPERTIES
        INSTALL_RPATH "/oem/usr/lib"
    )

    mcr_add_executable(stream_manager_demo
        demo/rtsp/StreamManagerDemo.cpp
        src/core/stream/StreamManager.cpp
        ${MCR_RTSP_CLIENT_SOURCES}
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
        demo/rtsp/StreamRecoveryDemo.cpp
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
        ${MCR_RTSP_CLIENT_SOURCES}
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
        demo/osd/FreeTypeBitmapDemo.cpp
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
