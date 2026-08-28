#include "FrameHub.hpp"

#include <string>
#include <vector>

FrameHub::FrameHub(int streamId)
	: m_streamId(streamId)
{
}

bool FrameHub::addSink(std::shared_ptr<Sink> sink)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!sink) {
		setError("sink 不能为空");
		return false;
	}

	m_sinks.push_back(sink);
	m_lastError.clear();
	return true;
}

bool FrameHub::publishFrame(const FramePacket& packet)
{
	std::vector<std::shared_ptr<Sink>> sinks;
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (m_streamId < 0) {
			setError("FrameHub 尚未绑定 streamId");
			return false;
		}

		if (packet.frame.streamId != m_streamId) {
			setError("VideoFrame streamId 和 FrameHub streamId 不匹配");
			return false;
		}

		sinks.reserve(m_sinks.size());
		for (auto it = m_sinks.begin(); it != m_sinks.end();) {
			if (auto sink = it->lock()) {
				sinks.push_back(std::move(sink));
				++it;
			} else {
				it = m_sinks.erase(it);
			}
		}
	}

	for (const std::shared_ptr<Sink>& sink : sinks) {
		sink->onFrame(packet);
	}

	m_lastError.clear();
	return true;
}

void FrameHub::removeSink()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_sinks.clear();
}

int FrameHub::streamId() const
{
	return m_streamId;
}

size_t FrameHub::sinkCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_sinks.size();
}

const std::string& FrameHub::lastError() const
{
	return m_lastError;
}

void FrameHub::setError(const std::string& message)
{
	m_lastError = message;
}
