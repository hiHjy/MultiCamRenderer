#include "CamManager.hpp"
#include "TestConsumer.hpp"

#include <iostream>

int main()
{
    CamManager manager;

    CamManager::CameraConfig config0 {};
    config0.cameraId = 0;
    config0.devicePath = "/dev/video32";
    config0.width = 640;
    config0.height = 480;
    config0.fps = 30;
    config0.format = PixelFormat::Auto;
    config0.bufferCount = 4;

    if (!manager.addCamera(config0)) {
        std::cerr << "addCamera camera0 failed: " << manager.lastError() << "\n";
        return 1;
    }

    if (!manager.addFrameConsumer(config0.cameraId, std::make_unique<TestConsumer>(0))) {
        std::cerr << "addFrameConsumer camera0 failed: " << manager.lastError() << "\n";
        return 1;
    }

	if (!manager.addFrameConsumer(config0.cameraId, std::make_unique<TestConsumer>(1))) {
        std::cerr << "addFrameConsumer camera0 failed: " << manager.lastError() << "\n";
        return 1;
    }

	if (!manager.addFrameConsumer(config0.cameraId, std::make_unique<TestConsumer>(2))) {
        std::cerr << "addFrameConsumer camera0 failed: " << manager.lastError() << "\n";
        return 1;
    }


    CamManager::CameraConfig config1 {};
    config1.cameraId = 1;
    config1.devicePath = "/dev/video24";
    config1.width = 640;
    config1.height = 480;
    config1.fps = 30;
    config1.format = PixelFormat::Auto;
    config1.bufferCount = 4;

    if (!manager.addCamera(config1)) {
        std::cerr << "addCamera camera1 failed: " << manager.lastError() << "\n";
        return 1;
    }

    if (!manager.addFrameConsumer(config1.cameraId, std::make_unique<TestConsumer>(3))) {
        std::cerr << "addFrameConsumer camera1 failed: " << manager.lastError() << "\n";
        return 1;
    }


    if (!manager.addFrameConsumer(config1.cameraId, std::make_unique<TestConsumer>(4))) {
        std::cerr << "addFrameConsumer camera1 failed: " << manager.lastError() << "\n";
        return 1;
    }

	if (!manager.addFrameConsumer(config1.cameraId, std::make_unique<TestConsumer>(5))) {
        std::cerr << "addFrameConsumer camera1 failed: " << manager.lastError() << "\n";
        return 1;
    }

	if (!manager.addFrameConsumer(config1.cameraId, std::make_unique<TestConsumer>(6))) {
        std::cerr << "addFrameConsumer camera1 failed: " << manager.lastError() << "\n";
        return 1;
    }


    if (!manager.startAll()) {
        std::cerr << "startAll failed: " << manager.lastError() << "\n";
        return 1;
    }

    // for (int i = 0; i < 10; ++i) {
    //     if (!manager.pollOnce(2000)) {
    //         std::cerr << "pollOnce failed: " << manager.lastError() << "\n";
    //         manager.stopAll();
    //         return 1;
    //     }
    // }

	manager.run();
    manager.stopAll();
    return 0;
}
