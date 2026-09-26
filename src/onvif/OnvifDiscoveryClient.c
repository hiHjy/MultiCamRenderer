/**
 * @file OnvifDiscoveryClient.c
 * @brief ONVIF 设备发现客户端的公共实现。
 *
 * 本文件封装了底层的 WS-Discovery Probe 功能，
 * 并提供了去重后的设备列表。
 */
#include "OnvifDiscoveryClient.h"
#include "OnvifWsddClient.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 安全地复制字符串到目标缓冲区，确保以 null 结尾。
 *
 * @param destination 目标缓冲区的指针。
 * @param capacity 目标缓冲区的容量大小。
 * @param source 源字符串指针。如果为 NULL，则目标缓冲区将被设置为空字符串。
 */
static void copy_text(char* destination, size_t capacity, const char* source)
{
    size_t length;
    if (destination == NULL || capacity == 0)
        return;
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    length = strlen(source);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

/**
 * @brief 判断新发现的设备是否已经在结果列表中存在。
 *
 * 该函数通过 endpointReference 或 deviceServiceUrl 来判断是否为同一设备（去重逻辑）。
 *
 * @param result 已保存的去重设备列表结果。
 * @param device 待检查的新设备。
 * @return int 如果设备已存在返回 1，否则返回 0。
 */
static int is_duplicate(const OnvifDiscoveredDevices* result, const OnvifDiscoveredDevice* device)
{
    unsigned int index;
    for (index = 0; index < result->count; ++index) {
        const OnvifDiscoveredDevice* existing = &result->devices[index];
        // 按 endpointReference 检查是否重复
        if (device->endpointReference[0] != '\0' && existing->endpointReference[0] != '\0' &&
            strcmp(device->endpointReference, existing->endpointReference) == 0) {
            return 1;
        }
        // 按 deviceServiceUrl 检查是否重复
        if (device->deviceServiceUrl[0] != '\0' && existing->deviceServiceUrl[0] != '\0' &&
            strcmp(device->deviceServiceUrl, existing->deviceServiceUrl) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 发现同一网络内的 ONVIF 设备。
 *
 * 完整流程：参数校验、超时范围限制、调用底层 WS-Discovery probe、去重并拷贝结果。
 *
 * @param timeoutMs 探测的超时时间（毫秒）。
 * @param result 用于存储发现的设备列表（去重后的结果）。
 * @return int 成功返回 0，失败返回 -1（错误信息记录在 result->error 中）。
 */
int onvif_discover_devices(unsigned int timeoutMs, OnvifDiscoveredDevices* result)
{
    OnvifWsddProbeResult probeResult;
    unsigned int index;
    // 参数校验
    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));

    // 超时范围限制：默认 3000ms，最大 30000ms
    if (timeoutMs == 0)
        timeoutMs = 3000;
    if (timeoutMs > 30000)
        timeoutMs = 30000;

    memset(&probeResult, 0, sizeof(probeResult));
    // 调用底层 probe 进行设备发现
    if (onvif_wsdd_probe(timeoutMs, &probeResult) != 0) {
        copy_text(result->error, sizeof(result->error), probeResult.error);
        return -1;
    }

    // 遍历底层探针的返回结果，进行去重并拷贝
    for (index = 0; index < probeResult.count && result->count < ONVIF_DISCOVERY_MAX_DEVICES; ++index) {
        const OnvifWsddProbeMatch* match = &probeResult.matches[index];
        OnvifDiscoveredDevice device;
        memset(&device, 0, sizeof(device));
        copy_text(device.endpointReference, sizeof(device.endpointReference), match->endpointReference);
        copy_text(device.deviceServiceUrl, sizeof(device.deviceServiceUrl), match->deviceServiceUrl);
        copy_text(device.types, sizeof(device.types), match->types);
        copy_text(device.scopes, sizeof(device.scopes), match->scopes);
        // 去重逻辑判断，如果不重复则添加到结果列表中
        if (!is_duplicate(result, &device))
            result->devices[result->count++] = device;
    }
    return 0;
}
