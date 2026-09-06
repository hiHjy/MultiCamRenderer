#include "DmaBufferPool.hpp"
#include "Log.hpp"
#include "VideoFrame.hpp"
#include "drm_display.h"
#include "hw/MppDecoder.hpp"
#include "hw/RgaEngine.hpp"

#include "Live555RtspClient.hh"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <drm/drm_fourcc.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kOutputPoolBufferCount = 4;
constexpr size_t kMaxQueuedNalus = 512;
constexpr const char* kMppLumaDumpPath = "mpp_before_rga.pgm";
constexpr const char* kRgaLumaDumpPath = "rga_before_drm.pgm";

const char* codecName(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return "H264";
    case VideoCodec::H265:
        return "H265";
    }
    return "Unknown";
}

struct CompressedNalu {
    VideoCodec codec = VideoCodec::H264;
    uint64_t timestampUs = 0;
    std::vector<uint8_t> data;
};

// 将 NV12 的 Y 平面按真实 stride 去掉行尾填充后保存为 PGM。
// 小角在截图中是亮度异常，先看灰度图就足够区分 MPP/RGA/DRM 三段链路。
bool dumpNv12LumaAsPgm(const char* path, const VideoFrame& frame)
{
    if (path == nullptr || frame.format != PixelFormat::NV12 || frame.va == nullptr ||
        frame.width <= 0 || frame.height <= 0) {
        return false;
    }

    const int stride = videoFrameEffectiveStride(frame);
    if (stride < frame.width) {
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output << "P5\n" << frame.width << " " << frame.height << "\n255\n";
    const auto* yPlane = static_cast<const uint8_t*>(frame.va);
    for (int row = 0; row < frame.height; ++row) {
        output.write(reinterpret_cast<const char*>(yPlane + static_cast<size_t>(row) * stride),
                     frame.width);
    }
    return output.good();
}

// 直接 DRM 显示稳定 output pool 的图像。front/pending FramePacket 不能省：
// DRM page flip 尚未完成时，lease 必须继续持有 DMA buffer，不能让 pool 提前复用它。
class DrmPresenter {
public:
    ~DrmPresenter()
    {
        if (m_initialized) {
            drmDeinit(&m_ctx);
        }
    }

    bool present(FramePacket packet)
    {
        if (!ensureInitialized(packet.frame)) {
            ++m_droppedFrames;
            return false;
        }

        // 解码 worker 不是 live555 收包线程。这里最多等一个 VSync，能避免 poll(0)
        // 恰好早于 page-flip event 时把正常帧误判为 pending 而丢掉。
        consumeDrmEvents(20);
        if (m_ctx.pool.pending_idx >= 0) {
            ++m_droppedFrames;
            return true;
        }

        const VideoFrame& frame = packet.frame;
        const int stride = videoFrameEffectiveStride(frame);
        DRM_Buf buffer {};
        buffer.dma_fd = frame.dmaFd;
        buffer.size = frame.capacity;
        buffer.w = frame.width;
        buffer.h = frame.height;
        if (frame.format == PixelFormat::NV12) {
            buffer.fmt = DRM_FORMAT_NV12;
            buffer.pitches[0] = static_cast<uint32_t>(stride);
            buffer.pitches[1] = static_cast<uint32_t>(stride);
            buffer.offsets[1] = static_cast<uint32_t>(videoFramePlaneOffset(frame, 1));
        } else if (frame.format == PixelFormat::RGBA8888) {
            // RK_FORMAT_RGBA_8888 的 DMA 内存字节序是 R,G,B,A；DRM 的 ABGR8888
            // 在 little-endian 下正好使用相同的内存序。
            buffer.fmt = DRM_FORMAT_ABGR8888;
            buffer.pitches[0] = static_cast<uint32_t>(stride * 4);
        } else {
            LOG_ERROR("RtspDrmDemo", "DRM 不支持的输出格式");
            ++m_droppedFrames;
            return false;
        }
        buffer.offsets[0] = 0;
        buffer.modifier = DRM_FORMAT_MOD_INVALID;

        if (drmDisplaySubmit(&m_ctx, &buffer) != 0) {
            ++m_droppedFrames;
            if (errno != EAGAIN) {
                LOG_ERROR("RtspDrmDemo", "drmDisplaySubmit 失败: " << std::strerror(errno));
            }
            return false;
        }

        if (!m_hasFrontFrame) {
            m_frontPacket = std::move(packet);
            m_hasFrontFrame = true;
        } else {
            m_pendingPacket = std::move(packet);
        }

        ++m_displayedFrames;
        logStats(frame);
        return true;
    }

private:
    bool ensureInitialized(const VideoFrame& frame)
    {
        if ((frame.format != PixelFormat::NV12 && frame.format != PixelFormat::RGBA8888) ||
            frame.dmaFd < 0 || frame.width <= 0 || frame.height <= 0) {
            LOG_ERROR("RtspDrmDemo", "DRM 输入帧无效");
            return false;
        }

        if (m_initialized) {
            return true;
        }

        if (drmInit(&m_ctx) != 0) {
            LOG_ERROR("RtspDrmDemo", "drmInit 失败");
            return false;
        }

        DRM_Display_Config config {};
        config.fmt = frame.format == PixelFormat::NV12 ? DRM_FORMAT_NV12 : DRM_FORMAT_ABGR8888;
        config.mode_index = -1;
        config.src_x = 0;
        config.src_y = 0;
        config.src_w = frame.width;
        config.src_h = frame.height;
        config.crtc_x = 0;
        config.crtc_y = 0;
        config.crtc_w = frame.width;
        config.crtc_h = frame.height;

        if (drmDisplaySetupConfig(&m_ctx, &config) != 0) {
            LOG_ERROR("RtspDrmDemo", "drmDisplaySetupConfig 失败");
            drmDeinit(&m_ctx);
            return false;
        }

        m_initialized = true;
        LOG_INFO("RtspDrmDemo", "DRM 初始化完成 format="
                 << (frame.format == PixelFormat::NV12 ? "NV12" : "ABGR8888")
                 << " " << frame.width << "x" << frame.height
                 << " stride=" << videoFrameEffectiveStride(frame) << "x"
                 << videoFrameEffectiveHeightStride(frame));
        return true;
    }

    void consumeDrmEvents(int pendingWaitMs)
    {
        while (drmHandleEvents(&m_ctx, 0) > 0) {
        }

        if (m_ctx.pool.pending_idx >= 0 && pendingWaitMs > 0) {
            (void)drmHandleEvents(&m_ctx, pendingWaitMs);
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
            LOG_INFO("RtspDrmDemo", "display fps=" << (m_logFrames / elapsed.count())
                     << " size=" << frame.width << "x" << frame.height
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
    FramePacket m_frontPacket {};
    std::optional<FramePacket> m_pendingPacket {};
    uint64_t m_displayedFrames = 0;
    uint64_t m_droppedFrames = 0;
    int m_logFrames = 0;
    std::chrono::steady_clock::time_point m_logStart {};
};

class RtspDrmPipeline {
public:
    ~RtspDrmPipeline()
    {
        stop();
    }

    RtspDrmPipeline(const RtspDrmPipeline&) = delete;
    RtspDrmPipeline& operator=(const RtspDrmPipeline&) = delete;

    explicit RtspDrmPipeline(bool useRgbOutput)
        : m_outputFormat(useRgbOutput ? PixelFormat::RGBA8888 : PixelFormat::NV12)
    {
    }

    bool start(const std::string& url, bool rtpOverTcp)
    {
        if (m_started) {
            LOG_WARN("RtspDrmDemo", "pipeline 已经启动");
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_acceptingNalus = true;
            m_stopRequested = false;
            m_workerFailed = false;
            m_nalus.clear();
        }

        m_decodeThread = std::thread(&RtspDrmPipeline::decodeLoop, this);
        if (!m_client.start(url,
                            [this](VideoCodec codec, const uint8_t* data, size_t size, uint64_t timestampUs) {
                                onNalu(codec, data, size, timestampUs);
                            },
                            rtpOverTcp)) {
            setError("启动 live555 失败: " + m_client.lastError());
            requestDecodeThreadStop();
            if (m_decodeThread.joinable()) {
                m_decodeThread.join();
            }
            return false;
        }

        m_started = true;
        LOG_INFO("RtspDrmDemo", "已启动 RTSP -> MPP -> RGA -> DRM url=" << url
                 << " transport=" << (rtpOverTcp ? "tcp" : "udp")
                 << " drmOutput=" << (m_outputFormat == PixelFormat::NV12 ? "NV12" : "ABGR8888"));
        return true;
    }

    void stop()
    {
        if (!m_started && !m_decodeThread.joinable()) {
            return;
        }

        // stop() 会等待 live555 事件线程退出，之后队列中的 NALU 会由 decode worker 排空。
        m_client.stop();

        requestDecodeThreadStop();
        if (m_decodeThread.joinable()) {
            m_decodeThread.join();
        }

        m_started = false;
        LOG_INFO("RtspDrmDemo", "RTSP DRM pipeline 已停止");
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

private:
    void onNalu(VideoCodec codec, const uint8_t* data, size_t size, uint64_t timestampUs)
    {
        // AnnexBSink 已完成 RTP 分片重组，并补了 Annex-B start code。因为 live555 的
        // 回调内存只在本次调用期间有效，这里复制一个完整 NALU；MPP split_parse 会按
        // 顺序识别 slice/SPS/PPS，自己在凑足一张图时输出 NV12。
        if (data == nullptr || size == 0) {
            return;
        }

        CompressedNalu nalu;
        nalu.codec = codec;
        nalu.timestampUs = timestampUs;
        nalu.data.assign(data, data + size);
        enqueueNalu(std::move(nalu));
    }

    void enqueueNalu(CompressedNalu nalu)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_acceptingNalus || nalu.data.empty()) {
            return;
        }

        // H264/H265 的任意 NAL 丢失都可能破坏参考链。测试阶段宁可明确报背压错误；
        // 正式 RtspStream 再把这里设计成请求 IDR/重连的恢复策略。
        if (m_nalus.size() >= kMaxQueuedNalus) {
            ++m_droppedNalus;
            setError("解码输入 NALU 队列达到上限，不能安全丢弃 H264/H265 数据");
            return;
        }

        m_nalus.push_back(std::move(nalu));
        m_queueCv.notify_one();
    }

    void requestDecodeThreadStop()
    {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_acceptingNalus = false;
            m_stopRequested = true;
        }
        m_queueCv.notify_one();
    }

    void decodeLoop()
    {
        MppDecoder decoder;
        decoder.setFrameCallback([this](const VideoFrame& decodedFrame) {
            return copyAndPresent(decodedFrame);
        });

        while (true) {
            CompressedNalu nalu;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCv.wait(lock, [this] {
                    return m_stopRequested || !m_nalus.empty();
                });

                if (m_nalus.empty()) {
                    if (m_stopRequested) {
                        break;
                    }
                    continue;
                }

                nalu = std::move(m_nalus.front());
                m_nalus.pop_front();
            }

            const MppCodec codec = toMppCodec(nalu.codec);
            if (!m_decoderInitialized || codec != m_currentCodec) {
                decoder.deinit();
                if (!decoder.init(codec)) {
                    setError("MPP 解码器初始化失败: " + decoder.lastError());
                    m_workerFailed = true;
                    continue;
                }
                m_currentCodec = codec;
                m_decoderInitialized = true;
                LOG_INFO("RtspDrmDemo", "MPP 解码器已准备 codec=" << codecName(nalu.codec));
            }

            VideoFrame packet {};
            packet.va = nalu.data.data();
            packet.capacity = nalu.data.size();
            packet.bytesUsed = nalu.data.size();
            packet.timestampUs = nalu.timestampUs;
            packet.format = PixelFormat::Unknown;
            if (!decoder.sendPacket(packet)) {
                setError("MPP 解码失败: " + decoder.lastError());
                m_workerFailed = true;
            }
        }

        decoder.deinit();
    }

    bool copyAndPresent(const VideoFrame& decodedFrame)
    {
        if (decodedFrame.format != PixelFormat::NV12) {
            setError("当前 DRM demo 只支持 MPP 输出 NV12");
            return false;
        }

        if (!ensureOutputPool(decodedFrame)) {
            return false;
        }

        VideoFrame* outputFrame = m_outputPool->acquireFrame();
        if (outputFrame == nullptr) {
            setError("稳定输出池无空闲 buffer: " + m_outputPool->lastError());
            return false;
        }

        outputFrame->width = decodedFrame.width;
        outputFrame->height = decodedFrame.height;
        outputFrame->stride = m_outputFormat == PixelFormat::NV12
            ? videoFrameEffectiveStride(decodedFrame) : decodedFrame.width;
        outputFrame->heightStride = m_outputFormat == PixelFormat::NV12
            ? videoFrameEffectiveHeightStride(decodedFrame) : decodedFrame.height;
        outputFrame->format = m_outputFormat;
        outputFrame->nativeFormat = decodedFrame.nativeFormat;
        outputFrame->timestampUs = decodedFrame.timestampUs;
        outputFrame->sequence = ++m_outputSequence;

        const RgaOperation rgaOperation {
            m_outputFormat == PixelFormat::NV12 ? RgaOp::Copy : RgaOp::ConvertColor,
        };
        if (!m_rga.rga(decodedFrame, *outputFrame, rgaOperation)) {
            setError("RGA 输出 DRM 帧失败: " + m_rga.lastError());
            (void)m_outputPool->releaseFrame(outputFrame);
            return false;
        }

        if (!m_debugImagesSaved) {
            const bool savedMpp = dumpNv12LumaAsPgm(kMppLumaDumpPath, decodedFrame);
            const bool savedRga = dumpNv12LumaAsPgm(kRgaLumaDumpPath, *outputFrame);
            m_debugImagesSaved = true;
            LOG_INFO("RtspDrmDemo", "已保存同一帧的诊断图："
                     << kMppLumaDumpPath << " (MPP=" << (savedMpp ? "成功" : "失败") << ")，"
                     << kRgaLumaDumpPath << " (RGA=" << (savedRga ? "成功" : "失败") << ")");
        }

        const std::shared_ptr<DmaBufferPool> outputPool = m_outputPool;
        FramePacket stablePacket;
        stablePacket.frame = *outputFrame;
        stablePacket.lease = std::make_shared<FrameLease>([outputPool, outputFrame]() {
            (void)outputPool->releaseFrame(outputFrame);
        });

        if (!m_presenter.present(std::move(stablePacket))) {
            return false;
        }

        logPipelineStats(decodedFrame);
        return true;
    }

    bool ensureOutputPool(const VideoFrame& decodedFrame)
    {
        const int stride = m_outputFormat == PixelFormat::NV12
            ? videoFrameEffectiveStride(decodedFrame) : decodedFrame.width;
        const int heightStride = m_outputFormat == PixelFormat::NV12
            ? videoFrameEffectiveHeightStride(decodedFrame) : decodedFrame.height;
        const size_t payloadSize = videoFrameBufferSizeFor(m_outputFormat,
                                                            stride,
                                                            heightStride,
                                                            VideoBufferSizeMode::Payload);
        if (decodedFrame.width <= 0 || decodedFrame.height <= 0 || stride <= 0 ||
            heightStride <= 0 || payloadSize == 0) {
            setError("MPP 输出 frame layout 无效");
            return false;
        }

        if (m_outputPool) {
            if (m_outputWidth == decodedFrame.width && m_outputHeight == decodedFrame.height &&
                m_outputStride == stride && m_outputHeightStride == heightStride) {
                return true;
            }

            setError("检测到 RTSP 动态分辨率/layout 变化；第一版 DRM demo 需要重建 pipeline");
            return false;
        }

        auto pool = std::make_shared<DmaBufferPool>();
        if (!pool->init(kOutputPoolBufferCount, payloadSize)) {
            setError("初始化稳定输出池失败: " + pool->lastError());
            return false;
        }

        m_outputPool = std::move(pool);
        m_outputWidth = decodedFrame.width;
        m_outputHeight = decodedFrame.height;
        m_outputStride = stride;
        m_outputHeightStride = heightStride;
        LOG_INFO("RtspDrmDemo", "稳定输出池已创建 format="
                 << (m_outputFormat == PixelFormat::NV12 ? "NV12" : "RGBA8888")
                 << " count=" << kOutputPoolBufferCount
                 << " visible=" << m_outputWidth << "x" << m_outputHeight
                 << " stride=" << m_outputStride << "x" << m_outputHeightStride
                 << " payload=" << payloadSize);
        return true;
    }

    void logPipelineStats(const VideoFrame& decodedFrame)
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
            size_t queued = 0;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                queued = m_nalus.size();
            }
            LOG_INFO("RtspDrmDemo", "decode fps=" << (m_logFrames / elapsed.count())
                     << " size=" << decodedFrame.width << "x" << decodedFrame.height
                     << " queuedNalu=" << queued
                     << " droppedNalu=" << m_droppedNalus);
        }
        m_logFrames = 0;
        m_logStart = now;
    }

    void setError(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = error;
        LOG_ERROR("RtspDrmDemo", error);
    }

private:
    Live555RtspClient m_client;
    std::thread m_decodeThread;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<CompressedNalu> m_nalus;
    bool m_acceptingNalus = false;
    bool m_stopRequested = false;
    bool m_workerFailed = false;
    uint64_t m_droppedNalus = 0;

    RgaEngine m_rga;
    std::shared_ptr<DmaBufferPool> m_outputPool;
    DrmPresenter m_presenter;
    const PixelFormat m_outputFormat;
    MppCodec m_currentCodec = MppCodec::H264;
    bool m_decoderInitialized = false;
    int m_outputWidth = 0;
    int m_outputHeight = 0;
    int m_outputStride = 0;
    int m_outputHeightStride = 0;
    uint64_t m_outputSequence = 0;
    bool m_debugImagesSaved = false;
    int m_logFrames = 0;
    std::chrono::steady_clock::time_point m_logStart {};
    bool m_started = false;

    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};

} // namespace

int main(int argc, char** argv)
{
    const std::string url = argc > 1 ? argv[1] : "rtsp://192.168.1.5:8554/live";
    const int seconds = argc > 2 ? std::stoi(argv[2]) : 30;
    const bool rtpOverTcp = argc > 3 && std::string(argv[3]) == "tcp";
    const bool useRgbOutput = argc > 4 && std::string(argv[4]) == "rgb";

    RtspDrmPipeline pipeline(useRgbOutput);
    if (!pipeline.start(url, rtpOverTcp)) {
        LOG_ERROR("RtspDrmDemo", "启动失败: " << pipeline.lastError());
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    pipeline.stop();
    return 0;
}
