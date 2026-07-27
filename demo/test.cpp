#include "CamManager.hpp"

int main()
{
    CamManager manager;

    CamManager::CameraConfig config {};
    config.cameraId = 0;
    config.devicePath = "/dev/video32";
    config.width = 640;
    config.height = 480;
    config.fps = 30;
    config.format = CamManager::PixelFormat::Auto;
    config.bufferCount = 4;

    if (!manager.addCamera(config)) {
        return 1;
    }

    return 0;
}
