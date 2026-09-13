#include "RtspStream.hpp"

#include "Live555RtspClient.hh"

#include <utility>

RtspStream::RtspStream(std::string url, size_t readyQueueCapacity)
    : Stream(readyQueueCapacity),
      m_url(std::move(url)),
      m_client(std::make_unique<Live555RtspClient>())
{
}

RtspStream::~RtspStream()
{
    (void)stop();
}

bool RtspStream::start()
{
    if (m_started.load()) {
        return true;
    }
    if (m_url.empty()) {
        setError("RTSP URL 为空");
        return false;
    }
    // 异步 RTSP 失败后 DecodeWorker 会保留到下一次 start；重连前先确保旧 MPP 状态已释放。
    stopDecodeWorker();
    if (!startDecodeWorker()) {
        setError("启动 RTSP 解码 worker 失败");
        return false;
    }

    clearError();
    m_started.store(true);

	//启动live555线程
	// 第一个lambda：当网络数据包到达时调用，用于将压缩数据包送给项目框架，
	// 第二个lambda，握手状态变化时调用，用于上报状态
    if (!m_client->start(
            m_url,
            [this](VideoCodec codec, uint8_t* data, size_t size, uint64_t timestampUs) {
                onPacket(codec, data, size, timestampUs);
            },
            false,
            [this](Live555RtspClient::State state, const std::string& message) {
                onRtspClientState(state, message);
            })) {
        setError("启动 live555 RTSP 客户端失败: " + m_client->lastError());
        m_started.store(false);
        stopDecodeWorker();
        return false;
    }

    return true;
}

bool RtspStream::stop()
{
    if (m_client != nullptr) {
        m_client->stop();
    }
    stopDecodeWorker();
    m_started.store(false);
    reportRuntimeState(RuntimeState::Stopped);
    return true;
}

void RtspStream::onRtspClientState(Live555RtspClient::State state, const std::string& message)
{
    switch (state) {
    case Live555RtspClient::State::Connecting:
        reportRuntimeState(RuntimeState::Connecting);
        return;
    case Live555RtspClient::State::Playing:
        reportRuntimeState(RuntimeState::Streaming);
        return;
    case Live555RtspClient::State::Stopped:
        m_started.store(false);
        reportRuntimeState(RuntimeState::Stopped);
        return;
    case Live555RtspClient::State::Error:
        m_started.store(false);
        clearDecodePackets();
        setError(message);
        reportRuntimeState(RuntimeState::Error, message);
        return;
    }
}
