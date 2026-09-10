#include "RtspPublishSink.hpp"

#include "Live555RtspServer.hh"
#include "Log.hpp"

#include <utility>

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
        }
    }
    m_cv.notify_one();
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

        if (!encoder.init(m_config.encoderConfig)) {
            const std::string error = encoder.lastError();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_activeRequested = false;
                m_acceptingFrames = false;
                setErrorLocked("初始化 MPP 编码器失败: " + error);
            }
            LOG_ERROR("RtspPublishSink", "stream=" << m_config.streamName << ' ' << error);
            continue;
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
            LOG_INFO("RtspPublishSink", "stream=" << m_config.streamName << " 编码器已启动");
        }

        while (shouldEncode) {
            FramePacket packet;
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
            }

            // 当前 mpp_simple 封装会在 sendFrame() 内取回编码包后才返回；packet 的 lease
            // 因而会一直保护输入 dmaFd 到 MPP 使用完成。
            if (!encoder.sendFrame(packet.frame)) {
                const std::string error = encoder.lastError();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    setErrorLocked("MPP 编码失败: " + error);
                }
                LOG_ERROR("RtspPublishSink", "stream=" << m_config.streamName << ' ' << error);
            }
        }

        encoder.deinit();
        LOG_INFO("RtspPublishSink", "stream=" << m_config.streamName << " 编码器已停止");
    }
}

void RtspPublishSink::setErrorLocked(const std::string& message)
{
    m_lastError = message;
}
