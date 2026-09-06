cmake_minimum_required(VERSION 3.16)

project(rga_ops_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui)

add_executable(rga_ops_demo
    main.cpp
    ../../src/DmaAllocator.cpp
    ../../src/RgaEngine.cpp
)

target_include_directories(rga_ops_demo PRIVATE
    ../../include
    ${CMAKE_SYSROOT}/usr/include/rga
)

target_link_libraries(rga_ops_demo PRIVATE
    Qt6::Gui
    rga
)
