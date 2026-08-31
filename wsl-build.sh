#!/usr/bin/env bash
set -euo pipefail

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RK3568_PACK="${RK3568_PACK:-/opt/rk3568_kernel_pack}"
TOOLCHAIN="${RK3568_PACK}/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu"
SYSROOT="${RK3568_PACK}/sysroot"
BUILD_DIR="${BASE}/build"

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

mkdir -p "${BUILD_DIR}"

COMMON_CFLAGS=(
    --sysroot="${SYSROOT}"
    -std=gnu11
    -Wall
    -Wextra
    -Wno-unused-function
    -O2
    -g0
    -I"${BASE}/include"
    -I"${BASE}/include/hw/rkmpp_c"
    -I"${SYSROOT}/usr/include/rockchip"
    -I"${SYSROOT}/usr/include/libdrm"
)

COMMON_CXXFLAGS=(
    --sysroot="${SYSROOT}"
    -std=c++17
    -Wall
    -Wextra
    -O2
    -g0
    -I"${BASE}/include"
    -I"${BASE}/include/hw"
    -I"${BASE}/include/hw/rkmpp_c"
    -I"${SYSROOT}/usr/include/rockchip"
    -I"${SYSROOT}/usr/include/libdrm"
)

MPP_LIBS=(
    -L"${SYSROOT}/usr/lib"
    -lrockchip_mpp
    -ldrm
    -lpthread
)

echo "=== build rkmpp c objects ==="
"${CC}" "${COMMON_CFLAGS[@]}" -c "${BASE}/src/hw/rkmpp_c/mpp_simple.c" -o "${BUILD_DIR}/mpp_simple.o"
"${CC}" "${COMMON_CFLAGS[@]}" -c "${BASE}/src/hw/rkmpp_c/mpp_advance.c" -o "${BUILD_DIR}/mpp_advance.o"

echo "=== build mpp_decoder_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/demo/mpp_decoder_demo.cpp" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/hw/MppDecoder.cpp" \
    "${BUILD_DIR}/mpp_simple.o" \
    "${BUILD_DIR}/mpp_advance.o" \
    "${MPP_LIBS[@]}" \
    -o "${BUILD_DIR}/mpp_decoder_demo"

echo "=== build mpp_encoder_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/demo/mpp_encoder_demo.cpp" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/hw/MppEncoder.cpp" \
    "${BASE}/src/hw/MppDecoder.cpp" \
    "${BUILD_DIR}/mpp_simple.o" \
    "${BUILD_DIR}/mpp_advance.o" \
    "${MPP_LIBS[@]}" \
    -o "${BUILD_DIR}/mpp_encoder_demo"

echo "=== build v4l2_probe_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/src/V4L2DeviceProbe.cpp" \
    "${BASE}/demo/v4l2_probe_demo.cpp" \
    -o "${BUILD_DIR}/v4l2_probe_demo"

echo "=== build camera_capture_demo ==="
"${CXX}" "${COMMON_CXXFLAGS[@]}" \
    "${BASE}/src/DmaAllocator.cpp" \
    "${BASE}/src/V4L2CameraSource.cpp" \
    "${BASE}/demo/camera_capture_demo.cpp" \
    -o "${BUILD_DIR}/camera_capture_demo"

echo "=== done ==="
file "${BUILD_DIR}/mpp_decoder_demo" \
     "${BUILD_DIR}/mpp_encoder_demo" \
     "${BUILD_DIR}/v4l2_probe_demo" \
     "${BUILD_DIR}/camera_capture_demo" || true
