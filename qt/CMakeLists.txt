cmake_minimum_required(VERSION 3.16)

project(qt LANGUAGES C CXX)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)

qt_policy(SET QTP0001 NEW)

qt_standard_project_setup()


qt_add_executable(app
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FrameHub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/CamManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/V4L2CameraSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/DmaAllocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/DmaBufferPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/RgaEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/VideoFrame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/MppTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/MppDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/MppEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mpp_simple.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mpp_advance.c
	${CMAKE_CURRENT_SOURCE_DIR}/../src/AppRuntime.cpp

)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
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
