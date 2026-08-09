#!/bin/bash
# ==============================================================================
# 板端一键运行脚本 (RK3568 aarch64)
#
# 用法：
#   ./run.sh                # 默认 eglfs 平台
#   ./run.sh --platform linuxfb   # 指定其他平台
#   ./run.sh --help         # 查看帮助
# ==============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_DIR="${SCRIPT_DIR}/deploy"

# ---- 默认配置 ----------------------------------------------------------------
PLATFORM="eglfs"
APP_ARGS=()

# ---- 参数解析 ----------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --platform)
            PLATFORM="$2"
            shift 2
            ;;
        --platform=*)
            PLATFORM="${1#*=}"
            shift
            ;;
        --help|-h)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --platform <name>   指定 QPA 平台 (默认: eglfs)"
            echo "                      可选: eglfs, linuxfb, minimal, offscreen"
            echo "  --help, -h          显示帮助"
            echo ""
            echo "环境变量 (可按需覆盖):"
            echo "  QT_QPA_PLATFORM          显示平台"
            echo "  QT_QPA_EGLFS_KMS_ATOMIC  1=原子模式, 0=传统模式"
            echo "  QT_QPA_EGLFS_FORCE888    1=强制 RGB888"
            exit 0
            ;;
        *)
            APP_ARGS+=("$1")
            shift
            ;;
    esac
done

# ---- 检查部署目录 ------------------------------------------------------------
if [ ! -f "${DEPLOY_DIR}/bin/appqt-demo" ]; then
    echo "错误: 未找到可执行文件: ${DEPLOY_DIR}/bin/appqt-demo"
    echo "请先在 PC 上运行 ./build.sh 进行交叉编译和部署"
    exit 1
fi

# ---- 运行时环境变量 ----------------------------------------------------------
# 修复 UTF-8 locale 警告
export LANG=en_US.utf8

# Qt6 安装在板端的路径
QT6_DIR="/opt/6.10.3-rk3568-aarch64"

# Qt 插件路径
export QT_PLUGIN_PATH="${QT6_DIR}/plugins"

# QML 导入路径（系统 Qt 的 QML 模块）
export QML_IMPORT_PATH="${QT6_DIR}/qml"
export QML2_IMPORT_PATH="${QT6_DIR}/qml"

# 显示平台
export QT_QPA_PLATFORM="${PLATFORM}"

# ---- EGLFS 特定环境变量 ------------------------------------------------------
if [ "${PLATFORM}" = "eglfs" ]; then
    # KMS/DRM 后端（无 wayland 时使用）
    export QT_QPA_EGLFS_INTEGRATION="${QT_QPA_EGLFS_INTEGRATION:-eglfs_kms}"

    # RK3568 需要关闭 atomic 模式，否则会卡住
    export QT_QPA_EGLFS_KMS_ATOMIC="${QT_QPA_EGLFS_KMS_ATOMIC:-0}"

    # 如果没有设置物理屏幕尺寸，默认 1920x1080
    export QT_QPA_EGLFS_PHYSICAL_WIDTH="${QT_QPA_EGLFS_PHYSICAL_WIDTH:-520}"
    export QT_QPA_EGLFS_PHYSICAL_HEIGHT="${QT_QPA_EGLFS_PHYSICAL_HEIGHT:-290}"
fi

# ---- 运行应用 ----------------------------------------------------------------
echo "================================================"
echo "  qt-demo - RK3568 Qt6 Quick Demo"
echo "================================================"
echo "  平台:       ${PLATFORM} (QT_QPA_PLATFORM)"
echo "  EGLFS 后端: ${QT_QPA_EGLFS_INTEGRATION:-未设置}"
echo "  应用路径:   ${DEPLOY_DIR}/bin/appqt-demo"
echo "================================================"
echo ""

cd "${DEPLOY_DIR}/bin"
exec ./appqt-demo "${APP_ARGS[@]}"
