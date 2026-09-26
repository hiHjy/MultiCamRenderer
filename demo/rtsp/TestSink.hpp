#pragma once

#include "Sink.hpp"
#include "Log.hpp"

#include <iostream>

class TestSink : public Sink {
public:

	void onFrame(FramePacket packet) override
	{
		const VideoFrame& frame = packet.frame;
		LOG_INFO("TestSink", "id=" << m_n
		          << " stream=" << frame.streamId
		          << " seq=" << frame.sequence
		          << " ts=" << frame.timestampUs
		          << " " << frame.width << "x" << frame.height
		          << " stride=" << frame.stride
		          << " fmt=" << static_cast<int>(frame.format)
		          << " dmaFd=" << frame.dmaFd
		          << " va=" << frame.va
		          << " bytes=" << frame.bytesUsed
		          << "/" << frame.capacity
		          << " bufIdx=" << frame.bufferIndex);
	}

	TestSink() = default;
	TestSink(uint32_t n): m_n(n)
	{

	};
	~TestSink() override = default;
	uint32_t m_n;
};
