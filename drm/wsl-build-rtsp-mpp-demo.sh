#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-wsl-aarch64"
TOOLCHAIN_DIR="/opt/rk3568_kernel_pack/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin"
SYSROOT="/opt/rk3568_kernel_pack/sysroot"
LIVE555_DIR="${ROOT_DIR}/third_party/live555"
LIVE555_CLIENT_DIR="${SCRIPT_DIR}/live555"
CC="${TOOLCHAIN_DIR}/aarch64-none-linux-gnu-gcc"
CXX="${TOOLCHAIN_DIR}/aarch64-none-linux-gnu-g++"

for required in \
    "${SCRIPT_DIR}/RtspMppDecoderDemo.cpp" \
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
)

"${CC}" "${COMMON_CFLAGS[@]}" -c "${ROOT_DIR}/src/hw/rkmpp_c/mpp_simple.c" \
    -o "${BUILD_DIR}/mpp_simple.o"
"${CC}" "${COMMON_CFLAGS[@]}" -c "${ROOT_DIR}/src/hw/rkmpp_c/mpp_advance.c" \
    -o "${BUILD_DIR}/mpp_advance.o"

"${CXX}" "${CXXFLAGS[@]}" \
    "${SCRIPT_DIR}/RtspMppDecoderDemo.cpp" \
    "${LIVE555_CLIENT_DIR}/Live555RtspClient.cpp" \
    "${LIVE555_CLIENT_DIR}/AnnexBSink.cpp" \
    "${ROOT_DIR}/src/hw/MppDecoder.cpp" \
    "${BUILD_DIR}/mpp_simple.o" \
    "${BUILD_DIR}/mpp_advance.o" \
    -L"${LIVE555_DIR}/lib/aarch64" \
    -Wl,--start-group \
    -lliveMedia -lgroupsock -lBasicUsageEnvironment -lUsageEnvironment \
    -Wl,--end-group \
    -L"${SYSROOT}/usr/lib" \
    -lrockchip_mpp -ldrm -lpthread \
    -o "${BUILD_DIR}/rtsp_mpp_decoder_demo"

echo "built: ${BUILD_DIR}/rtsp_mpp_decoder_demo"
