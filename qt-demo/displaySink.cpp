#include "displaySink.hpp"

#include "Log.hpp"

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <utility>

namespace {

void dmabufSync(int fd, __u64 flags)
{
    if (fd < 0)
        return;

    dma_buf_sync sync {};
    sync.flags = flags;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        // Some exporters are already coherent or do not implement explicit sync.
        // Keep this non-fatal; RGA errors below are the ones that should stop a frame.
    }
}

void dmabufSyncStart(int fd, __u64 rw)
{
    dmabufSync(fd, DMA_BUF_SYNC_START | rw);
}

void dmabufSyncEnd(int fd, __u64 rw)
{
    dmabufSync(fd, DMA_BUF_SYNC_END | rw);
}

}

DisplaySink::DisplaySink(QObject *parent)
    : QObject(parent)
{
    const size_t bufferSize = RgaEngine::bufferSizeFor(PixelFormat::RGBA8888, WIDTH, HEIGHT);
    if (bufferSize == 0) {
        LOG_ERROR("DisplaySink", "计算 RGA 目标 DMA buffer size 失败");
    } else if (!m_rgbaPool.init(BUFFER_COUNT, bufferSize)) {
        LOG_ERROR("DisplaySink", "分配 RGA 目标 DMA buffer pool 失败: " << m_rgbaPool.lastError());
    }

    m_workerThread = std::thread(&DisplaySink::workerLoop, this);
}

DisplaySink::~DisplaySink()
{
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_stopping = true;
        m_latestPacket.reset();
    }
    m_pendingCv.notify_all();

    if (m_workerThread.joinable())
        m_workerThread.join();
}

void DisplaySink::onFrame(FramePacket packet)
{
    // 摄像头线程调用，只保留最新帧并立刻返回。
    // 如果 worker 还没来得及处理上一帧，旧 packet 会在这里析构并释放 lease。
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_latestPacket.has_value()) {
            ++m_droppedFrames;
            if (m_droppedFrames % 60 == 1) {
                LOG_WARN("DisplaySink", "drop pending frame seq="
                         << m_latestPacket->frame.sequence
                         << " total=" << m_droppedFrames);
            }
        }

        m_latestPacket = std::move(packet);
    }
    m_pendingCv.notify_one();
}

void DisplaySink::workerLoop()
{
    while (true) {
        FramePacket packet;
        {
            std::unique_lock<std::mutex> lock(m_pendingMutex);
            m_pendingCv.wait(lock, [this] {
                return m_stopping || m_latestPacket.has_value();
            });

            if (m_stopping && !m_latestPacket)
                break;

            packet = std::move(*m_latestPacket);
            m_latestPacket.reset();
        }

        processFrame(std::move(packet));
    }
}

void DisplaySink::processFrame(FramePacket packet)
{
    const VideoFrame &frame = packet.frame;
    // DisplaySink worker 线程调用。
    // 通过 RgaEngine::rga() 把摄像头 YUYV 转成 RGBA，存进我们私有 buffer。
    if (frame.width != WIDTH || frame.height != HEIGHT) {
        LOG_WARN("DisplaySink", "帧尺寸不匹配: " << frame.width << "x" << frame.height
                 << " expected=" << WIDTH << "x" << HEIGHT);
        return;
    }

    VideoFrame *dstFrame = m_rgbaPool.acquireFrame();
    if (!dstFrame) {
        LOG_WARN("DisplaySink", "RGA 目标 DMA buffer pool 无空闲帧: " << m_rgbaPool.lastError());
        return;
    }

    dstFrame->width = frame.width;
    dstFrame->height = frame.height;
    dstFrame->stride = frame.width;
    dstFrame->heightStride = frame.height;
    dstFrame->format = PixelFormat::RGBA8888;
    dstFrame->nativeFormat = frame.nativeFormat;
    dstFrame->bytesUsed = static_cast<size_t>(frame.width) * frame.height * 4;
    dstFrame->timestampUs = frame.timestampUs;
    dstFrame->sequence = frame.sequence;

    {
        std::lock_guard<std::mutex> lock(m_inFlightMutex);
        if (dstFrame->bufferIndex >= 0 && dstFrame->bufferIndex < BUFFER_COUNT) {
            m_inFlightFrames[static_cast<size_t>(dstFrame->bufferIndex)] = dstFrame;
        }
    }

    dmabufSyncStart(frame.dmaFd, DMA_BUF_SYNC_READ);
    // dmabufSyncStart(dstFrame->dmaFd, DMA_BUF_SYNC_WRITE);

    const bool rgaOk = m_rga.rga(frame, *dstFrame, RgaOperation {RgaOp::ConvertColor});

    // dmabufSyncEnd(dstFrame->dmaFd, DMA_BUF_SYNC_WRITE);
    dmabufSyncEnd(frame.dmaFd, DMA_BUF_SYNC_READ);

    if (!rgaOk) {
        LOG_ERROR("DisplaySink", "RGA YUYV->RGBA 转换失败: " << m_rga.lastError());
        releaseFrameByIndex(dstFrame->bufferIndex);
        return;
    }

    // RGA 已经把 V4L2 帧拷贝到显示私有 pool，后面只使用 dstFrame。
    // 这里主动释放 lease，让摄像头 buffer 可以尽早回到 CamManager 的 return queue。
    packet.lease.reset();

    // 转换成功，把稳定数据的 fd 抛给主线程
    DisplayFrame df;
    df.dmaFd = dstFrame->dmaFd;
    df.width = dstFrame->width;
    df.height = dstFrame->height;
    df.stride = dstFrame->stride;
    df.heightStride = dstFrame->heightStride;
    df.nativeFormat = dstFrame->nativeFormat;
    df.timestampUs = dstFrame->timestampUs;
    df.sequence = dstFrame->sequence;
    df.bufferIndex = dstFrame->bufferIndex;

    const auto now = std::chrono::steady_clock::now();
    if (m_fpsLogFrames == 0)
        m_fpsLogStart = now;
    ++m_fpsLogFrames;
    if (m_fpsLogFrames >= 60) {
        const std::chrono::duration<double> elapsed = now - m_fpsLogStart;
        if (elapsed.count() > 0.0) {
            LOG_INFO("DisplaySink", "fps=" << (m_fpsLogFrames / elapsed.count())
                     << " sequence=" << df.sequence
                     << " buffer=" << df.bufferIndex);
        }
        m_fpsLogFrames = 0;
        m_fpsLogStart = now;
    }

    emit frameReady(df);
}

void DisplaySink::releaseFrameByIndex(int bufferIndex)
{
    if (bufferIndex < 0 || bufferIndex >= BUFFER_COUNT)
        return;

    VideoFrame *frame = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_inFlightMutex);
        frame = m_inFlightFrames[static_cast<size_t>(bufferIndex)];
        m_inFlightFrames[static_cast<size_t>(bufferIndex)] = nullptr;
    }

    if (frame) {
        m_rgbaPool.releaseFrame(frame);
    }
}
