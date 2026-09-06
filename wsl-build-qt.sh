#!/usr/bin/env bash
set -euo pipefail

# RK3568 WSL：构建 Qt 应用和 Qt/RGA demo。

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BASE}/build/wsl-aarch64-qt"
DEPLOY_DIR="${BASE}/qt/deploy-wsl-aarch64"
TOOLCHAIN_FILE="${BASE}/cmake/wsl-aarch64-toolchain.cmake"
MODE="${1:-all}"

if [[ "${MODE}" == "clean" ]]; then
    rm -rf "${BUILD_DIR}" "${DEPLOY_DIR}"
fi

source /opt/rk3568_kernel_pack/qt6-aarch64-env.sh

cmake -S "${BASE}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMCR_BUILD_DEMOS=OFF \
    -DMCR_BUILD_QT=ON

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

if [[ -x "${BASE}/tools/update_compile_commands.sh" ]]; then
    "${BASE}/tools/update_compile_commands.sh" || true
fi

if [[ "${MODE}" == "build" ]]; then
    echo "=== built Qt targets: ${BUILD_DIR} ==="
    exit 0
fi

rm -rf "${DEPLOY_DIR}"
mkdir -p "${DEPLOY_DIR}/bin"
cp "${BUILD_DIR}/qt/app" "${DEPLOY_DIR}/bin/"

echo "=== deployed Qt app: ${DEPLOY_DIR} ==="
