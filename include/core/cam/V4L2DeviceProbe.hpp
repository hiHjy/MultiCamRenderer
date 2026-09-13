#pragma once

#include "VideoTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct V4L2FrameIntervalInfo {
    uint32_t numerator = 0;
    uint32_t denominator = 0;
};

struct V4L2FrameSizeInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<V4L2FrameIntervalInfo> intervals;
};

struct V4L2FormatInfo {
    PixelFormat format = PixelFormat::Unknown;
    uint32_t fourcc = 0;
    std::string fourccText;
    std::string description;
    std::vector<V4L2FrameSizeInfo> sizes;
};

struct V4L2DeviceInfo {
    std::string path;
    std::string driver;
    std::string card;
    std::string busInfo;
    uint32_t capabilities = 0;
    uint32_t deviceCapabilities = 0;
    bool canCapture = false;
    bool canStream = false;
    bool isMetadata = false;
    bool hasCurrentFormat = false;
    uint32_t currentFourcc = 0;
    std::string currentFourccText;
    uint32_t currentWidth = 0;
    uint32_t currentHeight = 0;
    uint32_t currentBytesPerLine = 0;
    uint32_t currentSizeImage = 0;
    std::vector<V4L2FormatInfo> formats;
};

class V4L2DeviceProbe {
public:
    static V4L2DeviceInfo queryDevice(const std::string &path);
    static std::vector<V4L2DeviceInfo> queryVideoDevices(const std::string &devDir = "/dev");

    static const std::string& lastError();

private:
    static void setError(const std::string &message);
};
