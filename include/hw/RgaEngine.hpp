#pragma once

#include "VideoFrame.hpp"

#include <cstddef>
#include <string>

class RgaEngine {
public:
    RgaEngine() = default;

    bool copy(const VideoFrame& src, VideoFrame& dst);
    bool resize(const VideoFrame& src, VideoFrame& dst);

    const std::string& lastError() const;

private:
    int toRgaFormat(PixelFormat format);
    bool validateFrame(const VideoFrame& frame, const char* name);
    bool validateSameFormat(const VideoFrame& src, const VideoFrame& dst);
    bool validateCapacity(const VideoFrame& frame, const char* name);
    int effectiveStride(const VideoFrame& frame) const;
    int effectiveHeightStride(const VideoFrame& frame) const;
    size_t requiredSize(const VideoFrame& frame);
    void copyMeta(const VideoFrame& src, VideoFrame& dst);
    void setError(const std::string& message);

private:
    std::string m_lastError;
};
