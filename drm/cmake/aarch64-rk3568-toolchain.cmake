set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(RK3568_SDK_OUTPUT "/home/alientek/rk3568_linux5.10_sdk/buildroot/output/rockchip_atk_dlrk3568" CACHE PATH "RK3568 Buildroot output directory")
set(RK3568_HOST "${RK3568_SDK_OUTPUT}/host")
set(RK3568_SYSROOT "${RK3568_HOST}/aarch64-buildroot-linux-gnu/sysroot")

set(CMAKE_SYSROOT "${RK3568_SYSROOT}")
set(CMAKE_C_COMPILER "${RK3568_HOST}/bin/aarch64-buildroot-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${RK3568_HOST}/bin/aarch64-buildroot-linux-gnu-g++")

set(CMAKE_FIND_ROOT_PATH "${RK3568_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${RK3568_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${RK3568_SYSROOT}/usr/lib/pkgconfig:${RK3568_SYSROOT}/usr/share/pkgconfig")
