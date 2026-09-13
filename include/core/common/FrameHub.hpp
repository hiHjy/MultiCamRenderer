#pragma once

#include "Sink.hpp"
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

	bool addSink(std::shared_ptr<Sink> sink);
	bool publishFrame(const FramePacket& packet);
	void close();
	void removeSink();
	int streamId() const;
	size_t sinkCount() const;
	const std::string& lastError() const;

private:
	void setError(const std::string& message);

private:
	int m_streamId = -1;
	mutable std::mutex m_publishMutex;
	mutable std::mutex m_mutex;
	std::vector<std::weak_ptr<Sink>> m_sinks;
	bool m_closed = false;
	std::string m_lastError;
};
