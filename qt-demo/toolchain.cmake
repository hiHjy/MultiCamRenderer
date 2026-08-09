set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 交叉编译时，try_compile 生成静态库而非可执行文件（避免链接/运行验证失败）
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)


set(CMAKE_C_COMPILER 
/home/alientek/rk3568_linux5.10_sdk/buildroot/output/rockchip_atk_dlrk3568/host/bin/aarch64-buildroot-linux-gnu-gcc)

set(CMAKE_CXX_COMPILER
/home/alientek/rk3568_linux5.10_sdk/buildroot/output/rockchip_atk_dlrk3568/host/bin/aarch64-buildroot-linux-gnu-g++)


set(CMAKE_SYSROOT
/home/alientek/rk3568_linux5.10_sdk/buildroot/output/rockchip_atk_dlrk3568/host/aarch64-buildroot-linux-gnu/sysroot
)


set(CMAKE_FIND_ROOT_PATH
/home/alientek/rk3568_linux5.10_sdk/buildroot/output/rockchip_atk_dlrk3568/host/aarch64-buildroot-linux-gnu/sysroot
/opt/Qt/6.10.3-rk3568-aarch64
)


set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)


set(QT_HOST_PATH /opt/Qt/6.10.3)