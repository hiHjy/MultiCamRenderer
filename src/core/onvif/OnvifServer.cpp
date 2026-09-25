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

constexpr const char kDeviceServicePath[] = "/onvif/device_service";
constexpr const char kMediaServicePath[] = "/onvif/media_service";

bool copyText(char* destination, std::size_t capacity, const std::string& text)
{
    if (text.empty() || text.size() >= capacity)
        return false;
    std::memcpy(destination, text.c_str(), text.size() + 1);
    return true;
}

// Scope 是 URI，不能把型号中的空格直接拼进去，否则 C 适配层会把它当作 scope 分隔符。
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

OnvifServer::~OnvifServer()
{
    stop();
}

bool OnvifServer::start(const OnvifServerConfig& config)
{
    stop();

    if (config.deviceServicePort == 0 || config.mainRtspUrl.empty() || config.subRtspUrl.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setLastErrorLocked("ONVIF 需要有效的 HTTP 端口以及 main/sub RTSP URL");
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

void OnvifServer::stop()
{
    m_stopRequested.store(true);
    m_protocolStopRequested.store(true);
    if (m_networkMonitorThread.joinable())
        m_networkMonitorThread.join();

    stopProtocolServices();
}

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

bool OnvifServer::isRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

std::string OnvifServer::lastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

std::string OnvifServer::deviceServiceUrl() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_soapConfig.deviceServiceUrl;
}

int OnvifServer::stopRequested(void* context)
{
    const auto* server = static_cast<const OnvifServer*>(context);
    return server->m_stopRequested.load() || server->m_protocolStopRequested.load() ? 1 : 0;
}

void OnvifServer::reportDeviceServiceStarted(void* context, int success, const char* errorText)
{
    auto* server = static_cast<OnvifServer*>(context);
    server->reportStart(server->m_deviceServiceStart, success, errorText);
}

void OnvifServer::reportDiscoveryStarted(void* context, int success, const char* errorText)
{
    auto* server = static_cast<OnvifServer*>(context);
    server->reportStart(server->m_discoveryStart, success, errorText);
}

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

std::string OnvifServer::macAddressText(const std::array<unsigned char, 6>& macAddress)
{
    char text[18] {};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  macAddress[0], macAddress[1], macAddress[2],
                  macAddress[3], macAddress[4], macAddress[5]);
    return text;
}

std::string OnvifServer::endpointUuid(const std::array<unsigned char, 6>& macAddress)
{
    char uuid[64] {};
    // 最后 48 bit 固定来自 MAC，故 DHCP 换 IP 后客户端仍将它识别为同一台设备。
    std::snprintf(uuid, sizeof(uuid), "urn:uuid:4d435200-0000-5000-8000-%02x%02x%02x%02x%02x%02x",
                  macAddress[0], macAddress[1], macAddress[2],
                  macAddress[3], macAddress[4], macAddress[5]);
    return uuid;
}

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
    const std::string serialNumber = "MCR-" + macAddressText(identity.macAddress);
    const std::string uuid = endpointUuid(identity.macAddress);
    const std::string scopes =
        "onvif://www.onvif.org/type/Network_Video_Transmitter "
        "onvif://www.onvif.org/name/" + encodeScopeSegment(config.model) + " "
        "onvif://www.onvif.org/hardware/" + encodeScopeSegment(config.hardwareId);

    OnvifSoapServiceConfig soapConfig {};
    if (!copyText(soapConfig.deviceServiceUrl, sizeof(soapConfig.deviceServiceUrl), deviceUrl.str()) ||
        !copyText(soapConfig.mediaServiceUrl, sizeof(soapConfig.mediaServiceUrl), mediaUrl.str()) ||
        !copyText(soapConfig.mainRtspUrl, sizeof(soapConfig.mainRtspUrl), config.mainRtspUrl) ||
        !copyText(soapConfig.subRtspUrl, sizeof(soapConfig.subRtspUrl), config.subRtspUrl) ||
        !copyText(soapConfig.endpointReference, sizeof(soapConfig.endpointReference), uuid) ||
        !copyText(soapConfig.manufacturer, sizeof(soapConfig.manufacturer), config.manufacturer) ||
        !copyText(soapConfig.model, sizeof(soapConfig.model), config.model) ||
        !copyText(soapConfig.firmwareVersion, sizeof(soapConfig.firmwareVersion), config.firmwareVersion) ||
        !copyText(soapConfig.serialNumber, sizeof(soapConfig.serialNumber), serialNumber) ||
        !copyText(soapConfig.hardwareId, sizeof(soapConfig.hardwareId), config.hardwareId) ||
        !copyText(soapConfig.scopes, sizeof(soapConfig.scopes), scopes)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setLastErrorLocked("ONVIF 配置为空或超过固定协议字段长度");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_soapConfig = soapConfig;
    m_deviceServicePort = config.deviceServicePort;
    return true;
}

bool OnvifServer::startDeviceService()
{
    m_deviceServiceThread = std::thread([this] {
        onvif_soap_run_device_service(&m_soapConfig, m_deviceServicePort,
                                      &OnvifServer::stopRequested, this,
                                      &OnvifServer::reportDeviceServiceStarted, this);
    });
    return waitForStart(m_deviceServiceStart, "ONVIF Device Service");
}

bool OnvifServer::startDiscoveryService()
{
    m_discoveryThread = std::thread([this] {
        onvif_soap_run_discovery_service(&m_soapConfig, &OnvifServer::stopRequested, this,
                                         &OnvifServer::reportDiscoveryStarted, this);
    });
    return waitForStart(m_discoveryStart, "WS-Discovery");
}

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

void OnvifServer::reportStart(ServiceStartResult& result, int success, const char* errorText)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    result = success != 0 ? ServiceStartResult::Succeeded : ServiceStartResult::Failed;
    if (success == 0)
        setLastErrorLocked(errorText != nullptr && *errorText != '\0' ? errorText : "ONVIF 底层服务启动失败");
    m_startCondition.notify_all();
}

void OnvifServer::setLastErrorLocked(std::string error)
{
    m_lastError = std::move(error);
}
