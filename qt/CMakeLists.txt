cmake_minimum_required(VERSION 3.16)

project(qt LANGUAGES C CXX)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)

qt_policy(SET QTP0001 NEW)

qt_standard_project_setup()


qt_add_executable(app
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/common/FrameHub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/cam/CamManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/cam/V4L2CameraSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/memory/DmaAllocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/memory/DmaBufferPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/hw/rga/RgaEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/common/VideoFrame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/hw/codec/MppTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/hw/codec/MppDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/hw/codec/MppEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/hw/codec/mpp_simple.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/hw/codec/mpp_advance.c
	${CMAKE_CURRENT_SOURCE_DIR}/../src/core/runtime/AppRuntime.cpp

)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/cam
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/stream
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/memory
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/hw/codec
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/core/hw/rga
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/rtsp/client
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/rtsp/server
    ${CMAKE_SYSROOT}/usr/include/rga
    ${CMAKE_SYSROOT}/usr/include/rockchip
    ${CMAKE_SYSROOT}/usr/include/libdrm
)

qt_add_qml_module(app
    URI QtDemo
    VERSION 1.0
    QML_FILES
        Main.qml
        SOURCES myitem.h myitem.cpp
                displaySink.hpp displaySink.cpp
                DisplayController.hpp DisplayController.cpp
)

target_link_libraries(app
    PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    rga
    rockchip_mpp
    drm
    -lEGL -lGLESv2 -lmali
)
