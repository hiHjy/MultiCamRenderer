#!/usr/bin/env bash
set -euo pipefail

# RK3568 WSL：构建所有非 Qt demo（MPP、V4L2、RTSP、DRM）。
# Qt 应用与依赖 QtGui 的 RGA demo 请使用根目录 wsl-build-qt.sh。

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BASE}/build/wsl-aarch64"
TOOLCHAIN_FILE="${BASE}/cmake/wsl-aarch64-toolchain.cmake"
MODE="${1:-build}"

if [[ "${MODE}" == "clean" ]]; then
    rm -rf "${BUILD_DIR}"
fi

cmake -S "${BASE}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMCR_BUILD_DEMOS=ON \
    -DMCR_BUILD_QT=OFF

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

if [[ -x "${BASE}/tools/update_compile_commands.sh" ]]; then
    "${BASE}/tools/update_compile_commands.sh" || true
fi

echo "=== built non-Qt demos: ${BUILD_DIR} ==="
