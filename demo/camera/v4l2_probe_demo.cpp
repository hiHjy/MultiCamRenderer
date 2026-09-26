#include "V4L2DeviceProbe.hpp"

#include <iomanip>
#include <iostream>

namespace {

const char *pixelFormatName(PixelFormat format)
{
    switch (format) {
    case PixelFormat::Unknown:
        return "Unknown";
    case PixelFormat::Auto:
        return "Auto";
    case PixelFormat::NV12:
        return "NV12";
    case PixelFormat::YUYV:
        return "YUYV";
    case PixelFormat::YUV420P:
        return "YUV420P";
    case PixelFormat::RGBA8888:
        return "RGBA8888";
    case PixelFormat::MJPEG:
        return "MJPEG";
    }
    return "Unknown";
}

double fpsOf(const V4L2FrameIntervalInfo &interval)
{
    if (interval.numerator == 0)
        return 0.0;
    return static_cast<double>(interval.denominator) / static_cast<double>(interval.numerator);
}

void printDevice(const V4L2DeviceInfo &device)
{
    std::cout << device.path << '\n'
              << "  driver: " << device.driver << '\n'
              << "  card:   " << device.card << '\n'
              << "  bus:    " << device.busInfo << '\n';

    if (device.hasCurrentFormat) {
        std::cout << "  current: " << device.currentWidth << "x" << device.currentHeight
                  << ' ' << device.currentFourccText
                  << " bytesPerLine=" << device.currentBytesPerLine
                  << " sizeImage=" << device.currentSizeImage << '\n';
    } else {
        std::cout << "  current: unconfigured\n";
    }

    for (const V4L2FormatInfo &format : device.formats) {
        std::cout << "  format: " << format.fourccText
                  << " (" << pixelFormatName(format.format) << ") "
                  << format.description << '\n';

        for (const V4L2FrameSizeInfo &size : format.sizes) {
            std::cout << "    " << size.width << "x" << size.height;
            if (!size.intervals.empty()) {
                std::cout << "  fps:";
                for (const V4L2FrameIntervalInfo &interval : size.intervals) {
                    std::cout << ' ' << std::fixed << std::setprecision(2) << fpsOf(interval);
                }
            }
            std::cout << '\n';
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            V4L2DeviceInfo device = V4L2DeviceProbe::queryDevice(argv[i]);
            if (device.canCapture && device.canStream && !device.isMetadata && !device.formats.empty()) {
                printDevice(device);
            } else {
                std::cout << argv[i] << " is not a usable video capture node\n";
            }
        }
        return 0;
    }

    const std::vector<V4L2DeviceInfo> devices = V4L2DeviceProbe::queryVideoDevices();
    for (const V4L2DeviceInfo &device : devices)
        printDevice(device);

    if (devices.empty()) {
        std::cerr << "No usable V4L2 capture node found";
        if (!V4L2DeviceProbe::lastError().empty())
            std::cerr << ": " << V4L2DeviceProbe::lastError();
        std::cerr << '\n';
        return 1;
    }

    return 0;
}
