set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# RK3568（ATK-DLRK3568）使用 Rockchip 5.10 SDK 自带的 buildroot 工具链。
# 用 stretch/staging 而不是 target：target/ 是精简后的根文件系统，不含开发头文件。
set(RK3568_SDK_ROOT "/home/hjy/rk3568_5.10_sdk" CACHE PATH "RK3568 Linux SDK 根目录")
set(RK3568_BUILDROOT_OUTPUT
    "${RK3568_SDK_ROOT}/buildroot/output/rockchip_atk_dlrk3568"
    CACHE PATH "RK3568 buildroot output 目录")
set(RK3568_TOOLCHAIN_ROOT
    "${RK3568_BUILDROOT_OUTPUT}/host"
    CACHE PATH "RK3568 buildroot 交叉工具链根目录")
set(RK3568_SYSROOT
    "${RK3568_BUILDROOT_OUTPUT}/staging"
    CACHE PATH "RK3568 buildroot staging sysroot（含头文件）")

if(NOT EXISTS "${RK3568_TOOLCHAIN_ROOT}/bin/aarch64-buildroot-linux-gnu-g++" OR
   NOT EXISTS "${RK3568_SYSROOT}/usr/include")
    message(FATAL_ERROR "RK3568 buildroot 工具链或 staging sysroot 不存在，请检查 RK3568_SDK_ROOT")
endif()

set(CMAKE_C_COMPILER "${RK3568_TOOLCHAIN_ROOT}/bin/aarch64-buildroot-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${RK3568_TOOLCHAIN_ROOT}/bin/aarch64-buildroot-linux-gnu-g++")
set(CMAKE_SYSROOT "${RK3568_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH "${RK3568_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${RK3568_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${RK3568_SYSROOT}/usr/lib/pkgconfig:${RK3568_SYSROOT}/usr/share/pkgconfig")
