#!/usr/bin/env bash
set -euo pipefail

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ENV="${SDK_ENV:-$HOME/RV1126_SDK/rv1126b-customer/buildroot/output/rockchip_rv1126bp_ipc_32_evb1_v10/rockchip_rv1126b_32_ipc/host/environment-setup}"

if [ ! -r "$SDK_ENV" ]; then
    echo "SDK environment not found: $SDK_ENV" >&2
    exit 1
fi

unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
unset SYSROOT SDKTARGETSYSROOT CMAKE_PREFIX_PATH LD_LIBRARY_PATH
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

# shellcheck disable=SC1090
source "$SDK_ENV" >/dev/null

mkdir -p "$BASE/build"

CXX_ABS="$(command -v "$CXX")"

echo "SDK_ENV=$SDK_ENV"
echo "CXX=$CXX"
echo "CXX_ABS=$CXX_ABS"
echo "OUT=$BASE/build/camera_capture_demo"
echo "OUT=$BASE/build/cam_manager_demo"
echo "OUT=$BASE/build/dma_allocator_demo"
echo "OUT=$BASE/build/test"
echo "OUT=$BASE/build/rga_test"
echo "OUT=$BASE/build/v4l2_probe_demo"

RGA_INC="$BASE/third_party/rga/include"
RGA_LIB_DIR="$BASE/third_party/rga/lib/arm32"
RGA_LINK_FLAGS="-L$RGA_LIB_DIR -Wl,-rpath,'\$ORIGIN/../third_party/rga/lib/arm32' -lrga"
PROJECT_INCLUDE_FLAGS="-I$BASE/include/core/common -I$BASE/include/core/cam -I$BASE/include/core/stream -I$BASE/include/core/memory -I$BASE/include/core/hw/codec -I$BASE/include/core/hw/rga -I$BASE/include/rtsp/client -I$BASE/include/rtsp/server"

cat > "$BASE/compile_commands.base.json" <<EOF
[
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/src/core/cam/V4L2CameraSource.cpp -o $BASE/build/V4L2CameraSource.o",
    "file": "$BASE/src/core/cam/V4L2CameraSource.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/src/core/cam/V4L2DeviceProbe.cpp -o $BASE/build/V4L2DeviceProbe.o",
    "file": "$BASE/src/core/cam/V4L2DeviceProbe.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/src/core/cam/CamManager.cpp -o $BASE/build/CamManager.o",
    "file": "$BASE/src/core/cam/CamManager.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/src/core/memory/DmaAllocator.cpp -o $BASE/build/DmaAllocator.o",
    "file": "$BASE/src/core/memory/DmaAllocator.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/src/core/common/FrameHub.cpp -o $BASE/build/FrameHub.o",
    "file": "$BASE/src/core/common/FrameHub.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/src/core/memory/DmaBufferPool.cpp -o $BASE/build/DmaBufferPool.o",
    "file": "$BASE/src/core/memory/DmaBufferPool.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -I$RGA_INC -c $BASE/src/core/hw/rga/RgaEngine.cpp -o $BASE/build/RgaEngine.o",
    "file": "$BASE/src/core/hw/rga/RgaEngine.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/demo/camera_capture_demo.cpp -o $BASE/build/camera_capture_demo.o",
    "file": "$BASE/demo/camera_capture_demo.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/demo/cam_manager_demo.cpp -o $BASE/build/cam_manager_demo.o",
    "file": "$BASE/demo/cam_manager_demo.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/demo/dma_allocator_demo.cpp -o $BASE/build/dma_allocator_demo.o",
    "file": "$BASE/demo/dma_allocator_demo.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/demo/test.cpp -o $BASE/build/test.o",
    "file": "$BASE/demo/test.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -I$RGA_INC -c $BASE/demo/rga_test.cpp -o $BASE/build/rga_test.o",
    "file": "$BASE/demo/rga_test.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -c $BASE/demo/v4l2_probe_demo.cpp -o $BASE/build/v4l2_probe_demo.o",
    "file": "$BASE/demo/v4l2_probe_demo.cpp"
  },
  {
    "directory": "$BASE",
    "command": "$CXX_ABS --sysroot=$STAGING_DIR -std=c++17 $CXXFLAGS -Wall -Wextra $PROJECT_INCLUDE_FLAGS -I$RGA_INC -c $BASE/src/sink/RgaCopySink.cpp -o $BASE/build/RgaCopySink.o",
    "file": "$BASE/src/sink/RgaCopySink.cpp"
  }
]
EOF

set -x
"$CXX" -std=c++17 -Wall -Wextra -O2 -g0 \
    $PROJECT_INCLUDE_FLAGS \
    "$BASE/src/core/memory/DmaAllocator.cpp" \
    "$BASE/src/core/cam/V4L2CameraSource.cpp" \
    "$BASE/demo/camera_capture_demo.cpp" \
    -o "$BASE/build/camera_capture_demo"
"$STRIP" "$BASE/build/camera_capture_demo" || true

"$CXX" -std=c++17 -Wall -Wextra -O2 -g0 \
    $PROJECT_INCLUDE_FLAGS \
    "$BASE/src/core/memory/DmaAllocator.cpp" \
    "$BASE/src/core/cam/V4L2CameraSource.cpp" \
    "$BASE/src/core/cam/CamManager.cpp" \
    "$BASE/src/core/common/FrameHub.cpp" \
    "$BASE/demo/cam_manager_demo.cpp" \
    -o "$BASE/build/cam_manager_demo"
"$STRIP" "$BASE/build/cam_manager_demo" || true

"$CXX" -std=c++17 -Wall -Wextra -O2 -g0 \
    $PROJECT_INCLUDE_FLAGS \
    "$BASE/src/core/memory/DmaAllocator.cpp" \
    "$BASE/demo/dma_allocator_demo.cpp" \
    -o "$BASE/build/dma_allocator_demo"
"$STRIP" "$BASE/build/dma_allocator_demo" || true

"$CXX" -std=c++17 -Wall -Wextra -O2 -g0 \
    $PROJECT_INCLUDE_FLAGS -I"$RGA_INC" \
    "$BASE/src/core/memory/DmaAllocator.cpp" \
    "$BASE/src/core/cam/V4L2CameraSource.cpp" \
    "$BASE/src/core/cam/CamManager.cpp" \
    "$BASE/src/core/common/FrameHub.cpp" \
    "$BASE/src/core/memory/DmaBufferPool.cpp" \
    "$BASE/src/core/hw/rga/RgaEngine.cpp" \
    "$BASE/demo/test.cpp" \
    -o "$BASE/build/test" \
    $RGA_LINK_FLAGS
"$STRIP" "$BASE/build/test" || true

"$CXX" -std=c++17 -Wall -Wextra -O2 -g0 \
    $PROJECT_INCLUDE_FLAGS -I"$RGA_INC" \
    "$BASE/src/core/memory/DmaAllocator.cpp" \
    "$BASE/src/core/cam/V4L2CameraSource.cpp" \
    "$BASE/src/core/cam/CamManager.cpp" \
    "$BASE/src/core/common/FrameHub.cpp" \
    "$BASE/src/core/memory/DmaBufferPool.cpp" \
    "$BASE/src/core/hw/rga/RgaEngine.cpp" \
    "$BASE/src/sink/RgaCopySink.cpp" \
    "$BASE/demo/rga_test.cpp" \
    -o "$BASE/build/rga_test" \
    $RGA_LINK_FLAGS
"$STRIP" "$BASE/build/rga_test" || true

"$CXX" -std=c++17 -Wall -Wextra -O2 -g0 \
    $PROJECT_INCLUDE_FLAGS \
    "$BASE/src/core/cam/V4L2DeviceProbe.cpp" \
    "$BASE/demo/v4l2_probe_demo.cpp" \
    -o "$BASE/build/v4l2_probe_demo"
"$STRIP" "$BASE/build/v4l2_probe_demo" || true
set +x

echo "Built: $BASE/build/camera_capture_demo"
file "$BASE/build/camera_capture_demo" || true
echo "Built: $BASE/build/cam_manager_demo"
file "$BASE/build/cam_manager_demo" || true
echo "Built: $BASE/build/dma_allocator_demo"
file "$BASE/build/dma_allocator_demo" || true
echo "Built: $BASE/build/test"
file "$BASE/build/test" || true
echo "Built: $BASE/build/rga_test"
file "$BASE/build/rga_test" || true
echo "Built: $BASE/build/v4l2_probe_demo"
file "$BASE/build/v4l2_probe_demo" || true

if [ -x "$BASE/tools/update_compile_commands.sh" ]; then
    "$BASE/tools/update_compile_commands.sh" || true
fi
