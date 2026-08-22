#include "CamManager.hpp"
#include "../include/sink/RgaCopySink.hpp"

#include <iostream>
#include <memory>

int main () 
{
	CamManager manager;
	auto sink = std::make_shared<RgaCopySink>();

    CamManager::CameraConfig config0 {};
    config0.cameraId = 0;
    config0.devicePath = "/dev/video32";
    config0.width = 1280;
    config0.height = 720;
    config0.fps = 30;
    config0.format = PixelFormat::Auto;
    config0.bufferCount = 4;

    if (!manager.addCamera(config0)) {
        std::cerr << "addCamera camera0 failed: " << manager.lastError() << "\n";
        return 1;
    }
	manager.addSinkForHub(0, sink);
	manager.startAllCameras();
	manager.run();



	return 0;
}
