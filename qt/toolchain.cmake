set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 交叉编译时，try_compile 生成静态库而非可执行文件（避免链接/运行验证失败）
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)


set(RK3568_TOOLCHAIN_ROOT "/home/hjy/rk3568_kernel_pack/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu")
set(RK3568_SYSROOT "/home/hjy/rk3568_kernel_pack/sysroot")
set(QT_HOST_PATH "/opt/Qt/6.10.3")
set(QT_TARGET_PATH "/opt/6.10.3-rk3568-aarch64")

set(CMAKE_C_COMPILER "${RK3568_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${RK3568_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-g++")

set(CMAKE_SYSROOT "${RK3568_SYSROOT}")


set(CMAKE_FIND_ROOT_PATH
    "${RK3568_SYSROOT}"
    "${QT_TARGET_PATH}"
)


set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_PREFIX_PATH "${QT_TARGET_PATH}")
