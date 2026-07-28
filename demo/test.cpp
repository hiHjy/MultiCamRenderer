#include "CamManager.hpp"

#include <iostream>

int main()
{
    CamManager manager;

    CamManager::CameraConfig config {};
    config.cameraId = 0;
    config.devicePath = "/dev/video28";
    config.width = 640;
    config.height = 480;
    config.fps = 30;
    config.format = PixelFormat::Auto;
    config.bufferCount = 4;

    if (!manager.addCamera(config)) {
        std::cerr << "addCamera failed: " << manager.lastError() << "\n";
        return 1;
    }

    if (!manager.startAll()) {
        std::cerr << "startAll failed: " << manager.lastError() << "\n";
        return 1;
    }

    for (int i = 0; i < 10; ++i) {
        if (!manager.pollOnce(2000)) {
            std::cerr << "pollOnce failed: " << manager.lastError() << "\n";
            manager.stopAll();
            return 1;
        }
    }

	//manager.run();
    manager.stopAll();
    return 0;
}
