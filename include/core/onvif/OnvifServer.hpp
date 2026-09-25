#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "OnvifSoapService.h"

/*
 * 应用层唯一需要接触的 ONVIF 对象。
 *
 * start() 会启动网络管理线程；它自行等待可用 IPv4，并把两条协议链路作为一个整体启动：
 *   1. HTTP/SOAP Device Service，地址由 deviceServiceUrl() 返回；
 *   2. WS-Discovery UDP 响应端，固定监听 239.255.255.250:3702。
 *
 * gSOAP 的生成函数受其 C 全局符号约束，留在私有的 OnvifSoapService.c。
 * 此类只管理 C++ 应用真正关心的线程、生命周期、配置和错误状态。
 */
struct OnvifServerConfig {
    std::uint16_t deviceServicePort = 8899;
    // 与 ProfileToken "main" / "sub" 一一对应的真实 live555 URL。
    std::string mainRtspUrl;
    std::string subRtspUrl;
    std::string manufacturer = "MultiCamRenderer";
    std::string model = "RV1126B IPC";
    std::string firmwareVersion = "development";
    std::string hardwareId = "RV1126B";
};

class OnvifServer {
public:
    OnvifServer() = default;
    ~OnvifServer();

    OnvifServer(const OnvifServer&) = delete;
    OnvifServer& operator=(const OnvifServer&) = delete;

    // 仅当配置非法时返回 false；网络尚未就绪时后台会每 5 秒自动重试。
    bool start(const OnvifServerConfig& config);
    void stop();

    bool isRunning() const;
    std::string lastError() const;
    std::string deviceServiceUrl() const;

private:
    struct NetworkIdentity {
        std::string ipv4Address;
        std::array<unsigned char, 6> macAddress {};
    };

    enum class ServiceStartResult {
        Pending,
        Succeeded,
        Failed,
    };

    static int stopRequested(void* context);
    static void reportDeviceServiceStarted(void* context, int success, const char* errorText);
    static void reportDiscoveryStarted(void* context, int success, const char* errorText);

    static bool getPrimaryNetworkIdentity(NetworkIdentity& identity);
    static std::string macAddressText(const std::array<unsigned char, 6>& macAddress);
    static std::string endpointUuid(const std::array<unsigned char, 6>& macAddress);

    void networkMonitorMain();
    void refreshNetworkService();
    bool buildSoapConfig(const OnvifServerConfig& config, const NetworkIdentity& identity);
    void stopProtocolServices();
    bool startDeviceService();
    bool startDiscoveryService();
    bool waitForStart(ServiceStartResult& result, const char* serviceName);
    void reportStart(ServiceStartResult& result, int success, const char* errorText);
    void setLastErrorLocked(std::string error);

    mutable std::mutex m_mutex;
    std::condition_variable m_startCondition;
    std::atomic_bool m_stopRequested {false};
    std::atomic_bool m_protocolStopRequested {false};
    bool m_running = false;
    std::string m_lastError;
    OnvifServerConfig m_config;
    NetworkIdentity m_activeIdentity;
    bool m_hasActiveIdentity = false;
    std::uint16_t m_deviceServicePort = 0;
    OnvifSoapServiceConfig m_soapConfig {};
    ServiceStartResult m_deviceServiceStart = ServiceStartResult::Pending;
    ServiceStartResult m_discoveryStart = ServiceStartResult::Pending;
    std::thread m_deviceServiceThread;
    std::thread m_discoveryThread;
    std::thread m_networkMonitorThread;
};
