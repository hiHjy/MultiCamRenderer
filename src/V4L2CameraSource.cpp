#include "V4L2CameraSource.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

namespace {

int ioctlRetry(int fd, unsigned long request, void* arg)
{
    int ret = -1;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

std::string errnoText(const std::string& prefix)
{
    std::ostringstream oss;
    oss << prefix << ": " << std::strerror(errno);
    return oss.str();
}

uint32_t toV4L2Format(V4L2CameraSource::PixelFormat format)
{
    switch (format) {
    case V4L2CameraSource::PixelFormat::NV12:
        return V4L2_PIX_FMT_NV12;
    case V4L2CameraSource::PixelFormat::YUYV:
        return V4L2_PIX_FMT_YUYV;
    case V4L2CameraSource::PixelFormat::YUV420P:
        return V4L2_PIX_FMT_YUV420;
    case V4L2CameraSource::PixelFormat::Auto:
        return 0;
    }
    return 0;
}

V4L2CameraSource::PixelFormat fromV4L2Format(uint32_t format)
{
    switch (format) {
    case V4L2_PIX_FMT_NV12:
        return V4L2CameraSource::PixelFormat::NV12;
    case V4L2_PIX_FMT_YUYV:
        return V4L2CameraSource::PixelFormat::YUYV;
    case V4L2_PIX_FMT_YUV420:
        return V4L2CameraSource::PixelFormat::YUV420P;
    default:
        return V4L2CameraSource::PixelFormat::Auto;
    }
}

std::vector<V4L2CameraSource::PixelFormat> formatCandidates(
    V4L2CameraSource::PixelFormat format)
{
    if (format != V4L2CameraSource::PixelFormat::Auto) {
        return {format};
    }

    return {
        V4L2CameraSource::PixelFormat::NV12,
        V4L2CameraSource::PixelFormat::YUYV,
        V4L2CameraSource::PixelFormat::YUV420P,
    };
}

} // namespace

V4L2CameraSource::V4L2CameraSource(int cameraId)
    : m_cameraId(cameraId)
{
}

V4L2CameraSource::~V4L2CameraSource()
{
    closeDevice();
}

bool V4L2CameraSource::openDevice(const std::string& devicePath)
{
    if (m_state != State::Closed) {
        setError("设备已经打开");
        return false;
    }

    m_v4l2Fd = open(devicePath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_v4l2Fd < 0) {
        setError(errnoText("打开摄像头设备失败"));
        return false;
    }

    v4l2_capability cap {};
    if (ioctlRetry(m_v4l2Fd, VIDIOC_QUERYCAP, &cap) < 0) {
        setError(errnoText("VIDIOC_QUERYCAP 失败"));
        closeDevice();
        return false;
    }

    const uint32_t capabilities =
        (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;

    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0) {
        m_bufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        m_multiPlanar = true;
    } else if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0) {
        m_bufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        m_multiPlanar = false;
    } else {
        setError("设备不支持 V4L2 视频采集");
        closeDevice();
        return false;
    }

    if ((capabilities & V4L2_CAP_STREAMING) == 0) {
        setError("设备不支持 streaming I/O");
        closeDevice();
        return false;
    }

    m_state = State::Opened;
    m_lastError.clear();
    return true;
}

void V4L2CameraSource::closeDevice()
{
    stop();

    releaseBuffers();

    if (m_v4l2Fd >= 0) {
        close(m_v4l2Fd);
        m_v4l2Fd = -1;
    }

    m_state = State::Closed;
}

bool V4L2CameraSource::configure(const CamConfig& cfg)
{
    if (m_state != State::Opened && m_state != State::Configured) {
        setError("configure 只能在 Opened/Configured 状态调用");
        return false;
    }

    if (cfg.width <= 0 || cfg.height <= 0) {
        setError("摄像头宽高必须大于 0");
        return false;
    }

    std::string lastFormatError;
    for (PixelFormat candidate : formatCandidates(cfg.format)) {
        const uint32_t v4l2Format = toV4L2Format(candidate);
        if (v4l2Format == 0) {
            continue;
        }

        v4l2_format fmt {};
        fmt.type = static_cast<v4l2_buf_type>(m_bufferType);
        if (m_multiPlanar) {
            fmt.fmt.pix_mp.width = static_cast<uint32_t>(cfg.width);
            fmt.fmt.pix_mp.height = static_cast<uint32_t>(cfg.height);
            fmt.fmt.pix_mp.pixelformat = v4l2Format;
            fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
            fmt.fmt.pix_mp.num_planes = 1;
        } else {
            fmt.fmt.pix.width = static_cast<uint32_t>(cfg.width);
            fmt.fmt.pix.height = static_cast<uint32_t>(cfg.height);
            fmt.fmt.pix.pixelformat = v4l2Format;
            fmt.fmt.pix.field = V4L2_FIELD_NONE;
        }

        if (ioctlRetry(m_v4l2Fd, VIDIOC_S_FMT, &fmt) < 0) {
            lastFormatError = errnoText("VIDIOC_S_FMT 失败");
            continue;
        }

        v4l2_format currentFmt {};
        currentFmt.type = static_cast<v4l2_buf_type>(m_bufferType);
        if (ioctlRetry(m_v4l2Fd, VIDIOC_G_FMT, &currentFmt) < 0) {
            lastFormatError = errnoText("VIDIOC_G_FMT 失败");
            continue;
        }

        const uint32_t currentV4L2Format = m_multiPlanar
            ? currentFmt.fmt.pix_mp.pixelformat
            : currentFmt.fmt.pix.pixelformat;
        if (currentV4L2Format != v4l2Format) {
            lastFormatError = "驱动没有接受请求的像素格式";
            continue;
        }

        v4l2_streamparm parm {};
        parm.type = static_cast<v4l2_buf_type>(m_bufferType);
        if (cfg.fps > 0) {
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator =
                static_cast<uint32_t>(cfg.fps);
            (void)ioctlRetry(m_v4l2Fd, VIDIOC_S_PARM, &parm);
        }

        int actualFps = cfg.fps;
        v4l2_streamparm currentParm {};
        currentParm.type = static_cast<v4l2_buf_type>(m_bufferType);
        if (ioctlRetry(m_v4l2Fd, VIDIOC_G_PARM, &currentParm) == 0 &&
            currentParm.parm.capture.timeperframe.numerator > 0) {
            actualFps = static_cast<int>(
                currentParm.parm.capture.timeperframe.denominator /
                currentParm.parm.capture.timeperframe.numerator);
        }

        if (m_multiPlanar) {
            m_currentMode.width = static_cast<int>(currentFmt.fmt.pix_mp.width);
            m_currentMode.height = static_cast<int>(currentFmt.fmt.pix_mp.height);
            m_currentMode.stride =
                static_cast<int>(currentFmt.fmt.pix_mp.plane_fmt[0].bytesperline);
            m_currentMode.sizeimg = currentFmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        } else {
            m_currentMode.width = static_cast<int>(currentFmt.fmt.pix.width);
            m_currentMode.height = static_cast<int>(currentFmt.fmt.pix.height);
            m_currentMode.stride = static_cast<int>(currentFmt.fmt.pix.bytesperline);
            m_currentMode.sizeimg = currentFmt.fmt.pix.sizeimage;
        }
        m_currentMode.format = fromV4L2Format(currentV4L2Format);
        m_currentMode.v4l2Format = currentV4L2Format;
        m_currentMode.fps = actualFps;

        m_currentConfig = cfg;
        m_currentConfig.width = m_currentMode.width;
        m_currentConfig.height = m_currentMode.height;
        m_currentConfig.fps = m_currentMode.fps;
        m_currentConfig.format = m_currentMode.format;

        m_state = State::Configured;
        m_lastError.clear();
        return true;
    }

    setError(lastFormatError.empty() ? "没有可用的摄像头像素格式" : lastFormatError);
    return false;
}

bool V4L2CameraSource::getCurrentConfig(CamConfig& cfg)
{
    if (m_state == State::Closed || m_state == State::Opened) {
        setError("摄像头尚未配置");
        return false;
    }

    cfg = m_currentConfig;
    return true;
}

bool V4L2CameraSource::setupDmaImportBuffers(
    int bufferCount,
    const std::string& heapPath)
{
    if (m_state != State::Configured) {
        setError("setupDmaImportBuffers 只能在 Configured 状态调用");
        return false;
    }

    if (bufferCount <= 0) {
        setError("bufferCount 必须大于 0");
        return false;
    }

    if (m_currentMode.sizeimg == 0) {
        setError("当前格式的 sizeimage 为 0，无法分配 DMA buffer");
        return false;
    }

    v4l2_requestbuffers req {};
    req.count = static_cast<uint32_t>(bufferCount);
    req.type = static_cast<v4l2_buf_type>(m_bufferType);
    req.memory = V4L2_MEMORY_DMABUF;

    if (ioctlRetry(m_v4l2Fd, VIDIOC_REQBUFS, &req) < 0) {
        setError(errnoText("VIDIOC_REQBUFS(DMABUF) 失败"));
        return false;
    }

    if (req.count == 0) {
        setError("驱动没有分配任何 V4L2 buffer 槽位");
        return false;
    }

    releaseBuffers();
    m_buffers.reserve(req.count);

    DmaAllocator allocator(heapPath);

    for (uint32_t i = 0; i < req.count; ++i) {
        DmaMemory memory;
        if (!allocator.allocate(static_cast<size_t>(m_currentMode.sizeimg), memory)) {
            setError("DMA buffer 分配失败: " + allocator.lastError());
            releaseBuffers();
            return false;
        }

        DmaBuffer buffer {};
        buffer.index = static_cast<int>(i);
        buffer.memory = std::move(memory);
        buffer.queued = false;
        m_buffers.push_back(std::move(buffer));
    }

    for (DmaBuffer& buffer : m_buffers) {
        if (!queueBuffer(buffer.index)) {
            releaseBuffers();
            return false;
        }
    }

    m_state = State::BufferAllocated;
    m_lastError.clear();
    return true;
}

bool V4L2CameraSource::start()
{
    if (m_state != State::BufferAllocated) {
        setError("start 只能在 BufferAllocated 状态调用");
        return false;
    }

    v4l2_buf_type type = static_cast<v4l2_buf_type>(m_bufferType);
    if (ioctlRetry(m_v4l2Fd, VIDIOC_STREAMON, &type) < 0) {
        setError(errnoText("VIDIOC_STREAMON 失败"));
        return false;
    }

    m_state = State::Streaming;
    m_lastError.clear();
    return true;
}

void V4L2CameraSource::stop()
{
    if (m_state == State::Streaming && m_v4l2Fd >= 0) {
        v4l2_buf_type type = static_cast<v4l2_buf_type>(m_bufferType);
        (void)ioctlRetry(m_v4l2Fd, VIDIOC_STREAMOFF, &type);
        for (DmaBuffer& buffer : m_buffers) {
            buffer.queued = false;
        }
        m_state = m_buffers.empty() ? State::Configured : State::BufferAllocated;
    }
}

int V4L2CameraSource::cameraId() const
{
    return m_cameraId;
}

int V4L2CameraSource::fd() const
{
    return m_v4l2Fd;
}

V4L2CameraSource::State V4L2CameraSource::state() const
{
    return m_state;
}

bool V4L2CameraSource::isStreaming() const
{
    return m_state == State::Streaming;
}

bool V4L2CameraSource::isMultiPlanar() const
{
    return m_multiPlanar;
}

bool V4L2CameraSource::dequeueFrame(Frame& frame)
{
    if (m_state != State::Streaming) {
        setError("dequeueFrame 只能在 Streaming 状态调用");
        return false;
    }

    v4l2_plane planes[1] {};
    v4l2_buffer buf {};
    buf.type = static_cast<v4l2_buf_type>(m_bufferType);
    buf.memory = V4L2_MEMORY_DMABUF;
    if (m_multiPlanar) {
        buf.length = 1;
        buf.m.planes = planes;
    }

    if (ioctlRetry(m_v4l2Fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN) {
            setError(errnoText("VIDIOC_DQBUF 失败"));
        }
        return false;
    }

    if (!validateBufferIndex(static_cast<int>(buf.index))) {
        return false;
    }

    DmaBuffer& buffer = m_buffers[buf.index];
    buffer.queued = false;

    frame.width = m_currentMode.width;
    frame.height = m_currentMode.height;
    frame.stride = m_currentMode.stride;
    frame.format = m_currentMode.format;
    frame.v4l2Format = m_currentMode.v4l2Format;
    frame.bytesUsed = m_multiPlanar ? planes[0].bytesused : buf.bytesused;
    frame.index = static_cast<int>(buf.index);
    frame.dmaFd = buffer.memory.fd();
    frame.va = buffer.memory.va();
    frame.capacity = buffer.memory.size();
    frame.timestampUs =
        static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000ULL +
        static_cast<uint64_t>(buf.timestamp.tv_usec);
    frame.sequence = buf.sequence;

    return true;
}

bool V4L2CameraSource::requeueFrame(const Frame& frame)
{
    if (m_state != State::Streaming) {
        setError("requeueFrame 只能在 Streaming 状态调用");
        return false;
    }

    return queueBuffer(frame.index);
}

const V4L2CameraSource::CamConfig& V4L2CameraSource::currentConfig() const
{
    return m_currentConfig;
}

const V4L2CameraSource::VideoMode& V4L2CameraSource::currentMode() const
{
    return m_currentMode;
}

const std::string& V4L2CameraSource::lastError() const
{
    return m_lastError;
}

bool V4L2CameraSource::queueBuffer(int index)
{
    if (!validateBufferIndex(index)) {
        return false;
    }

    DmaBuffer& buffer = m_buffers[static_cast<size_t>(index)];
    if (buffer.queued) {
        setError("重复 QBUF 同一个 buffer");
        return false;
    }

    v4l2_plane planes[1] {};
    v4l2_buffer buf {};
    buf.type = static_cast<v4l2_buf_type>(m_bufferType);
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = static_cast<uint32_t>(index);
    if (m_multiPlanar) {
        buf.length = 1;
        buf.m.planes = planes;
        planes[0].length = static_cast<uint32_t>(buffer.memory.size());
        planes[0].m.fd = buffer.memory.fd();
    } else {
        buf.length = static_cast<uint32_t>(buffer.memory.size());
        buf.m.fd = buffer.memory.fd();
    }

    if (ioctlRetry(m_v4l2Fd, VIDIOC_QBUF, &buf) < 0) {
        setError(errnoText("VIDIOC_QBUF 失败"));
        return false;
    }

    buffer.queued = true;
    return true;
}

bool V4L2CameraSource::validateBufferIndex(int index) const
{
    if (index < 0 || static_cast<size_t>(index) >= m_buffers.size()) {
        return false;
    }
    return true;
}

void V4L2CameraSource::releaseBuffers()
{
    m_buffers.clear();
}

void V4L2CameraSource::setError(const std::string& message)
{
    m_lastError = message;
}
