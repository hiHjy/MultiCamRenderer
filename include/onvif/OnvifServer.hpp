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
/* ONVIF 服务端的配置选项 */
struct OnvifServerConfig {
    std::uint16_t deviceServicePort = 8899;        /* 设备服务要监听的 HTTP 端口 */
    /*
     * ONVIF 在每次网络身份刷新时，用同一次获得的 IPv4 动态拼出 main/sub 的 RTSP
     * URL。不能由 App 预先传入带 IP 的字符串：DHCP 换址或冷启动未联网时会变陈旧。
     */
    std::uint16_t rtspPort = 8554;                 /* live555 RTSP 服务监听端口 */
    std::string manufacturer = "MultiCamRenderer"; /* 设备制造商名称 */
    std::string model = "RV1126B IPC";             /* 设备型号 */
    std::string firmwareVersion = "development";   /* 固件版本 */
    std::string hardwareId = "RV1126B";            /* 硬件标识符 */

    // 留空表示当前设备明确以匿名模式运行。启用时两个字段必须同时非空，并与
    // Live555RtspServer::start() 传入的同一份账号保持一致。
    std::string username;                           /* ONVIF/RTSP 共用的管理员用户名 */
    std::string password;                           /* ONVIF/RTSP 共用的管理员密码，不写日志 */
    std::string authenticationRealm = "MultiCamRenderer ONVIF"; /* HTTP Digest realm */
};

/*
 * 管理 ONVIF Device Service 和 WS-Discovery 生命周期的 C++ 服务端类。
 */
class OnvifServer {
public:
    OnvifServer() = default;
    ~OnvifServer();

    OnvifServer(const OnvifServer&) = delete;
    OnvifServer& operator=(const OnvifServer&) = delete;

    // 仅当配置非法时返回 false；网络尚未就绪时后台会每 5 秒自动重试。
    /*
     * 启动 ONVIF 相关的各项服务和网络监听。
     * @param config 启动所需配置
     * @return 成功开启后台管理线程返回 true；如果传入配置存在明显错误则返回 false
     */
    bool start(const OnvifServerConfig& config);

    /* 停止所有相关服务并清理资源 */
    void stop();

    /* 返回服务是否处于运行状态 */
    bool isRunning() const;

    /* 获取上一次出现的错误信息 */
    std::string lastError() const;

    /* 获取当前正在提供服务的 Device Service URL */
    std::string deviceServiceUrl() const;

private:
    /* 表示当前设备的网络身份信息（IP 与 MAC） */
    struct NetworkIdentity {
        std::string ipv4Address;                    /* IPv4 地址的字符串表示 */
        std::array<unsigned char, 6> macAddress {}; /* 6 字节 MAC 地址 */
    };

    /* 服务启动的状态枚举 */
    enum class ServiceStartResult {
        Pending,   /* 正在等待启动结果 */
        Succeeded, /* 启动成功 */
        Failed,    /* 启动失败 */
    };

    /* 内部用于响应停止请求的回调（对应 C 接口） */
    static int stopRequested(void* context);
    /* 内部用于接收设备服务启动完成的回调 */
    static void reportDeviceServiceStarted(void* context, int success, const char* errorText);
    /* 内部用于接收发现服务启动完成的回调 */
    static void reportDiscoveryStarted(void* context, int success, const char* errorText);

    /* 获取系统主网卡的网络身份信息 */
    static bool getPrimaryNetworkIdentity(NetworkIdentity& identity);
    /* 将 MAC 地址字节数组格式化为字符串 */
    static std::string macAddressText(const std::array<unsigned char, 6>& macAddress);
    /* 通过 MAC 地址生成符合规范的 UUID 端点标识 */
    static std::string endpointUuid(const std::array<unsigned char, 6>& macAddress);

    /* 后台网络监测主循环，负责在网络变化时重启服务 */
    void networkMonitorMain();
    /* 根据最新的网络状态启动/重启具体的协议服务 */
    void refreshNetworkService();
    /* 构建 C 层需要的 SOAP 服务配置 */
    bool buildSoapConfig(const OnvifServerConfig& config, const NetworkIdentity& identity);
    /* 停止正在运行的 HTTP 和 UDP 服务监听线程 */
    void stopProtocolServices();
    /* 创建设备服务监听线程 */
    bool startDeviceService();
    /* 创建 WS-Discovery 监听线程 */
    bool startDiscoveryService();
    /* 阻塞等待某一项服务启动完成 */
    bool waitForStart(ServiceStartResult& result, const char* serviceName);
    /* 回报特定服务启动结果，并唤醒等待的线程 */
    void reportStart(ServiceStartResult& result, int success, const char* errorText);
    /* 在持有锁的状态下设置错误信息 */
    void setLastErrorLocked(std::string error);

    mutable std::mutex m_mutex;                      /* 保护内部状态的互斥锁 */
    std::condition_variable m_startCondition;        /* 用于等待服务启动状态的条件变量 */
    std::atomic_bool m_stopRequested {false};        /* 标志整个 Server 是否被要求停止 */
    std::atomic_bool m_protocolStopRequested {false};/* 标志底层协议服务线程是否被要求停止 */
    bool m_running = false;                          /* 标志网络监测线程是否在运行中 */
    std::string m_lastError;                         /* 最近一次发生的错误信息 */
    OnvifServerConfig m_config;                      /* 用户传入的配置备份 */
    NetworkIdentity m_activeIdentity;                /* 当前激活生效的网络身份信息 */
    bool m_hasActiveIdentity = false;                /* 是否已经成功获取到网络身份 */
    std::uint16_t m_deviceServicePort = 0;           /* 实际监听的设备服务端口 */
    OnvifSoapServiceConfig m_soapConfig {};          /* 传递给 C 层 SOAP 服务的配置块 */
    ServiceStartResult m_deviceServiceStart = ServiceStartResult::Pending; /* 设备服务的启动状态 */
    ServiceStartResult m_discoveryStart = ServiceStartResult::Pending;     /* 发现服务的启动状态 */
    std::thread m_deviceServiceThread;               /* 设备服务监听的工作线程 */
    std::thread m_discoveryThread;                   /* WS-Discovery 监听的工作线程 */
    std::thread m_networkMonitorThread;              /* 监测网络并调度服务的管理线程 */
};
