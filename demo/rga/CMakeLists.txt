cmake_minimum_required(VERSION 3.16)

project(rga_ops_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui)

add_executable(rga_ops_demo
    main.cpp
    ../../src/DmaAllocator.cpp
    ../../src/hw/RgaEngine.cpp
)

target_include_directories(rga_ops_demo PRIVATE
    ../../include
    ../../include/hw
    /home/alientek/rk3568_linux5.10_sdk/buildroot/output/rockchip_atk_dlrk3568/host/aarch64-buildroot-linux-gnu/sysroot/usr/include/rga
)

target_link_libraries(rga_ops_demo PRIVATE
    Qt6::Gui
    rga
)
