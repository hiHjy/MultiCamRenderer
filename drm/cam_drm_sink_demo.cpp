#include "CamManager.hpp"
#include "DmaBufferPool.hpp"
#include "Log.hpp"
#include "Sink.hpp"
#include "VideoTypes.hpp"
#include "drm_display.h"
#include "hw/RgaEngine.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <drm/drm_fourcc.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

class DrmSink final : public Sink {
public:
    ~DrmSink() override
    {
        if (m_initialized) {
            drmDeinit(&m_ctx);
        }
    }

    void onFrame(FramePacket packet) override
    {
        FramePacket displayPacket;
        if (packet.frame.format == PixelFormat::NV12) {
            displayPacket = std::move(packet);
        } else if (packet.frame.format == PixelFormat::YUYV) {
            displayPacket = convertToNv12(std::move(packet));
            if (!displayPacket.lease) {
                ++m_droppedFrames;
                return;
            }
        } else {
            ++m_droppedFrames;
            LOG_WARN("DrmSink", "只接受 NV12/YUYV，丢弃 format=" << static_cast<int>(packet.frame.format));
            return;
        }

        if (!ensureInitialized(displayPacket.frame)) {
            ++m_droppedFrames;
            return;
        }

        consumeDrmEvents();
        if (m_ctx.pool.pending_idx >= 0) {
            ++m_droppedFrames;
            return;
        }

        const VideoFrame& frame = displayPacket.frame;
        const int stride = videoFrameEffectiveStride(frame);

        DRM_Buf buf {};
        buf.dma_fd = frame.dmaFd;
        buf.size = frame.capacity;
        buf.w = frame.width;
        buf.h = frame.height;
        buf.fmt = DRM_FORMAT_NV12;
        buf.pitches[0] = static_cast<uint32_t>(stride);
        buf.pitches[1] = static_cast<uint32_t>(stride);
        buf.offsets[0] = 0;
        buf.offsets[1] = static_cast<uint32_t>(videoFramePlaneOffset(frame, 1));
        buf.modifier = DRM_FORMAT_MOD_INVALID;

        if (drmDisplaySubmit(&m_ctx, &buf) != 0) {
            ++m_droppedFrames;
            if (errno != EAGAIN) {
                LOG_ERROR("DrmSink", "drmDisplaySubmit 失败: " << std::strerror(errno));
            }
            return;
        }

        if (!m_hasFrontFrame) {
            m_frontPacket = std::move(displayPacket);
            m_hasFrontFrame = true;
        } else {
            m_pendingPacket = std::move(displayPacket);
        }

        ++m_displayedFrames;
        logStats(frame);
    }

private:
    bool ensureInitialized(const VideoFrame& frame)
    {
        if (m_initialized) {
            return true;
        }

        if (drmInit(&m_ctx) != 0) {
            LOG_ERROR("DrmSink", "drmInit 失败");
            return false;
        }

        DRM_Display_Config cfg {};
        cfg.fmt = DRM_FORMAT_NV12;
        cfg.mode_index = -1;
        cfg.src_x = 0;
        cfg.src_y = 0;
        cfg.src_w = frame.width;
        cfg.src_h = frame.height;
        cfg.crtc_x = 0;
        cfg.crtc_y = 0;
        cfg.crtc_w = frame.width;
        cfg.crtc_h = frame.height;

        if (drmDisplaySetupConfig(&m_ctx, &cfg) != 0) {
            LOG_ERROR("DrmSink", "drmDisplaySetupConfig 失败");
            drmDeinit(&m_ctx);
            return false;
        }

        m_initialized = true;
        LOG_INFO("DrmSink", "DRM NV12 显示初始化完成 size="
                 << frame.width << "x" << frame.height
                 << " stride=" << frame.stride << "x" << frame.heightStride);
        return true;
    }

    FramePacket convertToNv12(FramePacket packet)
    {
        const VideoFrame& frame = packet.frame;
        if (!ensureNv12Pool(frame)) {
            return {};
        }

        VideoFrame* outputFrame = m_nv12Pool.acquireFrame();
        if (!outputFrame) {
            LOG_WARN("DrmSink", "NV12 输出池无空闲帧: " << m_nv12Pool.lastError());
            return {};
        }

        outputFrame->width = frame.width;
        outputFrame->height = frame.height;
        outputFrame->stride = frame.width;
        outputFrame->heightStride = frame.height;
        outputFrame->format = PixelFormat::NV12;
        outputFrame->bytesUsed = videoFrameBufferSize(*outputFrame);
        outputFrame->timestampUs = frame.timestampUs;
        outputFrame->sequence = frame.sequence;

        if (!m_rga.rga(frame, *outputFrame, RgaOperation {RgaOp::ConvertColor})) {
            LOG_ERROR("DrmSink", "RGA YUYV->NV12 失败: " << m_rga.lastError());
            (void)m_nv12Pool.releaseFrame(outputFrame);
            return {};
        }

        packet.lease.reset();

        FramePacket outputPacket {
            .frame = *outputFrame,
            .lease = std::make_shared<FrameLease>(
                [this, outputFrame]() {
                    (void)m_nv12Pool.releaseFrame(outputFrame);
                }),
        };
        return outputPacket;
    }

    bool ensureNv12Pool(const VideoFrame& frame)
    {
        if (m_nv12Pool.initialized()) {
            return true;
        }

        const size_t bufferSize = RgaEngine::bufferSizeFor(PixelFormat::NV12, frame.width, frame.height);
        if (bufferSize == 0) {
            LOG_ERROR("DrmSink", "计算 NV12 输出池大小失败");
            return false;
        }

        if (!m_nv12Pool.init(4, bufferSize)) {
            LOG_ERROR("DrmSink", "初始化 NV12 输出池失败: " << m_nv12Pool.lastError());
            return false;
        }

        LOG_INFO("DrmSink", "初始化 YUYV->NV12 输出池 size="
                 << frame.width << "x" << frame.height
                 << " bufferSize=" << bufferSize);
        return true;
    }

    void consumeDrmEvents()
    {
        if (!m_initialized) {
            return;
        }

        while (drmHandleEvents(&m_ctx, 0) > 0) {
        }

        if (m_ctx.pool.pending_idx < 0 && m_pendingPacket.has_value()) {
            m_frontPacket = std::move(*m_pendingPacket);
            m_pendingPacket.reset();
        }
    }

    void logStats(const VideoFrame& frame)
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_logFrames == 0) {
            m_logStart = now;
        }

        ++m_logFrames;
        if (m_logFrames < 60) {
            return;
        }

        const std::chrono::duration<double> elapsed = now - m_logStart;
        if (elapsed.count() > 0.0) {
            LOG_INFO("DrmSink", "fps=" << (m_logFrames / elapsed.count())
                     << " size=" << frame.width << "x" << frame.height
                     << " stride=" << frame.stride << "x" << frame.heightStride
                     << " sequence=" << frame.sequence
                     << " displayed=" << m_displayedFrames
                     << " dropped=" << m_droppedFrames);
        }

        m_logFrames = 0;
        m_logStart = now;
    }

private:
    DRM_Ctx m_ctx {};
    bool m_initialized = false;
    bool m_hasFrontFrame = false;
    RgaEngine m_rga {};
    DmaBufferPool m_nv12Pool {};
    FramePacket m_frontPacket {};
    std::optional<FramePacket> m_pendingPacket {};
    uint64_t m_displayedFrames = 0;
    uint64_t m_droppedFrames = 0;
    int m_logFrames = 0;
    std::chrono::steady_clock::time_point m_logStart {};
};

} // namespace

int main(int argc, char** argv)
{
    const std::string device = argc > 1 ? argv[1] : "/dev/video12";
    const int seconds = argc > 2 ? std::stoi(argv[2]) : 30;
    const std::string mode = argc > 3 ? argv[3] : "mjpeg";

    CamManager manager;
    CamManager::CameraConfig config {};
    config.devicePath = device;
    config.width = 640;
    config.height = 480;
    config.fps = 30;
    config.format = mode == "yuyv" ? PixelFormat::YUYV : PixelFormat::MJPEG;
    config.bufferCount = 4;

    const int cameraId = manager.addCamera(config);
    if (cameraId < 0) {
        LOG_ERROR("DrmDemo", "addCamera 失败: " << manager.lastError());
        return 1;
    }

    auto sink = std::make_shared<DrmSink>();
    if (!manager.addFrameSink(cameraId, sink)) {
        LOG_ERROR("DrmDemo", "addFrameSink 失败: " << manager.lastError());
        return 1;
    }

    manager.startPolling();
    if (!manager.startCamera(cameraId)) {
        LOG_ERROR("DrmDemo", "startCamera 失败: " << manager.lastError());
        manager.shutdownPolling();
        return 1;
    }

    LOG_INFO("DrmDemo", "开始 DRM 直显测试 device=" << device
             << " seconds=" << seconds
             << " mode=" << mode);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    manager.delCamera(cameraId);
    manager.shutdownPolling();
    LOG_INFO("DrmDemo", "DRM 直显测试结束");
    return 0;
}
