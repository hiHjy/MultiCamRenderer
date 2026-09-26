/*
 * 此文件定义了 ONVIF 设备发现客户端的公共 C 接口。
 * 提供了基于 WS-Discovery 协议在局域网中搜索设备的功能。
 */
#pragma once

#include "OnvifDeviceClient.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 发现操作支持的最大设备数量 */
#define ONVIF_DISCOVERY_MAX_DEVICES 32

/* 一条 WS-Discovery ProbeMatch；字符串均已复制，不依赖 gSOAP 的临时内存。 */
/* 表示一个被发现的 ONVIF 设备信息 */
typedef struct OnvifDiscoveredDevice {
    char endpointReference[ONVIF_CLIENT_TEXT_CAPACITY]; /* 设备的端点引用 (如 UUID) */
    char deviceServiceUrl[ONVIF_CLIENT_TEXT_CAPACITY];  /* 设备的服务入口 URL (XAddr) */
    char types[ONVIF_CLIENT_TEXT_CAPACITY];             /* 设备声明支持的类型集 */
    char scopes[ONVIF_CLIENT_TEXT_CAPACITY];            /* 设备的范围信息 (Scopes) */
} OnvifDiscoveredDevice;

/* 表示被发现的所有 ONVIF 设备集合 */
typedef struct OnvifDiscoveredDevices {
    unsigned int count;                                         /* 发现的设备总数 */
    OnvifDiscoveredDevice devices[ONVIF_DISCOVERY_MAX_DEVICES]; /* 发现的设备数组 */
    char error[ONVIF_CLIENT_TEXT_CAPACITY];                     /* 错误信息，为空表示成功 */
} OnvifDiscoveredDevices;

/* 同步发送 WS-Discovery Probe；timeoutMs 为等待单播 ProbeMatch 的总时长。 */
/*
 * 发现局域网内的 ONVIF 设备
 * @param timeoutMs 探测操作的超时时间，单位毫秒
 * @param result 用于存储设备发现的结果集合
 * @return 成功返回 0，失败返回非 0
 */
int onvif_discover_devices(unsigned int timeoutMs, OnvifDiscoveredDevices* result);

#ifdef __cplusplus
}
#endif
