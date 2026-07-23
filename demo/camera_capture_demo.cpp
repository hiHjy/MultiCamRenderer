#include "V4L2CameraSource.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

const char* pixelFormatName(V4L2CameraSource::PixelFormat format)
{
    switch (format) {
    case V4L2CameraSource::PixelFormat::Auto:
        return "Auto";
    case V4L2CameraSource::PixelFormat::NV12:
        return "NV12";
    case V4L2CameraSource::PixelFormat::YUYV:
        return "YUYV";
    case V4L2CameraSource::PixelFormat::YUV420P:
        return "YUV420P";
    }
    return "Unknown";
}

std::string fourccToString(uint32_t fourcc)
{
    std::string s;
    s.push_back(static_cast<char>(fourcc & 0xff));
    s.push_back(static_cast<char>((fourcc >> 8) & 0xff));
    s.push_back(static_cast<char>((fourcc >> 16) & 0xff));
    s.push_back(static_cast<char>((fourcc >> 24) & 0xff));
    return s;
}

void printUsage(const char* argv0)
{
    std::cout
        << "用法: " << argv0 << " [device] [width] [height] [fps] [frames] [dump_path]\n"
        << "示例: " << argv0 << " /dev/video32 640 480 30 60 /tmp/frame.raw\n";
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

void printDeviceCaps(int fd)
{
    v4l2_capability cap {};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        std::cerr << "VIDIOC_QUERYCAP 打印失败: " << std::strerror(errno) << "\n";
        return;
    }

    const uint32_t capabilities =
        (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;

    std::cout << "设备信息:\n";
    std::cout << "  driver: " << reinterpret_cast<const char*>(cap.driver) << "\n";
    std::cout << "  card: " << reinterpret_cast<const char*>(cap.card) << "\n";
    std::cout << "  bus: " << reinterpret_cast<const char*>(cap.bus_info) << "\n";
    std::cout << "  capabilities: 0x" << std::hex << capabilities << std::dec << "\n";
    std::cout << "  capture type: "
              << ((capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? "multiplanar" : "single-plane")
              << "\n";
}

void printFrameSize(const v4l2_frmsizeenum& size)
{
    if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        std::cout << "      Size: " << size.discrete.width << "x"
                  << size.discrete.height << "\n";
        return;
    }

    if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
        size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
        std::cout << "      Size: Stepwise "
                  << size.stepwise.min_width << "x" << size.stepwise.min_height
                  << " - "
                  << size.stepwise.max_width << "x" << size.stepwise.max_height
                  << " step "
                  << size.stepwise.step_width << "/" << size.stepwise.step_height
                  << "\n";
    }
}

void printSupportedFormats(int fd, uint32_t type)
{
    std::cout << "支持格式:\n";
    for (uint32_t i = 0;; ++i) {
        v4l2_fmtdesc desc {};
        desc.index = i;
        desc.type = static_cast<v4l2_buf_type>(type);
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) {
            if (errno == EINVAL && i == 0) {
                std::cout << "  暂无可枚举格式\n";
            }
            break;
        }

        std::cout << "  [" << i << "] " << fourccToString(desc.pixelformat)
                  << " (" << reinterpret_cast<const char*>(desc.description) << ")\n";

        for (uint32_t j = 0;; ++j) {
            v4l2_frmsizeenum size {};
            size.index = j;
            size.pixel_format = desc.pixelformat;
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
                break;
            }
            printFrameSize(size);
        }
    }
}

bool dumpFrame(const std::string& path, const V4L2CameraSource::Frame& frame)
{
    int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        std::cerr << "打开 dump 文件失败: " << path << " " << std::strerror(errno) << "\n";
        return false;
    }

    const size_t bytes = frame.bytesUsed > 0 ? frame.bytesUsed : frame.capacity;
    const char* data = static_cast<const char*>(frame.va);
    size_t writtenTotal = 0;
    while (writtenTotal < bytes) {
        ssize_t written = write(out, data + writtenTotal, bytes - writtenTotal);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "写 dump 文件失败: " << std::strerror(errno) << "\n";
            close(out);
            return false;
        }
        writtenTotal += static_cast<size_t>(written);
    }

    close(out);
    std::cout << "已保存第一帧: " << path << " bytes=" << bytes << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    const std::string device = argc > 1 ? argv[1] : "/dev/video32";
    const int width = argc > 2 ? parseInt(argv[2], 640) : 640;
    const int height = argc > 3 ? parseInt(argv[3], 480) : 480;
    const int fps = argc > 4 ? parseInt(argv[4], 30) : 30;
    const int frameLimit = argc > 5 ? parseInt(argv[5], 60) : 60;
    const std::string dumpPath = argc > 6 ? argv[6] : "";

    V4L2CameraSource camera(0);
    if (!camera.openDevice(device)) {
        std::cerr << "openDevice 失败: " << camera.lastError() << "\n";
        return 1;
    }

    printDeviceCaps(camera.fd());
    printSupportedFormats(
        camera.fd(),
        camera.isMultiPlanar()
            ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
            : V4L2_BUF_TYPE_VIDEO_CAPTURE);

    V4L2CameraSource::CamConfig cfg {};
    cfg.width = width;
    cfg.height = height;
    cfg.fps = fps;
    cfg.format = V4L2CameraSource::PixelFormat::Auto;

    if (!camera.configure(cfg)) {
        std::cerr << "configure 失败: " << camera.lastError() << "\n";
        return 1;
    }

    const auto& mode = camera.currentMode();
    std::cout << "协商结果:\n";
    std::cout << "  api: " << (camera.isMultiPlanar() ? "multiplanar" : "single-plane") << "\n";
    std::cout << "  size: " << mode.width << "x" << mode.height << "\n";
    std::cout << "  fps request: " << fps << "\n";
    std::cout << "  fps actual: " << mode.fps << "\n";
    std::cout << "  stride: " << mode.stride << "\n";
    std::cout << "  sizeimage: " << mode.sizeimg << "\n";
    std::cout << "  format: " << pixelFormatName(mode.format)
              << " (" << fourccToString(mode.v4l2Format) << ")\n";

    if (!camera.setupDmaImportBuffers(4)) {
        std::cerr << "setupDmaImportBuffers 失败: " << camera.lastError() << "\n";
        return 1;
    }

    if (!camera.start()) {
        std::cerr << "start 失败: " << camera.lastError() << "\n";
        return 1;
    }

    std::cout << "开始采集，目标帧数: " << frameLimit << "\n";
    int captured = 0;
    bool dumped = false;
    while (captured < frameLimit) {
        pollfd pfd {};
        pfd.fd = camera.fd();
        pfd.events = POLLIN | POLLERR | POLLHUP;

        const int pollRet = poll(&pfd, 1, 2000);
        if (pollRet < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "poll 失败: " << std::strerror(errno) << "\n";
            return 1;
        }
        if (pollRet == 0) {
            std::cerr << "poll 超时，未收到新帧\n";
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP)) != 0) {
            std::cerr << "摄像头 fd 异常 revents=0x" << std::hex << pfd.revents << std::dec << "\n";
            return 1;
        }

        V4L2CameraSource::Frame frame {};
        if (!camera.dequeueFrame(frame)) {
            if (!camera.lastError().empty()) {
                std::cerr << "dequeueFrame 失败: " << camera.lastError() << "\n";
            }
            continue;
        }

        std::cout << "frame " << captured
                  << " seq=" << frame.sequence
                  << " ts_us=" << frame.timestampUs
                  << " bytes=" << frame.bytesUsed
                  << " fd=" << frame.dmaFd
                  << " index=" << frame.index
                  << "\n";

        if (!dumpPath.empty() && !dumped) {
            dumpFrame(dumpPath, frame);
            dumped = true;
        }

        if (!camera.requeueFrame(frame)) {
            std::cerr << "requeueFrame 失败: " << camera.lastError() << "\n";
            return 1;
        }

        captured++;
    }

    camera.stop();
    std::cout << "采集完成\n";
    return 0;
}
