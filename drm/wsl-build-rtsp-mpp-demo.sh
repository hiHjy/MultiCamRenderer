#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-wsl-aarch64"
# 默认编译基于 Stream/RtspStream 的完整拉流、解码、稳定输出 demo。
# 需要旧的直接 MPP demo 时，可显式传入 DEMO_SOURCE/DEMO_OUTPUT 覆盖。
DEMO_SOURCE="${DEMO_SOURCE:-${SCRIPT_DIR}/RtspStreamDemo.cpp}"
DEMO_OUTPUT="${DEMO_OUTPUT:-${BUILD_DIR}/rtsp_stream_demo}"
TOOLCHAIN_DIR="/opt/rk3568_kernel_pack/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin"
SYSROOT="/opt/rk3568_kernel_pack/sysroot"
LIVE555_DIR="${ROOT_DIR}/third_party/live555"
LIVE555_CLIENT_DIR="${SCRIPT_DIR}/live555"
CC="${TOOLCHAIN_DIR}/aarch64-none-linux-gnu-gcc"
CXX="${TOOLCHAIN_DIR}/aarch64-none-linux-gnu-g++"

for required in \
    "${DEMO_SOURCE}" \
    "${LIVE555_CLIENT_DIR}/Live555RtspClient.cpp" \
    "${LIVE555_CLIENT_DIR}/AnnexBSink.cpp" \
    "${LIVE555_DIR}/lib/aarch64/libliveMedia.a" \
    "${SYSROOT}/usr/lib/librockchip_mpp.so"; do
    if [[ ! -e "${required}" ]]; then
        echo "required file not found: ${required}" >&2
        exit 1
    fi
done

mkdir -p "${BUILD_DIR}"

COMMON_CFLAGS=(
    --sysroot="${SYSROOT}"
    -O2 -g0 -Wall -Wextra
    -I"${ROOT_DIR}/include"
    -I"${ROOT_DIR}/include/hw/rkmpp_c"
    -I"${SYSROOT}/usr/include/rockchip"
    -I"${SYSROOT}/usr/include/libdrm"
    -I"${SYSROOT}/usr/include/rga"
)

CXXFLAGS=(
    --sysroot="${SYSROOT}"
    -std=c++17 -O2 -g0 -Wall -Wextra
    -I"${ROOT_DIR}/include"
    -I"${ROOT_DIR}/include/hw"
    -I"${ROOT_DIR}/include/hw/rkmpp_c"
    -I"${LIVE555_CLIENT_DIR}"
    -I"${LIVE555_DIR}/include/liveMedia"
    -I"${LIVE555_DIR}/include/groupsock"
    -I"${LIVE555_DIR}/include/BasicUsageEnvironment"
    -I"${LIVE555_DIR}/include/UsageEnvironment"
    -I"${SYSROOT}/usr/include/rockchip"
    -I"${SYSROOT}/usr/include/libdrm"
    -I"${SYSROOT}/usr/include/rga"
)

"${CC}" "${COMMON_CFLAGS[@]}" -c "${ROOT_DIR}/src/hw/rkmpp_c/mpp_simple.c" \
    -o "${BUILD_DIR}/mpp_simple.o"
"${CC}" "${COMMON_CFLAGS[@]}" -c "${ROOT_DIR}/src/hw/rkmpp_c/mpp_advance.c" \
    -o "${BUILD_DIR}/mpp_advance.o"

"${CXX}" "${CXXFLAGS[@]}" \
    "${DEMO_SOURCE}" \
    "${LIVE555_CLIENT_DIR}/Live555RtspClient.cpp" \
    "${LIVE555_CLIENT_DIR}/AnnexBSink.cpp" \
    "${ROOT_DIR}/src/Stream.cpp" \
    "${ROOT_DIR}/src/RtspStream.cpp" \
    "${ROOT_DIR}/src/DmaAllocator.cpp" \
    "${ROOT_DIR}/src/DmaBufferPool.cpp" \
    "${ROOT_DIR}/src/hw/MppDecoder.cpp" \
    "${ROOT_DIR}/src/hw/RgaEngine.cpp" \
    "${BUILD_DIR}/mpp_simple.o" \
    "${BUILD_DIR}/mpp_advance.o" \
    -L"${LIVE555_DIR}/lib/aarch64" \
    -Wl,--start-group \
    -lliveMedia -lgroupsock -lBasicUsageEnvironment -lUsageEnvironment \
    -Wl,--end-group \
    -L"${SYSROOT}/usr/lib" \
    -lrockchip_mpp -lrga -ldrm -lpthread \
    -o "${DEMO_OUTPUT}"

echo "built: ${DEMO_OUTPUT}"
