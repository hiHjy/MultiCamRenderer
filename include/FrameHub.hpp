#pragma once

#include "Consumer.hpp"
#include "VideoFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FrameHub {

public:
	FrameHub() = default;
	explicit FrameHub(uint32_t streamId)
		: m_streamId(streamId)
	{
	}
	void addConsumer(std::unique_ptr<Consumer> consumer) {
		m_consumers.push_back(std::move(consumer));
		
	}

	bool publishFrame(const VideoFrame& frame) {
		for(auto& consumer :  m_consumers) {
			consumer->onFrame(frame);
		}
		return true;
	}

private:
	uint32_t m_streamId = 0;
	std::vector<std::unique_ptr<Consumer>> m_consumers;
};
