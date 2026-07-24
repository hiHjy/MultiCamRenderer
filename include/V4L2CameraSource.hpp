#pragma once

#include "DmaAllocator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class V4L2CameraSource {
public:
    explicit V4L2CameraSource(int cameraId);
    ~V4L2CameraSource();

    enum class State {
        Closed,
        Opened,
        Configured,
        BufferAllocated,
        Streaming
    };

    enum class PixelFormat {
        Auto,
        NV12,
        YUYV,
        YUV420P
    };

    struct DmaBuffer {
        int index = -1;
        DmaMemory memory;
        bool queued = false;
    };

    struct VideoMode {
        int width = 0;
        int height = 0;
        int fps = 0;
        int stride = 0;
        uint64_t sizeimg = 0;
        PixelFormat format = PixelFormat::Auto;
        uint32_t v4l2Format = 0;
    };

    struct CamConfig {
        int width = 0;
        int height = 0;
        int fps = 0;
        PixelFormat format = PixelFormat::Auto;
    };

    // dequeue 出来的 V4L2 buffer 借用视图。
    // dmaFd/va 归 V4L2CameraSource 持有，调用方不能 close/munmap，
    // 并且不能在 requeueFrame() 后继续使用。
    struct Frame {
        int width = 0;
        int height = 0;
        int stride = 0;
        PixelFormat format = PixelFormat::Auto;
        uint32_t v4l2Format = 0;
        size_t bytesUsed = 0;
        int index = -1;
        int dmaFd = -1;
        void* va = nullptr;
        size_t capacity = 0;

        uint64_t timestampUs = 0;
        uint64_t sequence = 0;
    };

    bool openDevice(const std::string& devicePath);
    void closeDevice();

    bool configure(const CamConfig& cfg);
    bool getCurrentConfig(CamConfig& cfg);

    bool setupDmaImportBuffers(
        int bufferCount,
        const std::string& heapPath = "/dev/dma_heap/system"
    );

    bool start();
    void stop();

    int cameraId() const;
    int fd() const;
    State state() const;
    bool isStreaming() const;
    bool isMultiPlanar() const;

    bool dequeueFrame(Frame& frame);
    bool requeueFrame(const Frame& frame);

    const CamConfig& currentConfig() const;
    const VideoMode& currentMode() const;
    const std::string& lastError() const;

private:
    bool queueBuffer(int index);
    bool validateBufferIndex(int index) const;
    void releaseBuffers();
    void setError(const std::string& message);

private:
    int m_cameraId = -1;
    int m_v4l2Fd = -1;
    uint32_t m_bufferType = 0;
    bool m_multiPlanar = false;
    State m_state = State::Closed;
    VideoMode m_currentMode {};
    CamConfig m_currentConfig {};
    std::vector<DmaBuffer> m_buffers;
    std::string m_lastError;
};
