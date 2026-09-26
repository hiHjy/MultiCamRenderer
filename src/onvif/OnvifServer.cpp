/**
 * @file OnvifServer.cpp
 * @brief ONVIF 服务端核心实现文件。
 *
 * 负责管理网络监控、IP 变化检测，以及生命周期管理（包括 SOAP 设备服务和 WS-Discovery 发现服务的启动、停止与重启）。
 * 该实现将网络身份检测逻辑与协议服务封装在一起，为上层提供统一的接口。
 */
#include "OnvifServer.hpp"


#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <iomanip>
#include <net/if.h>
#include <netpacket/packet.h>
#include <sstream>

namespace {

// ONVIF 设备服务和媒体服务的默认 URL 路径常量
constexpr const char kDeviceServicePath[] = "/onvif/device_service";
constexpr const char kMediaServicePath[] = "/onvif/media_service";
constexpr const char kMedia2ServicePath[] = "/onvif/media2_service";

/**
 * @brief 安全地将 std::string 复制到 C 风格字符串数组中。
 *
 * @param destination 目标字符数组指针
 * @param capacity 目标数组的容量大小
 * @param text 要复制的源字符串
 * @return 复制成功返回 true；如果源字符串为空或超出目标容量则返回 false。
 */
bool copyText(char* destination, std::size_t capacity, const std::string& text)
{
    if (text.empty() || text.size() >= capacity)
        return false;
    std::memcpy(destination, text.c_str(), text.size() + 1);
    return true;
}

// Scope 是 URI，不能把型号中的空格直接拼进去，否则 C 适配层会把它当作 scope 分隔符。
/**
 * @brief 对作用域（Scope）URI 中的特殊字符进行 URL 编码。
 *
 * ONVIF 发现协议中的 Scope 使用 URI 格式，遇到空格等特殊字符需要转义，
 * 以免底层 C 代码在解析时将其误认为不同 Scope 的分隔符。
 *
 * @param text 需要编码的原始字符串
 * @return 编码后的字符串
 */
std::string encodeScopeSegment(const std::string& text)
{
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char character : text) {
        const bool isUnreserved = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9') ||
                                  character == '-' || character == '_' ||
                                  character == '.' || character == '~';
        if (isUnreserved) {
            encoded << character;
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned int>(character)
                    << std::setfill(' ');
        }
    }
    return encoded.str();
}

} // namespace

/**
 * @brief 析构函数，在对象销毁前停止所有服务并清理线程。
 */
OnvifServer::~OnvifServer()
{
    stop();
}

/**
 * @brief 启动 ONVIF 服务端。
 *
 * 校验配置信息是否有效，尝试同步获取本机网络身份并拉起底层服务。
 * 如果一次性拉起失败或网络尚未就绪，则会启动后台网络监控线程负责重试。
 *
 * @param config ONVIF 服务端配置结构，包含监听端口和 RTSP 地址等必需项。
 * @return 只要配置无明显错误，就会返回 true（即使因为网络未就绪导致实际服务挂起等待）。
 */
bool OnvifServer::start(const OnvifServerConfig& config)
{
    stop();
    if (config.username.empty() != config.password.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setLastErrorLocked("ONVIF 认证必须同时提供用户名和密码，或两者均留空关闭认证");
        return false;
    }
    if (!config.username.empty() && config.authenticationRealm.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setLastErrorLocked("启用 ONVIF 认证时 authenticationRealm 不能为空");
        return false;
    }

    if (config.deviceServicePort == 0 || config.rtspPort == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setLastErrorLocked("ONVIF 需要有效的 HTTP 与 RTSP 端口");
        return false;
    }

    m_stopRequested.store(false);
    m_protocolStopRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
        m_lastError.clear();
        m_hasActiveIdentity = false;
    }

    // 先同步尝试一次，网络已就绪时 App 启动日志会立刻给出 ONVIF URL；未就绪则由
    // 后台线程持续等待。网络身份的查找与重启策略完全收在 OnvifServer 内部。
    refreshNetworkService();
    m_networkMonitorThread = std::thread(&OnvifServer::networkMonitorMain, this);
    return true;
}

/**
 * @brief 停止所有 ONVIF 相关服务及网络监控线程。
 */
void OnvifServer::stop()
{
    m_stopRequested.store(true);
    m_protocolStopRequested.store(true);
    if (m_networkMonitorThread.joinable())
        m_networkMonitorThread.join();

    stopProtocolServices();
}

/**
 * @brief 仅停止底层 SOAP 设备服务和 WS-Discovery 发现服务。
 *
 * 在 IP 发生变化需要重启底层服务，或者完全关闭服务器时被调用。
 */
void OnvifServer::stopProtocolServices()
{
    m_protocolStopRequested.store(true);
    if (m_deviceServiceThread.joinable())
        m_deviceServiceThread.join();
    if (m_discoveryThread.joinable())
        m_discoveryThread.join();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_running = false;
    m_hasActiveIdentity = false;
}

/**
 * @brief 查询底层服务是否正在运行（网络正常且成功拉起）。
 *
 * @return 若正在运行返回 true，否则返回 false。
 */
bool OnvifServer::isRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

/**
 * @brief 获取最后一次记录的错误信息。
 *
 * @return 错误信息字符串。
 */
std::string OnvifServer::lastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

/**
 * @brief 获取当前的设备服务 URL。
 *
 * @return 设备服务 URL（例如：http://192.168.1.100:8000/onvif/device_service）。
 */
std::string OnvifServer::deviceServiceUrl() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_soapConfig.deviceServiceUrl;
}

/**
 * @brief 供底层 C 代码调用的回调函数，检查是否收到停止请求。
 *
 * @param context OnvifServer 实例指针。
 * @return 1 表示请求停止，0 表示继续运行。
 */
int OnvifServer::stopRequested(void* context)
{
    const auto* server = static_cast<const OnvifServer*>(context);
    return server->m_stopRequested.load() || server->m_protocolStopRequested.load() ? 1 : 0;
}

/**
 * @brief 供底层 C 代码调用的回调函数，报告设备服务启动结果。
 *
 * @param context OnvifServer 实例指针。
 * @param success 1 表示成功，0 表示失败。
 * @param errorText 失败时的错误信息。
 */
void OnvifServer::reportDeviceServiceStarted(void* context, int success, const char* errorText)
{
    auto* server = static_cast<OnvifServer*>(context);
    server->reportStart(server->m_deviceServiceStart, success, errorText);
}

/**
 * @brief 供底层 C 代码调用的回调函数，报告发现服务启动结果。
 *
 * @param context OnvifServer 实例指针。
 * @param success 1 表示成功，0 表示失败。
 * @param errorText 失败时的错误信息。
 */
void OnvifServer::reportDiscoveryStarted(void* context, int success, const char* errorText)
{
    auto* server = static_cast<OnvifServer*>(context);
    server->reportStart(server->m_discoveryStart, success, errorText);
}

/**
 * @brief 获取本机第一个非 loopback 的可用 IPv4 地址及其对应的 MAC 地址。
 *
 * 遍历所有网络接口，找到处于 UP 状态且非环回的网卡，记录其 IP 地址；
 * 随后再次遍历获取该网卡名称匹配的物理层 MAC 地址。
 *
 * @param identity 用于保存获取到的网络身份（IP 和 MAC 地址）的对象。
 * @return 获取成功返回 true，否则返回 false。
 */
bool OnvifServer::getPrimaryNetworkIdentity(NetworkIdentity& identity)
{
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0)
        return false;

    std::string interfaceName;
    for (const ifaddrs* item = addresses; item != nullptr; item = item->ifa_next) {
        if (item->ifa_name == nullptr || item->ifa_addr == nullptr ||
            item->ifa_addr->sa_family != AF_INET || (item->ifa_flags & IFF_UP) == 0 ||
            (item->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char addressText[INET_ADDRSTRLEN] {};
        const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, addressText, sizeof(addressText)) == nullptr)
            continue;
        interfaceName = item->ifa_name;
        identity.ipv4Address = addressText;
        break;
    }

    bool macFound = false;
    if (!interfaceName.empty()) {
        for (const ifaddrs* item = addresses; item != nullptr; item = item->ifa_next) {
            if (item->ifa_name == nullptr || item->ifa_addr == nullptr ||
                item->ifa_addr->sa_family != AF_PACKET || interfaceName != item->ifa_name) {
                continue;
            }
            const auto* linkAddress = reinterpret_cast<const sockaddr_ll*>(item->ifa_addr);
            if (linkAddress->sll_halen == identity.macAddress.size()) {
                std::memcpy(identity.macAddress.data(), linkAddress->sll_addr, identity.macAddress.size());
                macFound = true;
                break;
            }
        }
    }
    freeifaddrs(addresses);
    return !identity.ipv4Address.empty() && macFound;
}

/**
 * @brief 将 MAC 地址格式化为大写的十六进制字符串形式。
 *
 * @param macAddress 包含 6 字节 MAC 地址的数组。
 * @return 格式化后的字符串，如 "00:1A:2B:3C:4D:5E"。
 */
std::string OnvifServer::macAddressText(const std::array<unsigned char, 6>& macAddress)
{
    char text[18] {};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  macAddress[0], macAddress[1], macAddress[2],
                  macAddress[3], macAddress[4], macAddress[5]);
    return text;
}

/**
 * @brief 根据 MAC 地址生成唯一的设备 UUID 字符串。
 *
 * UUID 的最后 48 位取自 MAC 地址。这样即使设备因为 DHCP 获取到了新 IP，
 * 客户端依旧可以通过 UUID 认出这是同一台设备。
 *
 * @param macAddress 包含 6 字节 MAC 地址的数组。
 * @return URN 格式的 UUID 字符串，例如 "urn:uuid:4d435200-0000-5000-8000-001a2b3c4d5e"。
 */
std::string OnvifServer::endpointUuid(const std::array<unsigned char, 6>& macAddress)
{
    char uuid[64] {};
    // 最后 48 bit 固定来自 MAC，故 DHCP 换 IP 后客户端仍将它识别为同一台设备。
    std::snprintf(uuid, sizeof(uuid), "urn:uuid:4d435200-0000-5000-8000-%02x%02x%02x%02x%02x%02x",
                  macAddress[0], macAddress[1], macAddress[2],
                  macAddress[3], macAddress[4], macAddress[5]);
    return uuid;
}

/**
 * @brief 网络监控线程的主循环函数。
 *
 * 周期性地唤醒并调用 refreshNetworkService() 以检查网络连接和 IP 是否发生变化。
 * 每隔较短时间检查一次停止标志，以便在收到停止请求时快速退出。
 */
void OnvifServer::networkMonitorMain()
{
    while (!m_stopRequested.load()) {
        // stop() 最多等待一个很短的 sleep slice，不会因 5 秒重试间隔而卡住退出。
        for (int retrySlice = 0; retrySlice < 50 && !m_stopRequested.load(); ++retrySlice)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!m_stopRequested.load())
            refreshNetworkService();
    }
}

/**
 * @brief 刷新并检查网络服务状态。
 *
 * 负责检测当前 IP 是否发生变化。如果发生变化（或者之前未就绪现在就绪了），
 * 则停止旧的服务并以新的网络身份重新生成配置、启动设备服务和发现服务。
 */
void OnvifServer::refreshNetworkService()
{
    NetworkIdentity currentIdentity;
    if (!getPrimaryNetworkIdentity(currentIdentity)) {
        if (isRunning())
            stopProtocolServices();
        std::lock_guard<std::mutex> lock(m_mutex);
        setLastErrorLocked("等待可用的非 loopback IPv4 与 MAC 地址");
        return;
    }

    OnvifServerConfig config;
    bool alreadyRunning = false;
    bool sameIdentity = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        config = m_config;
        alreadyRunning = m_running;
        sameIdentity = m_hasActiveIdentity && currentIdentity.ipv4Address == m_activeIdentity.ipv4Address &&
                       currentIdentity.macAddress == m_activeIdentity.macAddress;
    }
    if (alreadyRunning && sameIdentity)
        return;

    // IP 变化时先完整停止旧 socket/组播响应端，再以新 XAddr 起新的两条协议服务。
    if (alreadyRunning)
        stopProtocolServices();

    if (!buildSoapConfig(config, currentIdentity))
        return;

    m_protocolStopRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError.clear();
        m_deviceServiceStart = ServiceStartResult::Pending;
        m_discoveryStart = ServiceStartResult::Pending;
    }
    if (!startDeviceService() || !startDiscoveryService()) {
        stopProtocolServices();
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_activeIdentity = currentIdentity;
    m_hasActiveIdentity = true;
    m_running = true;
}

/**
 * @brief 根据服务端配置和当前网络身份，构建传递给 C 层 SOAP 服务的配置结构。
 *
 * 将所有需要的参数（如各种服务的 URL、RTSP URL、UUID 以及各类设备信息等）
 * 拷贝至固定长度字符数组中。
 *
 * @param config 用户提供的 ONVIF 配置（包含型号、固件版本等）。
 * @param identity 当前使用的网络身份（包含 IP 和 MAC）。
 * @return 构建成功返回 true；如果内容过长或 IP 无效则返回 false。
 */
bool OnvifServer::buildSoapConfig(const OnvifServerConfig& config, const NetworkIdentity& identity)
{
    if (identity.ipv4Address.empty() || config.deviceServicePort == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setLastErrorLocked("ONVIF 需要有效的本机 IP 地址和 HTTP 端口");
        return false;
    }

    std::ostringstream deviceUrl;
    deviceUrl << "http://" << identity.ipv4Address << ':' << config.deviceServicePort << kDeviceServicePath;
    std::ostringstream mediaUrl;
    mediaUrl << "http://" << identity.ipv4Address << ':' << config.deviceServicePort << kMediaServicePath;
    std::ostringstream media2Url;
    media2Url << "http://" << identity.ipv4Address << ':' << config.deviceServicePort << kMedia2ServicePath;
    // 与本轮 WSDD / Device Service 使用同一份 IPv4。网络换址后 refreshNetworkService()
    // 会重新执行此函数，因而 Media.GetStreamUri 永远不会返回旧地址。
    std::ostringstream mainRtspUrl;
    mainRtspUrl << "rtsp://" << identity.ipv4Address << ':' << config.rtspPort << "/main";
    std::ostringstream subRtspUrl;
    subRtspUrl << "rtsp://" << identity.ipv4Address << ':' << config.rtspPort << "/sub";
    const std::string serialNumber = "MCR-" + macAddressText(identity.macAddress);
    const std::string uuid = endpointUuid(identity.macAddress);
    const std::string scopes =
        "onvif://www.onvif.org/type/Network_Video_Transmitter "
        "onvif://www.onvif.org/name/" + encodeScopeSegment(config.model) + " "
        "onvif://www.onvif.org/hardware/" + encodeScopeSegment(config.hardwareId);

    OnvifSoapServiceConfig soapConfig {};
    if (!copyText(soapConfig.deviceServiceUrl, sizeof(soapConfig.deviceServiceUrl), deviceUrl.str()) ||
        !copyText(soapConfig.mediaServiceUrl, sizeof(soapConfig.mediaServiceUrl), mediaUrl.str()) ||
        !copyText(soapConfig.media2ServiceUrl, sizeof(soapConfig.media2ServiceUrl), media2Url.str()) ||
        !copyText(soapConfig.mainRtspUrl, sizeof(soapConfig.mainRtspUrl), mainRtspUrl.str()) ||
        !copyText(soapConfig.subRtspUrl, sizeof(soapConfig.subRtspUrl), subRtspUrl.str()) ||
        !copyText(soapConfig.endpointReference, sizeof(soapConfig.endpointReference), uuid) ||
        !copyText(soapConfig.manufacturer, sizeof(soapConfig.manufacturer), config.manufacturer) ||
        !copyText(soapConfig.model, sizeof(soapConfig.model), config.model) ||
        !copyText(soapConfig.firmwareVersion, sizeof(soapConfig.firmwareVersion), config.firmwareVersion) ||
        !copyText(soapConfig.serialNumber, sizeof(soapConfig.serialNumber), serialNumber) ||
        !copyText(soapConfig.hardwareId, sizeof(soapConfig.hardwareId), config.hardwareId) ||
        !copyText(soapConfig.scopes, sizeof(soapConfig.scopes), scopes) ||
        (!config.username.empty() &&
         (!copyText(soapConfig.username, sizeof(soapConfig.username), config.username) ||
          !copyText(soapConfig.password, sizeof(soapConfig.password), config.password) ||
          !copyText(soapConfig.authenticationRealm, sizeof(soapConfig.authenticationRealm),
                    config.authenticationRealm)))) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setLastErrorLocked("ONVIF 配置为空或超过固定协议字段长度");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_soapConfig = soapConfig;
    m_deviceServicePort = config.deviceServicePort;
    return true;
}

/**
 * @brief 启动底层 SOAP 设备服务线程。
 *
 * @return 启动成功返回 true，否则记录错误并返回 false。
 */
bool OnvifServer::startDeviceService()
{
    m_deviceServiceThread = std::thread([this] {
        onvif_soap_run_device_service(&m_soapConfig, m_deviceServicePort,
                                      &OnvifServer::stopRequested, this,
                                      &OnvifServer::reportDeviceServiceStarted, this);
    });
    return waitForStart(m_deviceServiceStart, "ONVIF Device Service");
}

/**
 * @brief 启动底层 WS-Discovery 发现服务线程。
 *
 * @return 启动成功返回 true，否则记录错误并返回 false。
 */
bool OnvifServer::startDiscoveryService()
{
    m_discoveryThread = std::thread([this] {
        onvif_soap_run_discovery_service(&m_soapConfig, &OnvifServer::stopRequested, this,
                                         &OnvifServer::reportDiscoveryStarted, this);
    });
    return waitForStart(m_discoveryStart, "WS-Discovery");
}

/**
 * @brief 阻塞等待后台服务启动结果。
 *
 * @param result 引用的启动结果状态。
 * @param serviceName 用于生成错误信息的服务名称。
 * @return 成功返回 true，否则返回 false。
 */
bool OnvifServer::waitForStart(ServiceStartResult& result, const char* serviceName)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_startCondition.wait(lock, [&result] { return result != ServiceStartResult::Pending; });
    if (result == ServiceStartResult::Succeeded)
        return true;
    if (m_lastError.empty())
        setLastErrorLocked(std::string(serviceName) + " 启动失败");
    return false;
}

/**
 * @brief 接收服务启动结果的内部调用，负责更新状态并唤醒等待线程。
 *
 * @param result 要更新的启动结果状态变量。
 * @param success 是否成功。
 * @param errorText 失败时的错误信息。
 */
void OnvifServer::reportStart(ServiceStartResult& result, int success, const char* errorText)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    result = success != 0 ? ServiceStartResult::Succeeded : ServiceStartResult::Failed;
    if (success == 0)
        setLastErrorLocked(errorText != nullptr && *errorText != '\0' ? errorText : "ONVIF 底层服务启动失败");
    m_startCondition.notify_all();
}

/**
 * @brief 在已加锁的状态下设置最后一次错误信息。
 *
 * @param error 错误信息字符串。
 */
void OnvifServer::setLastErrorLocked(std::string error)
{
    m_lastError = std::move(error);
}
