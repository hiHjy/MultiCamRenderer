/*
 * WS-Discovery 客户端的实现文件。
 * 其核心逻辑是通过 UDP 多播发送 Probe 消息（探测请求），
 * 并收集局域网内响应的 ProbeMatch 消息，从而发现符合特定类型的设备。
 */
#include "OnvifWsddClient.h"

#include "stdsoap2.h"
#include "wsddapi.h"

#include <stdio.h>
#include <string.h>

#define ONVIF_WSDD_ENDPOINT "soap.udp://239.255.255.250:3702" // WS-Discovery 默认的 UDP 多播地址和端口
#define ONVIF_WSDD_TYPES "dn:NetworkVideoTransmitter"         // 要探测的 ONVIF 设备类型（网络视频发送器）
#define ONVIF_WSDD_DEFAULT_TIMEOUT_US 3000000U                // 默认的探测等待超时时间（3秒，单位微秒）
#define ONVIF_WSDD_MAX_TIMEOUT_US 30000000U                   // 最大的探测等待超时时间（30秒，单位微秒）

/*
 * 安全地将源字符串拷贝到目标缓冲区，并保证字符串以 null 结尾。
 * @param destination 目标缓冲区
 * @param destinationSize 目标缓冲区的大小
 * @param source 源字符串（允许为 NULL）
 */
static void copy_text(char* destination, size_t destinationSize, const char* source)
{
    if (destination == NULL || destinationSize == 0)
        return;
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(destination, destinationSize, "%s", source);
}

/*
 * 从可能包含多个以空格（或制表符、换行符）分隔的 XAddr 字符串中提取第一个。
 * 设备有时会返回多个地址，客户端通常只需要第一个可用地址。
 * @param destination 目标缓冲区，用于存放提取到的第一个地址
 * @param destinationSize 目标缓冲区的大小
 * @param xaddrs 原始的 XAddr 字符串
 */
static void copy_first_xaddr(char* destination, size_t destinationSize, const char* xaddrs)
{
    const char* begin = xaddrs;
    const char* end;
    size_t length;
    if (destination == NULL || destinationSize == 0)
        return;
    while (begin != NULL && (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n'))
        ++begin;
    if (begin == NULL || *begin == '\0') {
        destination[0] = '\0';
        return;
    }
    end = begin;
    while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
        ++end;
    length = (size_t)(end - begin);
    if (length >= destinationSize)
        length = destinationSize - 1;
    memcpy(destination, begin, length);
    destination[length] = '\0';
}

/*
 * 格式化并记录错误信息，将 gSOAP 的错误码写入到结果结构中。
 * @param result 保存整体结果和错误的结构体
 * @param operation 发生错误的具体操作描述
 * @param soap gSOAP 上下文（可提取错误码，允许为 NULL）
 */
static void set_error(OnvifWsddProbeResult* result, const char* operation, const struct soap* soap)
{
    if (result == NULL)
        return;
    (void)snprintf(result->error, sizeof(result->error), "%s: soap=%d errno=%d", operation,
                   soap != NULL ? soap->error : -1, soap != NULL ? soap->errnum : 0);
}

/* ProbeMatch 允许 UDP 重传；必须在固定槽位耗尽前去重，不能等到上层再处理。
 * 检查当前 candidate 设备是否已经存在于探测结果列表中。
 * 判断依据：设备的 EndpointReference（终端节点引用）或 DeviceServiceUrl（设备服务URL）是否相同。
 * @param result 已有探测结果
 * @param candidate 即将要添加的候选设备
 * @return 若为重复设备返回 1，否则返回 0
 */
static int is_duplicate_match(const OnvifWsddProbeResult* result,
                              const OnvifWsddProbeMatch* candidate)
{
    unsigned int index;
    for (index = 0; index < result->count; ++index) {
        const OnvifWsddProbeMatch* existing = &result->matches[index];
        if (candidate->endpointReference[0] != '\0' && existing->endpointReference[0] != '\0' &&
            strcmp(candidate->endpointReference, existing->endpointReference) == 0) {
            return 1;
        }
        if (candidate->deviceServiceUrl[0] != '\0' && existing->deviceServiceUrl[0] != '\0' &&
            strcmp(candidate->deviceServiceUrl, existing->deviceServiceUrl) == 0) {
            return 1;
        }
    }
    return 0;
}

/* wsddapi is a C plug-in and resolves these six global callbacks by name.
 * wsddapi 是一个 C 语言插件，它通过名称静态解析这 6 个全局回调函数。
 * 当底层处理各种 WS-Discovery 事件时会调用这些函数。
 * 必须实现所有函数，即使某些事件当前并不需要处理，以避免链接错误。
 */

// 当收到设备的 Hello 消息（设备上线）时触发。此处不处理，故为空实现。
void wsdd_event_Hello(struct soap* soap, unsigned int instanceId, const char* sequenceId,
                      unsigned int messageNumber, const char* messageId, const char* relatesTo,
                      const char* endpointReference, const char* types, const char* scopes,
                      const char* matchBy, const char* xaddrs, unsigned int metadataVersion)
{
    (void)soap; (void)instanceId; (void)sequenceId; (void)messageNumber; (void)messageId;
    (void)relatesTo; (void)endpointReference; (void)types; (void)scopes; (void)matchBy;
    (void)xaddrs; (void)metadataVersion;
}

// 当收到设备的 Bye 消息（设备下线）时触发。此处不处理，故为空实现。
void wsdd_event_Bye(struct soap* soap, unsigned int instanceId, const char* sequenceId,
                    unsigned int messageNumber, const char* messageId, const char* relatesTo,
                    const char* endpointReference, const char* types, const char* scopes,
                    const char* matchBy, const char* xaddrs, unsigned int* metadataVersion)
{
    (void)soap; (void)instanceId; (void)sequenceId; (void)messageNumber; (void)messageId;
    (void)relatesTo; (void)endpointReference; (void)types; (void)scopes; (void)matchBy;
    (void)xaddrs; (void)metadataVersion;
}

// 当收到其他客户端发送的 Probe 消息时触发（作为服务端时使用）。这里返回 ADHOC 模式继续默认处理。
soap_wsdd_mode wsdd_event_Probe(struct soap* soap, const char* messageId, const char* replyTo,
                                const char* types, const char* scopes, const char* matchBy,
                                struct wsdd__ProbeMatchesType* matches)
{
    (void)soap; (void)messageId; (void)replyTo; (void)types; (void)scopes; (void)matchBy;
    (void)matches;
    return SOAP_WSDD_ADHOC;
}

// 这是核心回调函数。当收到设备的 ProbeMatch 响应（对我们发送的 Probe 的回应）时触发。
// 此处负责提取设备信息（如终端节点、服务地址、类型等）并去重保存到探测结果列表中。
void wsdd_event_ProbeMatches(struct soap* soap, unsigned int instanceId, const char* sequenceId,
                             unsigned int messageNumber, const char* messageId,
                             const char* relatesTo, struct wsdd__ProbeMatchesType* matches)
{
    OnvifWsddProbeResult* result = soap != NULL ? (OnvifWsddProbeResult*)soap->user : NULL;
    int index;
    (void)instanceId; (void)sequenceId; (void)messageNumber; (void)messageId; (void)relatesTo;
    if (result == NULL || matches == NULL || matches->ProbeMatch == NULL)
        return;
    for (index = 0; index < matches->__sizeProbeMatch && result->count < ONVIF_WSDD_MAX_MATCHES; ++index) {
        const struct wsdd__ProbeMatchType* match = &matches->ProbeMatch[index];
        OnvifWsddProbeMatch candidate;
        memset(&candidate, 0, sizeof(candidate));
        copy_text(candidate.endpointReference, sizeof(candidate.endpointReference),
                  match->wsa5__EndpointReference.Address);
        copy_first_xaddr(candidate.deviceServiceUrl, sizeof(candidate.deviceServiceUrl), match->XAddrs);
        copy_text(candidate.types, sizeof(candidate.types), match->Types);
        copy_text(candidate.scopes, sizeof(candidate.scopes),
                  match->Scopes != NULL ? match->Scopes->__item : NULL);
        if (candidate.deviceServiceUrl[0] != '\0' && !is_duplicate_match(result, &candidate))
            result->matches[result->count++] = candidate;
    }
}

// 当收到 Resolve 消息时触发（用于解析具体的终端节点地址）。此处不处理。
soap_wsdd_mode wsdd_event_Resolve(struct soap* soap, const char* messageId, const char* replyTo,
                                  const char* endpointReference, struct wsdd__ResolveMatchType* match)
{
    (void)soap; (void)messageId; (void)replyTo; (void)endpointReference; (void)match;
    return SOAP_WSDD_ADHOC;
}

// 当收到 ResolveMatches 响应时触发。此处不处理。
void wsdd_event_ResolveMatches(struct soap* soap, unsigned int instanceId, const char* sequenceId,
                               unsigned int messageNumber, const char* messageId,
                               const char* relatesTo, struct wsdd__ResolveMatchType* match)
{
    (void)soap; (void)instanceId; (void)sequenceId; (void)messageNumber; (void)messageId;
    (void)relatesTo; (void)match;
}

/*
 * 完整的 Probe 探测流程函数。
 * 流程：
 * 1. 初始化 gSOAP 运行环境。
 * 2. 注册 WS-Addressing 插件。
 * 3. 绑定一个本地的临时 UDP 端口用于接收响应。
 * 4. 发送 WS-Discovery Probe 多播消息（指定类型为 NetworkVideoTransmitter）。
 * 5. 阻塞等待超时，期间不断接收响应并触发 wsdd_event_ProbeMatches 回调收集结果。
 * 6. 释放并清理 gSOAP 运行环境。
 *
 * @param timeoutMs 探测等待超时时间（毫秒）
 * @param result 保存探测结果的指针
 * @return 探测成功（包括未发现设备的正常超时）返回 0，出现内部错误返回 -1
 */
int onvif_wsdd_probe(unsigned int timeoutMs, OnvifWsddProbeResult* result)
{
    struct soap soap;
    unsigned int timeoutUs;
    int listenResult;
    const char* messageId;
    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    timeoutUs = timeoutMs == 0 ? ONVIF_WSDD_DEFAULT_TIMEOUT_US : timeoutMs * 1000U;
    if (timeoutUs > ONVIF_WSDD_MAX_TIMEOUT_US)
        timeoutUs = ONVIF_WSDD_MAX_TIMEOUT_US;

    soap_init1(&soap, SOAP_IO_UDP | SOAP_XML_TREE);
    soap.user = result;
    if (soap_register_plugin(&soap, soap_wsa) != SOAP_OK) {
        set_error(result, "注册 WS-Addressing 插件失败", &soap);
        soap_done(&soap);
        return -1;
    }
    if (!soap_valid_socket(soap_bind(&soap, NULL, 0, 16))) {
        set_error(result, "绑定 WS-Discovery 临时 UDP 端口失败", &soap);
        soap_done(&soap);
        return -1;
    }
    messageId = soap_wsa_rand_uuid(&soap);
    if (messageId == NULL || soap_wsdd_Probe(&soap, SOAP_WSDD_ADHOC, SOAP_WSDD_TO_TS,
                                             ONVIF_WSDD_ENDPOINT, messageId, NULL,
                                             ONVIF_WSDD_TYPES, NULL, NULL) != SOAP_OK) {
        set_error(result, "发送 WS-Discovery Probe 失败", &soap);
        soap_destroy(&soap);
        soap_end(&soap);
        soap_done(&soap);
        return -1;
    }
    listenResult = soap_wsdd_listen(&soap, -(int)timeoutUs);
    if (listenResult != SOAP_OK && soap.errnum != 0) {
        set_error(result, "等待 WS-Discovery ProbeMatch 失败", &soap);
        soap_destroy(&soap);
        soap_end(&soap);
        soap_done(&soap);
        return -1;
    }
    soap_destroy(&soap);
    soap_end(&soap);
    soap_done(&soap);
    return 0;
}
