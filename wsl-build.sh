#!/usr/bin/env bash
set -euo pipefail

# RK3568 aarch64 demo 的统一构建入口。
# qt-demo 是独立应用，仍使用 qt-demo/wsl-build.sh。

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RK3568_PACK="${RK3568_PACK:-/opt/rk3568_kernel_pack}"
TOOLCHAIN="${RK3568_PACK}/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu"
SYSROOT="${RK3568_PACK}/sysroot"
BUILD_DIR="${BASE}/build-wsl-aarch64"
LIVE555_DIR="${BASE}/third_party/live555"

CC="${TOOLCHAIN}-gcc"
CXX="${TOOLCHAIN}-g++"

if [[ ! -x "${CC}" || ! -x "${CXX}" ]]; then
    echo "RK3568 toolchain not found under: ${RK3568_PACK}" >&2
    exit 1
fi

if [[ ! -d "${SYSROOT}" ]]; then
    echo "RK3568 sysroot not found: ${SYSROOT}" >&2
    exit 1
fi

for required in \
    "${LIVE555_DIR}/lib/aarch64/libliveMedia.a" \
    "${SYSROOT}/usr/lib/librockchip_mpp.so" \
    "${SYSROOT}/usr/lib/librga.so"; do
    if [[ ! -e "${required}" ]]; then
        echo "required file not found: ${required}" >&2
        exit 1
    fi
done

mkdir -p "${BUILD_DIR}"
# 这两个 demo 已删除；清理旧二进制，避免误以为仍由统一脚本产出。
rm -f "${BUILD_DIR}/cam_manager_demo" "${BUILD_DIR}/camera_test_demo"

COMMON_CFLAGS=(
    --sysroot="${SYSROOT}"
    -std=gnu11 -O2 -g0 -Wall -Wextra -Wno-unused-function
    -I"${BASE}/include"
    -I"${BASE}/drm"
    -I"${SYSROOT}/usr/include/rockchip"
    -I"${SYSROOT}/usr/include/libdrm"
    -I"${SYSROOT}/usr/include/rga"
)

COMMON_CXXFLAGS=(
    --sysroot="${SYSROOT}"
    -std=c++17 -O2 -g0 -Wall -Wextra
    -I"${BASE}/include"
    -I"${BASE}/src"
    -I"${BASE}/drm"
    -I"${LIVE555_DIR}/include/liveMedia"
    -I"${LIVE555_DIR}/include/groupsock"
    -I"${LIVE555_DIR}/include/BasicUsageEnvironment"
    -I"${LIVE555_DIR}/include/UsageEnvironment"
    -I"${SYSROOT}/usr/include/rockchip"
    -I"${SYSROOT}/usr/include/libdrm"
    -I"${SYSROOT}/usr/include/rga"
)

MPP_LIBS=(
    -L"${SYSROOT}/usr/lib"
    -lrockchip_mpp -lrga -ldrm -lpthread
)

LIVE555_LIBS=(
    -L"${LIVE555_DIR}/lib/aarch64"
    -Wl,--start-group
    -lliveMedia -lgroupsock -lBasicUsageEnvironment -lUsageEnvironment
    -Wl,--end-group
)

echo "=== build common C objects ==="
"${CC}" "${COMMON_CFLAGS[@]}" -c "${BASE}/src/mpp_simple.c" -o "${BUILD_DIR}/mpp_simple.o"
"${CC}" "${COMMON_CFLAGS[@]}" -c "${BASE}/src/mpp_advance.c" -o "${BUILD_DIR}/mpp_advance.o"
"${CC}" "${COMMON_CFLAGS[@]}" -c "${BASE}/drm/drm_display.c" -o "${BUILD_DIR}/drm_display.o"

echo "=== build mpp_decoder_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/demo/mpp_decoder_demo.cpp" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/MppDecoder.cpp" \
    "${BUILD_DIR}/mpp_simple.o" "${BUILD_DIR}/mpp_advance.o" \
    "${MPP_LIBS[@]}" \
    -o "${BUILD_DIR}/mpp_decoder_demo"

echo "=== build mpp_encoder_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/demo/mpp_encoder_demo.cpp" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/MppEncoder.cpp" \
    "${BASE}/src/MppDecoder.cpp" \
    "${BUILD_DIR}/mpp_simple.o" "${BUILD_DIR}/mpp_advance.o" \
    "${MPP_LIBS[@]}" \
    -o "${BUILD_DIR}/mpp_encoder_demo"

echo "=== build v4l2_probe_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/src/V4L2DeviceProbe.cpp" \
    "${BASE}/demo/v4l2_probe_demo.cpp" \
    -o "${BUILD_DIR}/v4l2_probe_demo"

echo "=== build dma_allocator_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/demo/dma_allocator_demo.cpp" \
    -o "${BUILD_DIR}/dma_allocator_demo"

echo "=== build camera_capture_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/V4L2CameraSource.cpp" \
    "${BASE}/demo/camera_capture_demo.cpp" \
    -o "${BUILD_DIR}/camera_capture_demo"

echo "=== build frame_lease_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/demo/frame_lease/main.cpp" \
    -o "${BUILD_DIR}/frame_lease_demo"

echo "=== build rtsp_mpp_decoder_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/demo/RtspMppDecoderDemo.cpp" \
    "${BASE}/src/Live555RtspClient.cpp" \
    "${BASE}/src/AnnexBSink.cpp" \
    "${BASE}/src/MppDecoder.cpp" \
    "${BUILD_DIR}/mpp_simple.o" "${BUILD_DIR}/mpp_advance.o" \
    "${LIVE555_LIBS[@]}" "${MPP_LIBS[@]}" \
    -o "${BUILD_DIR}/rtsp_mpp_decoder_demo"

echo "=== build rtsp_stream_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/demo/RtspStreamDemo.cpp" \
    "${BASE}/src/Live555RtspClient.cpp" \
    "${BASE}/src/AnnexBSink.cpp" \
    "${BASE}/src/RtspStream.cpp" \
    "${BASE}/src/Stream.cpp" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/DmaBufferPool.cpp" \
    "${BASE}/src/MppDecoder.cpp" \
    "${BASE}/src/RgaEngine.cpp" \
    "${BUILD_DIR}/mpp_simple.o" "${BUILD_DIR}/mpp_advance.o" \
    "${LIVE555_LIBS[@]}" "${MPP_LIBS[@]}" \
    -o "${BUILD_DIR}/rtsp_stream_demo"

echo "=== build drm_test ==="
"${CC}" "${COMMON_CFLAGS[@]}" \
    "${BASE}/drm/main.c" "${BASE}/drm/drm_display.c" \
    -L"${SYSROOT}/usr/lib" -ldrm \
    -o "${BUILD_DIR}/drm_test"

echo "=== build cam_drm_sink_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/drm/cam_drm_sink_demo.cpp" \
    "${BASE}/src/AppRuntime.cpp" \
    "${BASE}/src/CamManager.cpp" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/DmaBufferPool.cpp" \
    "${BASE}/src/FrameHub.cpp" \
    "${BASE}/src/V4L2CameraSource.cpp" \
    "${BASE}/src/MppDecoder.cpp" \
    "${BASE}/src/RgaEngine.cpp" \
    "${BUILD_DIR}/drm_display.o" \
    "${BUILD_DIR}/mpp_simple.o" "${BUILD_DIR}/mpp_advance.o" \
    "${MPP_LIBS[@]}" \
    -o "${BUILD_DIR}/cam_drm_sink_demo"

echo "=== build rtsp_drm_sink_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/drm/rtsp_drm_sink_demo.cpp" \
    "${BASE}/src/Live555RtspClient.cpp" \
    "${BASE}/src/AnnexBSink.cpp" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/DmaBufferPool.cpp" \
    "${BASE}/src/MppDecoder.cpp" \
    "${BASE}/src/RgaEngine.cpp" \
    "${BUILD_DIR}/drm_display.o" \
    "${BUILD_DIR}/mpp_simple.o" "${BUILD_DIR}/mpp_advance.o" \
    "${LIVE555_LIBS[@]}" "${MPP_LIBS[@]}" \
    -o "${BUILD_DIR}/rtsp_drm_sink_demo"

echo "=== build rga_ops_demo ==="
"${BASE}/demo/rga/build.sh"

echo "=== Qt app is separate ==="
echo "qt-demo: ./qt-demo/wsl-build.sh"

echo "=== built aarch64 demos in: ${BUILD_DIR} ==="
find "${BUILD_DIR}" -maxdepth 1 -type f -executable -printf '  %f\n' | sort
