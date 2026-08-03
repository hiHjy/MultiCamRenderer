#pragma once

#include "Consumer.hpp"
#include "DmaFramePool.hpp"
#include "hw/RgaEngine.hpp"

#include <string>

class RgaCopyConsumer : public Consumer {
public:
    RgaCopyConsumer() = default;
    ~RgaCopyConsumer() override = default;

    void onFrame(const VideoFrame& frame) override;
    const std::string& lastError() const;

private:
    RgaEngine m_rga;
    DmaFramePool m_pool;
    std::string m_lastError;
};
