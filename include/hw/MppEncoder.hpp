#pragma once

#include <string>

class MppEncoder {
public:
    MppEncoder() = default;
    ~MppEncoder() = default;

    const std::string& lastError() const;

private:
    std::string m_lastError;
};
