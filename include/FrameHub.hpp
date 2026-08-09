#pragma once

#include "Consumer.hpp"
#include "VideoFrame.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class FrameHub {
public:
	FrameHub() = default;
	explicit FrameHub(int streamId);

	bool addConsumer(std::shared_ptr<Consumer> consumer);
	bool publishFrame(const FramePacket& packet);

	int streamId() const;
	size_t consumerCount() const;
	const std::string& lastError() const;

private:
	void setError(const std::string& message);

private:
	int m_streamId = -1;
	mutable std::mutex m_mutex;
	std::vector<std::weak_ptr<Consumer>> m_consumers;
	std::string m_lastError;
};
