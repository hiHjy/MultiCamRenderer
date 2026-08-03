#pragma once

#include <string>

class MppDecoder {
public:
    MppDecoder() = default;
    ~MppDecoder() = default;

    const std::string& lastError() const;

private:
    std::string m_lastError;
};
