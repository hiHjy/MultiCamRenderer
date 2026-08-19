#ifndef DISPLAYCONSUMER_HPP
#define DISPLAYCONSUMER_HPP

#include <QObject>
#include "Consumer.hpp"
#include "VideoFrame.hpp"
#include "DmaBufferPool.hpp"
#include "hw/RgaEngine.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

// 跨线程传递帧的轻量结构体——只包含标量字段，不含指针/fd 所有权
struct DisplayFrame {
    int dmaFd = -1;
    int width = 0;
    int height = 0;
    int stride = 0;       // 像素 stride（RGBA 紧密排布时 stride = width）
    int heightStride = 0;
    uint32_t nativeFormat = 0;
    uint64_t timestampUs = 0;
    uint64_t sequence = 0;
    int bufferIndex = -1;
};
Q_DECLARE_METATYPE(DisplayFrame)

class DisplayConsumer : public QObject, public Consumer {
    Q_OBJECT
public:
    static constexpr int WIDTH = 640;
    static constexpr int HEIGHT = 480;
    static constexpr int BUFFER_COUNT = 4;

    explicit DisplayConsumer(QObject *parent = nullptr);
    ~DisplayConsumer() override;
    void onFrame(FramePacket packet) override;

public slots:
    void releaseFrameByIndex(int bufferIndex);

signals:
    void frameReady(const DisplayFrame &frame);

private:
    void workerLoop();
    void processFrame(FramePacket packet);

private:
    RgaEngine m_rga;
    DmaBufferPool m_rgbaPool;
    std::array<VideoFrame*, BUFFER_COUNT> m_inFlightFrames {};
    std::mutex m_inFlightMutex;
    std::mutex m_pendingMutex;
    std::condition_variable m_pendingCv;
    std::optional<FramePacket> m_latestPacket;
    uint64_t m_droppedFrames = 0;
    bool m_stopping = false;
    std::thread m_workerThread;
    std::chrono::steady_clock::time_point m_fpsLogStart {};
    int m_fpsLogFrames = 0;
};

#endif // DISPLAYCONSUMER_HPP
