#include "RtspPublishSink.hpp"

#include "Live555RtspServer.hh"
#include "Log.hpp"

#include <chrono>
#include <ctime>
#include <memory>
#include <utility>

namespace {

std::string formatOsdLocalTime()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime {};
    localtime_r(&now, &localTime);

    char text[32] {};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &localTime);
    return std::string(text);
}

VideoFrame makeOsdOutputFrame(const DmaMemory& memory, const VideoFrame& source)
{
    VideoFrame frame;
    frame.dmaFd = memory.fd();
    frame.va = memory.va();
    frame.capacity = memory.size();
    frame.width = source.width;
    frame.height = source.height;
    frame.stride = videoFrameEffectiveStride(source);
    frame.heightStride = videoFrameEffectiveHeightStride(source);
    frame.format = PixelFormat::NV12;
    frame.bytesUsed = RgaEngine::bufferSizeFor(frame.format,
                                                frame.stride,
                                                frame.heightStride,
                                                64);
    return frame;
}

void fillEncoderInputLayout(MppEncoderConfig& config, const VideoFrame& frame)
{
    config.width = frame.width;
    config.height = frame.height;
    config.stride = videoFrameEffectiveStride(frame);
    config.heightStride = videoFrameEffectiveHeightStride(frame);
    config.inputFormat = frame.format;
}

} // namespace

RtspPublishSink::RtspPublishSink(Live555RtspServer& server, Config config)
    : m_server(server),
      m_config(std::move(config)),
      m_workerThread(&RtspPublishSink::workerMain, this)
{
}

RtspPublishSink::~RtspPublishSink()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeRequested = false;
        m_acceptingFrames = false;
        m_pendingFrame.reset();
        m_stopping = true;
    }
    m_cv.notify_one();
    if (m_workerThread.joinable())
        m_workerThread.join();
}

void RtspPublishSink::setActive(bool active)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping)
            return;

        m_activeRequested = active;
        if (!active) {
            // optional 被销毁时会释放旧 FrameLease，CamManager 随后可将 V4L2 buffer QBUF。
            m_acceptingFrames = false;
            m_pendingFrame.reset();
            m_keyFrameRequested = false;
        }
    }
    m_cv.notify_one();
}

void RtspPublishSink::requestKeyFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping || !m_activeRequested)
        return;

    // 只需要下一帧成为 IDR；同一时刻多个新客户端加入时无需重复向 MPP 发控制命令。
    m_keyFrameRequested = true;
}

void RtspPublishSink::onFrame(FramePacket packet)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_acceptingFrames)
            return;

        if (m_pendingFrame.has_value())
            ++m_droppedFrames;

        // 仅缓存一张尚未交给 MPP 的帧；emplace 会释放被替换的旧 FrameLease。
        m_pendingFrame.emplace(std::move(packet));
    }
    m_cv.notify_one();
}

bool RtspPublishSink::isActive() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_acceptingFrames;
}

uint64_t RtspPublishSink::droppedFrameCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_droppedFrames;
}

std::string RtspPublishSink::lastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

void RtspPublishSink::workerMain()
{
    MppEncoder encoder;
    RgaEngine rga;
    OsdRenderer osd;
    DmaAllocator osdOutputAllocator;
    DmaMemory osdOutputMemory;
    VideoFrame osdOutputFrame;
    std::string lastOsdTime;
    encoder.setPacketCallback([this](const EncodedPacket& packet) {
        // EncodedPacket::data 仅在本回调中有效；Live555RtspServer 会立即复制 Annex-B 数据。
        return m_server.pushAnnexBFrame(m_config.streamName,
                                        packet.data,
                                        packet.size,
                                        packet.timestampUs);
    });

    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stopping || m_activeRequested; });
            if (m_stopping)
                break;
        }

        if (m_config.enableOsd) {
            if (!osd.initialize(m_config.osdConfig)) {
                const std::string error = osd.lastError();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_activeRequested = false;
                    m_acceptingFrames = false;
                    setErrorLocked("初始化 OSD 失败: " + error);
                }
                LOG_ERROR("RtspPublishSink", "stream=" << m_config.streamName
                                                           << " 初始化 OSD 失败: " << error);
                continue;
            }
            lastOsdTime.clear();
            LOG_INFO("RtspPublishSink", "stream=" << m_config.streamName
                                                      << " OSD 已启动，等待第一张 VPSS 帧确定输出 layout，文字层="
                                                      << m_config.osdConfig.overlayWidth << "x"
                                                      << m_config.osdConfig.overlayHeight);
        }

        bool shouldEncode = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            shouldEncode = !m_stopping && m_activeRequested;
            m_acceptingFrames = shouldEncode;
            if (shouldEncode)
                m_lastError.clear();
        }
        if (shouldEncode) {
            LOG_INFO("RtspPublishSink", "stream=" << m_config.streamName
                                                      << " 已开始接收裸帧，收到第一张后初始化编码器");
        }

        bool encoderInitialized = false;
        // m_config 是启动期策略；每次 active 周期根据真实首帧生成本次 MPP prep 配置。
        MppEncoderConfig activeEncoderConfig = m_config.encoderConfig;
        while (shouldEncode) {
            FramePacket packet;
            bool requestKeyFrame = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] {
                    return m_stopping || !m_activeRequested || m_pendingFrame.has_value();
                });

                if (m_stopping || !m_activeRequested) {
                    m_acceptingFrames = false;
                    m_pendingFrame.reset();
                    shouldEncode = false;
                    continue;
                }

                packet = std::move(*m_pendingFrame);
                m_pendingFrame.reset();
                requestKeyFrame = m_keyFrameRequested;
                m_keyFrameRequested = false;
            }

            const VideoFrame* frameToEncode = &packet.frame;
            if (m_config.enableOsd) {
                // 私有 NV12 输出 buffer 的 layout 必须与实际 VPSS 输入一致，因此不能从
                // 配置猜宽高/stride，收到第一张图像后才创建。
                if (!osdOutputMemory.valid()) {
                    const int outputStride = videoFrameEffectiveStride(packet.frame);
                    const int outputHeightStride = videoFrameEffectiveHeightStride(packet.frame);
                    const size_t outputBytes = RgaEngine::bufferSizeFor(PixelFormat::NV12,
                                                                        outputStride,
                                                                        outputHeightStride,
                                                                        64);
                    if (!osdOutputAllocator.allocate(outputBytes, osdOutputMemory)) {
                        const std::string error = osdOutputAllocator.lastError();
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            m_activeRequested = false;
                            m_acceptingFrames = false;
                            setErrorLocked("申请 OSD 输出 DMA-BUF 失败: " + error);
                        }
                        LOG_ERROR("RtspPublishSink", "stream=" << m_config.streamName
                                                                   << " 申请 OSD 输出 DMA-BUF 失败: " << error);
                        shouldEncode = false;
                        continue;
                    }
                    osdOutputFrame = makeOsdOutputFrame(osdOutputMemory, packet.frame);
                    LOG_INFO("RtspPublishSink", "stream=" << m_config.streamName
                                                              << " OSD 输出 layout="
                                                              << osdOutputFrame.width << "x" << osdOutputFrame.height
                                                              << " stride=" << osdOutputFrame.stride << "x"
                                                              << osdOutputFrame.heightStride);
                }

                const std::string currentOsdTime = formatOsdLocalTime();
                if (currentOsdTime != lastOsdTime) {
                    if (!osd.updateText(m_config.streamName + "  " + currentOsdTime)) {
                        const std::string error = osd.lastError();
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            setErrorLocked("更新 OSD 文字失败: " + error);
                        }
                        LOG_ERROR("RtspPublishSink", "stream=" << m_config.streamName
                                                                   << " 更新 OSD 文字失败: " << error);
                        continue;
                    }
                    lastOsdTime = currentOsdTime;
                }
                if (!osd.composite(packet.frame, osdOutputFrame, rga)) {
                    const std::string error = osd.lastError();
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        setErrorLocked("OSD 合成失败: " + error);
                    }
                    LOG_ERROR("RtspPublishSink", "stream=" << m_config.streamName
                                                               << " OSD 合成失败: " << error);
                    continue;
                }
                frameToEncode = &osdOutputFrame;
            }

            // 当前 mpp_simple 封装会在 sendFrame() 内取回编码包后才返回；packet 的 lease
            // 因而会一直保护输入 dmaFd 到 MPP 使用完成。MPP 初始化也必须取真实输入帧
            // 的 layout；编码器新建后的第一张图像天然从 IDR 序列开始。
            if (!encoderInitialized) {
                fillEncoderInputLayout(activeEncoderConfig, *frameToEncode);
                if (!encoder.init(activeEncoderConfig)) {
                    const std::string error = encoder.lastError();
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_activeRequested = false;
                        m_acceptingFrames = false;
                        setErrorLocked("初始化 MPP 编码器失败: " + error);
                    }
                    LOG_ERROR("RtspPublishSink", "stream=" << m_config.streamName << ' ' << error);
                    shouldEncode = false;
                    continue;
                }
                encoderInitialized = true;
                // 本次 setActive() 刚建立的编码器首帧不用额外请求 IDR；请求合并标志到此
                // 已没有意义，清掉即可。后续新客户端才会走 requestKeyFrame()。
                requestKeyFrame = false;
                LOG_INFO("RtspPublishSink", "stream=" << m_config.streamName << " 已按第一张真实帧补齐 MPP prep 配置并初始化编码器");
            }

            if (requestKeyFrame) {
                if (!encoder.requestKeyFrame()) {
                    // 强制 IDR 失败不应阻断普通编码；编码器的周期 IDR 仍能让客户端恢复。
                    LOG_WARN("RtspPublishSink", "stream=" << m_config.streamName
                                                                << " 请求下一帧 IDR 失败: "
                                                                << encoder.lastError());
                } else {
                    LOG_INFO("RtspPublishSink", "stream=" << m_config.streamName
                                                                << " 已请求 MPP 下一帧 IDR");
                }
            }

            if (!encoder.sendFrame(*frameToEncode)) {
                const std::string error = encoder.lastError();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    setErrorLocked("MPP 编码失败: " + error);
                }
                LOG_ERROR("RtspPublishSink", "stream=" << m_config.streamName << ' ' << error);
            }
        }

        encoder.deinit();
        // 下一次有客户端时重新申请，避免无人观看时保留不必要的 OSD DMA 内存。
        osdOutputMemory.reset();
        osdOutputFrame = {};
        lastOsdTime.clear();
        LOG_INFO("RtspPublishSink", "stream=" << m_config.streamName << " 编码器已停止");
    }
}

void RtspPublishSink::setErrorLocked(const std::string& message)
{
    m_lastError = message;
}
