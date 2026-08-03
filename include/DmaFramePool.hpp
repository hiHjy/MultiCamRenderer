#pragma once

#include "DmaAllocator.hpp"
#include "VideoFrame.hpp"

#include <string>
#include <vector>

class DmaFramePool {
public:
    DmaFramePool() = default;
    ~DmaFramePool() = default;

    const std::string& lastError() const;

private:
    struct Slot {
        DmaMemory memory;
        VideoFrame frame;
        bool inUse = false;
    };

private:
    std::vector<Slot> m_slots;
    std::string m_lastError;
};
