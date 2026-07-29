#pragma once

#include "Consumer.hpp"

#include <iostream>

class TestConsumer : public Consumer {
public:
	void onFrame(const VideoFrame &frame) override {
		std::cout << " TestConsumer:"
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
		          << " bufIdx=" << frame.bufferIndex
		          << std::endl;
	}


	TestConsumer() = default;
	~TestConsumer() = default;

};
