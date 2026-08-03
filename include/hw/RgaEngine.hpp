#pragma once

#include "VideoFrame.hpp"

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
    void copyMeta(const VideoFrame& src, VideoFrame& dst);
    void setError(const std::string& message);

private:
    std::string m_lastError;
};
