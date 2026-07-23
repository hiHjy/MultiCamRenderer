#include "CamManager.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void printUsage(const char* argv0)
{
    std::cout
        << "用法: " << argv0 << " [width] [height] [fps] [frames] [device...]\n"
        << "示例: " << argv0 << " 640 480 30 60 /dev/video32 /dev/video24\n"
        << "不传参数时默认采集 /dev/video32 和 /dev/video24。\n";
}

int parseInt(const char* text, int defaultValue)
{
    if (text == nullptr) {
        return defaultValue;
    }
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || value <= 0) {
        return defaultValue;
    }
    return static_cast<int>(value);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    CamManager manager;
    const int width = argc > 1 ? parseInt(argv[1], 640) : 640;
    const int height = argc > 2 ? parseInt(argv[2], 480) : 480;
    const int fps = argc > 3 ? parseInt(argv[3], 30) : 30;
    const int frameLimit = argc > 4 ? parseInt(argv[4], 60) : 60;

    std::vector<std::string> devices;
    if (argc > 5) {
        for (int i = 5; i < argc; ++i) {
            devices.emplace_back(argv[i]);
        }
    } else {
        devices = {"/dev/video32", "/dev/video24"};
    }

    for (size_t i = 0; i < devices.size(); ++i) {
        CamManager::CameraConfig config {};
        config.cameraId = static_cast<int>(i);
        config.devicePath = devices[i];
        config.videoConfig.width = width;
        config.videoConfig.height = height;
        config.videoConfig.fps = fps;
        config.videoConfig.format = V4L2CameraSource::PixelFormat::Auto;
        config.bufferCount = 4;

        if (!manager.addCamera(config)) {
            std::cerr << "camera " << i << " addCamera 失败: "
                      << manager.lastError() << "\n";
            return 1;
        }

        std::cout << "camera " << i << " 添加完成: device=" << devices[i] << "\n";
    }

    if (!manager.startAll()) {
        std::cerr << "startAll 失败: " << manager.lastError() << "\n";
        return 1;
    }

    for (int i = 0; i < frameLimit; ++i) {
        if (!manager.pollOnce(2000)) {
            std::cerr << "pollOnce 失败: " << manager.lastError() << "\n";
            manager.stopAll();
            return 1;
        }
    }

    manager.stopAll();
    std::cout << "CamManager demo 完成\n";
    return 0;
}
