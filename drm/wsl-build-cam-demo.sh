#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-wsl-aarch64"

TOOLCHAIN_DIR="/opt/rk3568_kernel_pack/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin"
SYSROOT="/opt/rk3568_kernel_pack/sysroot"
CC="${TOOLCHAIN_DIR}/aarch64-none-linux-gnu-gcc"
CXX="${TOOLCHAIN_DIR}/aarch64-none-linux-gnu-g++"

mkdir -p "${BUILD_DIR}"

COMMON_CFLAGS=(
    --sysroot="${SYSROOT}"
    -O2
    -g0
    -Wall
    -Wextra
    -I"${SCRIPT_DIR}"
    -I"${SYSROOT}/usr/include/libdrm"
)

CXXFLAGS=(
    --sysroot="${SYSROOT}"
    -std=c++17
    -O2
    -g0
    -Wall
    -Wextra
    -I"${ROOT_DIR}/include"
    -I"${ROOT_DIR}/include/hw"
    -I"${ROOT_DIR}/include/hw/rkmpp_c"
    -I"${SCRIPT_DIR}"
    -I"${SYSROOT}/usr/include/rockchip"
    -I"${SYSROOT}/usr/include/libdrm"
    -I"${SYSROOT}/usr/include/rga"
)

echo "=== build drm_display.o ==="
"${CC}" "${COMMON_CFLAGS[@]}" -c "${SCRIPT_DIR}/drm_display.c" -o "${BUILD_DIR}/drm_display.o"

echo "=== build rkmpp c objects ==="
"${CC}" "${COMMON_CFLAGS[@]}" \
    -I"${ROOT_DIR}/include/hw/rkmpp_c" \
    -I"${SYSROOT}/usr/include/rockchip" \
    -c "${ROOT_DIR}/src/hw/rkmpp_c/mpp_simple.c" \
    -o "${BUILD_DIR}/mpp_simple.o"
"${CC}" "${COMMON_CFLAGS[@]}" \
    -I"${ROOT_DIR}/include/hw/rkmpp_c" \
    -I"${SYSROOT}/usr/include/rockchip" \
    -c "${ROOT_DIR}/src/hw/rkmpp_c/mpp_advance.c" \
    -o "${BUILD_DIR}/mpp_advance.o"

echo "=== build cam_drm_sink_demo ==="
"${CXX}" "${CXXFLAGS[@]}" \
    "${SCRIPT_DIR}/cam_drm_sink_demo.cpp" \
    "${ROOT_DIR}/src/AppRuntime.cpp" \
    "${ROOT_DIR}/src/CamManager.cpp" \
    "${ROOT_DIR}/src/DmaAllocator.cpp" \
    "${ROOT_DIR}/src/DmaBufferPool.cpp" \
    "${ROOT_DIR}/src/FrameHub.cpp" \
    "${ROOT_DIR}/src/V4L2CameraSource.cpp" \
    "${ROOT_DIR}/src/hw/MppDecoder.cpp" \
    "${ROOT_DIR}/src/hw/RgaEngine.cpp" \
    "${BUILD_DIR}/drm_display.o" \
    "${BUILD_DIR}/mpp_simple.o" \
    "${BUILD_DIR}/mpp_advance.o" \
    -L"${SYSROOT}/usr/lib" \
    -lrockchip_mpp \
    -lrga \
    -ldrm \
    -lpthread \
    -o "${BUILD_DIR}/cam_drm_sink_demo"

echo "=== built: ${BUILD_DIR}/cam_drm_sink_demo ==="
file "${BUILD_DIR}/cam_drm_sink_demo" || true
