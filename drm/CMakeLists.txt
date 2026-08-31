cmake_minimum_required(VERSION 3.16)

project(drm_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(${PROJECT_NAME}
    main.c
    drm_display.c
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        _GNU_SOURCE
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        PkgConfig::LIBDRM
)
