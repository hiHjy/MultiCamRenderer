#include "FrameHub.hpp"

#include <exception>
#include <string>
#include <vector>

FrameHub::FrameHub(int streamId)
	: m_streamId(streamId)
{
}

bool FrameHub::addConsumer(std::shared_ptr<Consumer> consumer)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!consumer) {
		setError("consumer 不能为空");
		return false;
	}

	m_consumers.push_back(consumer);
	m_lastError.clear();
	return true;
}

bool FrameHub::publishFrame(const FramePacket& packet)
{
	std::vector<std::shared_ptr<Consumer>> consumers;
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

		consumers.reserve(m_consumers.size());
		for (auto it = m_consumers.begin(); it != m_consumers.end();) {
			if (auto consumer = it->lock()) {
				consumers.push_back(std::move(consumer));
				++it;
			} else {
				it = m_consumers.erase(it);
			}
		}
	}

	for (const std::shared_ptr<Consumer>& consumer : consumers) {
		try {
			consumer->onFrame(packet);
		} catch (const std::exception& e) {
			setError(std::string("consumer onFrame 异常: ") + e.what());
			return false;
		} catch (...) {
			setError("consumer onFrame 未知异常");
			return false;
		}
	}

	m_lastError.clear();
	return true;
}

int FrameHub::streamId() const
{
	return m_streamId;
}

size_t FrameHub::consumerCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_consumers.size();
}

const std::string& FrameHub::lastError() const
{
	return m_lastError;
}

void FrameHub::setError(const std::string& message)
{
	m_lastError = message;
}
