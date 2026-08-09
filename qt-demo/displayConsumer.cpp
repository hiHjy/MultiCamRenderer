#include "displayConsumer.hpp"

#include <QDebug>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

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

DisplayConsumer::DisplayConsumer(QObject *parent)
    : QObject(parent)
{
    const size_t bufferSize = RgaEngine::bufferSizeFor(PixelFormat::RGBA8888, WIDTH, HEIGHT);
    if (bufferSize == 0) {
        qWarning() << "DisplayConsumer: 计算 RGA 目标 DMA buffer size 失败";
        return;
    }

    if (!m_rgbaPool.init(BUFFER_COUNT, bufferSize)) {
        qWarning() << "DisplayConsumer: 分配 RGA 目标 DMA buffer pool 失败:"
                   << QString::fromStdString(m_rgbaPool.lastError());
    }
}

void DisplayConsumer::onFrame(const FramePacket &packet)
{
    const VideoFrame &frame = packet.frame;
    // 摄像头线程调用，需快速返回。
    // 通过 RgaEngine::rga() 把摄像头 YUYV 转成 RGBA，存进我们私有 buffer。
    if (frame.width != WIDTH || frame.height != HEIGHT) {
        qWarning() << "DisplayConsumer: 帧尺寸不匹配:"
                   << frame.width << "x" << frame.height;
        return;
    }

    VideoFrame *dstFrame = m_rgbaPool.acquireFrame();
    if (!dstFrame) {
        qWarning() << "DisplayConsumer: RGA 目标 DMA buffer pool 无空闲帧:"
                   << QString::fromStdString(m_rgbaPool.lastError());
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
        qWarning() << "DisplayConsumer: RGA YUYV->RGBA 转换失败:"
                   << QString::fromStdString(m_rga.lastError());
        releaseFrameByIndex(dstFrame->bufferIndex);
        return;
    }

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
            qDebug() << "DisplayConsumer FPS:" << (m_fpsLogFrames / elapsed.count())
                     << "sequence:" << df.sequence
                     << "buffer:" << df.bufferIndex;
        }
        m_fpsLogFrames = 0;
        m_fpsLogStart = now;
    }

    emit frameReady(df);
}

void DisplayConsumer::releaseFrameByIndex(int bufferIndex)
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
