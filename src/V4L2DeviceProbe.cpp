#include "V4L2DeviceProbe.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <utility>

namespace {

std::string g_lastError;

int xioctl(int fd, unsigned long request, void *arg)
{
    int ret = -1;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

std::string bytesToString(const unsigned char *data)
{
    return reinterpret_cast<const char *>(data);
}

std::string fourccToString(uint32_t fourcc)
{
    std::string text(4, ' ');
    text[0] = static_cast<char>(fourcc & 0xff);
    text[1] = static_cast<char>((fourcc >> 8) & 0xff);
    text[2] = static_cast<char>((fourcc >> 16) & 0xff);
    text[3] = static_cast<char>((fourcc >> 24) & 0xff);
    return text;
}

PixelFormat pixelFormatFromFourcc(uint32_t fourcc)
{
    switch (fourcc) {
    case V4L2_PIX_FMT_NV12:
        return PixelFormat::NV12;
    case V4L2_PIX_FMT_YUYV:
        return PixelFormat::YUYV;
    case V4L2_PIX_FMT_YUV420:
        return PixelFormat::YUV420P;
    case V4L2_PIX_FMT_RGB32:
    case V4L2_PIX_FMT_ABGR32:
        return PixelFormat::RGBA8888;
    default:
        return PixelFormat::Unknown;
    }
}

bool isVideoName(const char *name)
{
    constexpr const char *prefix = "video";
    if (std::strncmp(name, prefix, 5) != 0)
        return false;
    for (const char *p = name + 5; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return name[5] != '\0';
}

void appendDiscreteInterval(int fd,
                            uint32_t fourcc,
                            uint32_t width,
                            uint32_t height,
                            V4L2FrameSizeInfo &sizeInfo)
{
    for (uint32_t index = 0;; ++index) {
        v4l2_frmivalenum interval {};
        interval.index = index;
        interval.pixel_format = fourcc;
        interval.width = width;
        interval.height = height;

        if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0)
            break;

        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            sizeInfo.intervals.push_back({
                interval.discrete.numerator,
                interval.discrete.denominator,
            });
            continue;
        }

        // Stepwise/continuous intervals are represented by min/max for now.
        sizeInfo.intervals.push_back({
            interval.stepwise.min.numerator,
            interval.stepwise.min.denominator,
        });
        if (interval.stepwise.max.numerator != interval.stepwise.min.numerator ||
            interval.stepwise.max.denominator != interval.stepwise.min.denominator) {
            sizeInfo.intervals.push_back({
                interval.stepwise.max.numerator,
                interval.stepwise.max.denominator,
            });
        }
        break;
    }
}

void appendDiscreteSize(int fd, uint32_t fourcc, uint32_t width, uint32_t height, V4L2FormatInfo &format)
{
    V4L2FrameSizeInfo sizeInfo {};
    sizeInfo.width = width;
    sizeInfo.height = height;
    appendDiscreteInterval(fd, fourcc, width, height, sizeInfo);
    format.sizes.push_back(std::move(sizeInfo));
}

} // namespace

V4L2DeviceInfo V4L2DeviceProbe::queryDevice(const std::string &path)
{
    V4L2DeviceInfo info {};
    info.path = path;

    const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        setError("打开 V4L2 设备失败 " + path + ": " + std::strerror(errno));
        return info;
    }

    v4l2_capability cap {};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        setError("VIDIOC_QUERYCAP 失败 " + path + ": " + std::strerror(errno));
        close(fd);
        return info;
    }

    info.driver = bytesToString(cap.driver);
    info.card = bytesToString(cap.card);
    info.busInfo = bytesToString(cap.bus_info);
    info.capabilities = cap.capabilities;
    info.deviceCapabilities = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
        ? cap.device_caps
        : cap.capabilities;

    const uint32_t caps = info.deviceCapabilities;
    info.canCapture = (caps & V4L2_CAP_VIDEO_CAPTURE) || (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    info.canStream = caps & V4L2_CAP_STREAMING;
    info.isMetadata = caps & V4L2_CAP_META_CAPTURE;

    if (info.canCapture && !info.isMetadata) {
        const uint32_t bufferType = (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
            : V4L2_BUF_TYPE_VIDEO_CAPTURE;

        v4l2_format currentFormat {};
        currentFormat.type = bufferType;
        if (xioctl(fd, VIDIOC_G_FMT, &currentFormat) == 0) {
            if (bufferType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                const v4l2_pix_format_mplane &pix = currentFormat.fmt.pix_mp;
                info.currentFourcc = pix.pixelformat;
                info.currentWidth = pix.width;
                info.currentHeight = pix.height;
                if (pix.num_planes > 0) {
                    info.currentBytesPerLine = pix.plane_fmt[0].bytesperline;
                    info.currentSizeImage = pix.plane_fmt[0].sizeimage;
                }
            } else {
                const v4l2_pix_format &pix = currentFormat.fmt.pix;
                info.currentFourcc = pix.pixelformat;
                info.currentWidth = pix.width;
                info.currentHeight = pix.height;
                info.currentBytesPerLine = pix.bytesperline;
                info.currentSizeImage = pix.sizeimage;
            }

            info.currentFourccText = fourccToString(info.currentFourcc);
            info.hasCurrentFormat = info.currentWidth > 0 && info.currentHeight > 0;
        }

        for (uint32_t index = 0;; ++index) {
            v4l2_fmtdesc desc {};
            desc.index = index;
            desc.type = bufferType;

            if (xioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0)
                break;

            V4L2FormatInfo format {};
            format.fourcc = desc.pixelformat;
            format.fourccText = fourccToString(desc.pixelformat);
            format.description = bytesToString(desc.description);
            format.format = pixelFormatFromFourcc(desc.pixelformat);

            for (uint32_t sizeIndex = 0;; ++sizeIndex) {
                v4l2_frmsizeenum size {};
                size.index = sizeIndex;
                size.pixel_format = desc.pixelformat;

                if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0)
                    break;

                if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    appendDiscreteSize(fd,
                                       desc.pixelformat,
                                       size.discrete.width,
                                       size.discrete.height,
                                       format);
                    continue;
                }

                // Stepwise/continuous sizes are represented by min/max for UI hints.
                appendDiscreteSize(fd,
                                   desc.pixelformat,
                                   size.stepwise.min_width,
                                   size.stepwise.min_height,
                                   format);
                if (size.stepwise.max_width != size.stepwise.min_width ||
                    size.stepwise.max_height != size.stepwise.min_height) {
                    appendDiscreteSize(fd,
                                       desc.pixelformat,
                                       size.stepwise.max_width,
                                       size.stepwise.max_height,
                                       format);
                }
                break;
            }

            info.formats.push_back(std::move(format));
        }
    }

    close(fd);
    g_lastError.clear();
    return info;
}

std::vector<V4L2DeviceInfo> V4L2DeviceProbe::queryVideoDevices(const std::string &devDir)
{
    std::vector<V4L2DeviceInfo> devices;

    DIR *dir = opendir(devDir.c_str());
    if (!dir) {
        setError("打开设备目录失败 " + devDir + ": " + std::strerror(errno));
        return devices;
    }

    while (dirent *entry = readdir(dir)) {
        if (!isVideoName(entry->d_name))
            continue;

        std::string path = devDir;
        if (!path.empty() && path.back() != '/')
            path += '/';
        path += entry->d_name;

        V4L2DeviceInfo info = queryDevice(path);
        if (info.canCapture && info.canStream && !info.isMetadata && !info.formats.empty())
            devices.push_back(std::move(info));
    }

    closedir(dir);
    std::sort(devices.begin(), devices.end(), [](const V4L2DeviceInfo &lhs, const V4L2DeviceInfo &rhs) {
        return lhs.path < rhs.path;
    });
    return devices;
}

const std::string& V4L2DeviceProbe::lastError()
{
    return g_lastError;
}

void V4L2DeviceProbe::setError(const std::string &message)
{
    g_lastError = message;
}
