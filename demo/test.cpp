#include "CamManager.hpp"
#include "TestConsumer.hpp"

#include <iostream>
#include <memory>
#include <vector>

int main()
{
    CamManager manager;
    std::vector<std::shared_ptr<Consumer>> consumers;

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

    consumers.push_back(std::make_shared<TestConsumer>(0));
    if (!manager.addFrameConsumer(config0.cameraId, consumers.back())) {
        std::cerr << "addFrameConsumer camera0 failed: " << manager.lastError() << "\n";
        return 1;
    }

    consumers.push_back(std::make_shared<TestConsumer>(1));
	if (!manager.addFrameConsumer(config0.cameraId, consumers.back())) {
        std::cerr << "addFrameConsumer camera0 failed: " << manager.lastError() << "\n";
        return 1;
    }

    consumers.push_back(std::make_shared<TestConsumer>(2));
	if (!manager.addFrameConsumer(config0.cameraId, consumers.back())) {
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

    consumers.push_back(std::make_shared<TestConsumer>(3));
    if (!manager.addFrameConsumer(config1.cameraId, consumers.back())) {
        std::cerr << "addFrameConsumer camera1 failed: " << manager.lastError() << "\n";
        return 1;
    }


    consumers.push_back(std::make_shared<TestConsumer>(4));
    if (!manager.addFrameConsumer(config1.cameraId, consumers.back())) {
        std::cerr << "addFrameConsumer camera1 failed: " << manager.lastError() << "\n";
        return 1;
    }

    consumers.push_back(std::make_shared<TestConsumer>(5));
	if (!manager.addFrameConsumer(config1.cameraId, consumers.back())) {
        std::cerr << "addFrameConsumer camera1 failed: " << manager.lastError() << "\n";
        return 1;
    }

    consumers.push_back(std::make_shared<TestConsumer>(6));
	if (!manager.addFrameConsumer(config1.cameraId, consumers.back())) {
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
