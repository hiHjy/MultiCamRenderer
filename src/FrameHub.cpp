#include "FrameHub.hpp"

#include <exception>
#include <string>
#include <vector>

FrameHub::FrameHub(int streamId)
	: m_streamId(streamId)
{
}

bool FrameHub::addConsumer(std::unique_ptr<Consumer> consumer)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!consumer) {
		setError("consumer 不能为空");
		return false;
	}

	m_consumers.push_back(std::move(consumer));
	m_lastError.clear();
	return true;
}

bool FrameHub::publishFrame(const VideoFrame& frame)
{
	std::vector<Consumer*> consumers;
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (m_streamId < 0) {
			setError("FrameHub 尚未绑定 streamId");
			return false;
		}

		if (frame.streamId != m_streamId) {
			setError("VideoFrame streamId 和 FrameHub streamId 不匹配");
			return false;
		}

		consumers.reserve(m_consumers.size());
		for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
			if (!consumer) {
				setError("consumer 对象为空");
				return false;
			}
			consumers.push_back(consumer.get());
		}
	}

	for (Consumer* consumer : consumers) {
		try {
			consumer->onFrame(frame);
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
