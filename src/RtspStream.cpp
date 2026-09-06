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
    if (m_started) {
        return true;
    }
    if (m_url.empty()) {
        setError("RTSP URL 为空");
        return false;
    }
	//启动解码线程
    if (!startDecodeWorker()) {
        setError("启动 RTSP 解码 worker 失败");
        return false;
    }

    clearError();
    if (!m_client->start(
            m_url,
            [this](VideoCodec codec, uint8_t* data, size_t size, uint64_t timestampUs) {
                onPacket(toMppCodec(codec), data, size, timestampUs);
            })) {
        setError("启动 live555 RTSP 客户端失败: " + m_client->lastError());
        stopDecodeWorker();
        return false;
    }

    m_started = true;
    return true;
}

bool RtspStream::stop()
{
    if (m_client != nullptr) {
        m_client->stop();
    }
    stopDecodeWorker();
    m_started = false;
    return true;
}
