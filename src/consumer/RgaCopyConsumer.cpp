#include "consumer/RgaCopyConsumer.hpp"

void RgaCopyConsumer::onFrame(const VideoFrame& frame)
{
    (void)frame;
}

const std::string& RgaCopyConsumer::lastError() const
{
    return m_lastError;
}
