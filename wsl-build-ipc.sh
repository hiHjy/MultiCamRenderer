#!/usr/bin/env bash
set -euo pipefail

# RV1126B IPC：只构建 src/IpcApp.cpp 及其 RTSP 发布链，不影响 RK3568/Qt 构建目录。

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ROOT="/home/hjy/2026-07-18/rv1126b_linux_ipc_xiaoyu"
MEDIA_ROOT="${SDK_ROOT}/output/out/media_out"
# gSOAP 的 ONVIF HTTP Digest 插件需要 OpenSSL；它属于完整 RV1126B rootfs，
# 不在 media_out 中。该目录同时提供交叉编译头文件与板端一致的 ABI 库。
OPENSSL_ROOT="${SDK_ROOT}/sysdrv/out/rootfs_glibc_rv1126b"
BUILD_DIR="${BASE}/build/rv1126b-aarch64"
TOOLCHAIN_FILE="${BASE}/cmake/rv1126b-aarch64-toolchain.cmake"
MODE="${1:-build}"

if [[ "${MODE}" == "clean" ]]; then
    rm -rf "${BUILD_DIR}"
fi

cmake -S "${BASE}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMCR_LIVE555_TARGET=rv1126b \
    -DMCR_BUILD_DEMOS=OFF \
    -DMCR_BUILD_QT=OFF \
    -DMCR_BUILD_IPC_APP=ON \
    -DMCR_MEDIA_ROOT="${MEDIA_ROOT}" \
    -DMCR_ONVIF_OPENSSL_ROOT="${OPENSSL_ROOT}"

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

if [[ -x "${BASE}/tools/update_compile_commands.sh" ]]; then
    "${BASE}/tools/update_compile_commands.sh" || true
fi

echo "=== built RV1126B IPC app: ${BUILD_DIR}/ipc_app ==="
