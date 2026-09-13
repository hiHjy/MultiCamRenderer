#pragma once

#include "CamManager.hpp"

class AppRuntime {
public:
    static AppRuntime& getInstance() {
        static AppRuntime instance;  // 魔法在这里！
        return instance;
    }
    CamManager& getCamManager() noexcept;

private:
    AppRuntime() = default;          // 构造函数私有，禁止外部构造
    ~AppRuntime() = default;
    AppRuntime(const AppRuntime&) = delete;            // 禁止拷贝
    AppRuntime& operator=(const AppRuntime&) = delete; // 禁止赋值
    CamManager m_camManager {};
};
