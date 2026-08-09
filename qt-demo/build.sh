#!/bin/bash
# ==============================================================================
# Cross-compile build script for qt-demo (RK3568 aarch64)
#
# Usage:
#   ./build.sh              # 增量编译 + 部署
#   ./build.sh clean        # 清理后重新编译 + 部署
#   ./build.sh build        # 仅编译，不部署
# ==============================================================================
set -e

# ---- 路径配置 ----------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
DEPLOY_DIR="${SCRIPT_DIR}/deploy"
TOOLCHAIN_FILE="${SCRIPT_DIR}/toolchain.cmake"

# ---- 参数处理 ----------------------------------------------------------------
MODE="${1:-all}"

if [ "$MODE" = "clean" ]; then
    echo "=== 清理 build 和 deploy 目录 ==="
    rm -rf "${BUILD_DIR}" "${DEPLOY_DIR}"
fi

# ---- 创建 build 目录 ---------------------------------------------------------
mkdir -p "${BUILD_DIR}"

# ---- CMake 配置（首次或 toolchain/cmake 变更时自动重新配置）--------------------
echo "=== CMake 配置 ==="
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_COLOR_MAKEFILE=ON

# ---- 编译 --------------------------------------------------------------------
echo "=== 开始编译 ==="
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo "=== 编译完成：${BUILD_DIR}/appqt-demo ==="

# ---- 部署 --------------------------------------------------------------------
if [ "$MODE" = "build" ]; then
    echo "=== 仅编译模式，跳过部署 ==="
    exit 0
fi

echo "=== 部署到 ${DEPLOY_DIR} ==="

# 清理旧部署
rm -rf "${DEPLOY_DIR}"
mkdir -p "${DEPLOY_DIR}/bin"

# --- 只拷贝可执行文件（QML 已作为 Qt 资源编译进二进制）---
cp "${BUILD_DIR}/appqt-demo" "${DEPLOY_DIR}/bin/"

echo "=== 部署完成 ==="
echo ""
echo "部署内容（单二进制，QML 嵌入在可执行文件中）："
find "${DEPLOY_DIR}" -type f | sed "s|${DEPLOY_DIR}/|  |g"
echo ""
echo "板端运行："
echo "  cd qt-demo && ./run.sh"
