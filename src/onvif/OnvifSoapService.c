/**
 * @file OnvifSoapService.c
 * @brief ONVIF SOAP 服务端实现文件
 *
 * 实现了 gSOAP 生成代码所需的回调函数，使设备能被 ONVIF 客户端发现和查询。
 * 提供 Device, Media, Media2 服务以及 WS-Discovery 支持。
 */

#include "OnvifSoapService.h"

#include "httpda.h"
#include "stdsoap2.h"
#include "wsddapi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/** ONVIF Device 服务的 XML 命名空间 */
#define ONVIF_DEVICE_NAMESPACE "http://www.onvif.org/ver10/device/wsdl"
/** ONVIF Media v1 服务的 XML 命名空间 */
#define ONVIF_MEDIA_NAMESPACE "http://www.onvif.org/ver10/media/wsdl"
/** ONVIF Media v2 服务的 XML 命名空间 */
#define ONVIF_MEDIA2_NAMESPACE "http://www.onvif.org/ver20/media/wsdl"
/** WS-Discovery 默认的 UDP 多播端口 */
#define ONVIF_WSDD_PORT 3702
/** WS-Discovery IPv4 多播地址 */
#define ONVIF_WSDD_MULTICAST_ADDRESS "239.255.255.250"
/** ONVIF 网络视频发送者设备类型 (NVT) */
#define ONVIF_NETWORK_VIDEO_TRANSMITTER "dn:NetworkVideoTransmitter"

// ONVIF Discovery Feature 规范要求 ProbeMatch 同时声明通用 Device 与 NVT 类型。
// 只声明 NVT 虽然很多客户端能识别，但 ODM 会用两种类型交叉过滤。
/** 设备发现时响应的设备类型列表 */
#define ONVIF_DISCOVERY_TYPES "dn:NetworkVideoTransmitter tds:Device"

/* gSOAP owns all response memory through the current struct soap. */
/**
 * @brief 分配并清零 SOAP 管理的内存
 * @param soap SOAP 上下文实例，用于内存生命周期管理
 * @param size 需要分配的字节数
 * @return 分配的内存指针，分配失败则返回 NULL
 */
static void *soap_calloc(struct soap *soap, size_t size)
{
    void *memory = soap_malloc(soap, size);
    if (memory != NULL)
        memset(memory, 0, size);
    return memory;
}

/**
 * @brief 在 SOAP 管理的内存中复制字符串
 * @param soap SOAP 上下文实例
 * @param text 需要复制的源字符串（以 null 结尾）
 * @return 复制后的字符串指针，分配失败则返回 NULL
 */
static char *soap_copy_text(struct soap *soap, const char *text)
{
    const size_t length = strlen(text) + 1;
    char *copy = (char *)soap_malloc(soap, length);
    if (copy != NULL)
        memcpy(copy, text, length);
    return copy;
}

/**
 * @brief 在 SOAP 管理的内存中复制指定长度的字符串，并自动添加 null 终止符
 * @param soap SOAP 上下文实例
 * @param begin 字符串起始位置
 * @param length 需要复制的长度
 * @return 复制后的字符串指针，分配失败则返回 NULL
 */
static char *soap_copy_text_range(struct soap *soap, const char *begin, size_t length)
{
    char *copy = (char *)soap_malloc(soap, length + 1);
    if (copy != NULL) {
        memcpy(copy, begin, length);
        copy[length] = '\0';
    }
    return copy;
}

/**
 * @brief 在 SOAP 内存中分配一个布尔值并初始化
 * @param soap SOAP 上下文实例
 * @param value 要设置的布尔值枚举
 * @return 布尔值的指针，分配失败则返回 NULL
 */
static enum xsd__boolean *soap_boolean(struct soap *soap, enum xsd__boolean value)
{
    enum xsd__boolean *result = (enum xsd__boolean *)soap_calloc(soap, sizeof(*result));
    if (result != NULL)
        *result = value;
    return result;
}

/**
 * @brief 获取与当前 SOAP 实例绑定的服务配置
 * @param soap SOAP 上下文实例
 * @return 配置指针，保存在 soap->user 中
 */
static const OnvifSoapServiceConfig *service_config(const struct soap *soap)
{
    return (const OnvifSoapServiceConfig *)soap->user;
}

/*
 * ONVIF 的 Device、Media、Media2 操作都是 HTTP POST。认证在每个 gSOAP handler
 * 进入时经过同一个门，而不是让业务函数自己比较用户名/密码。
 *
 * 未配置账号时保持匿名模式，便于 demo 和尚未配置设备的开发流程；一旦 username
 * 与 password 同时存在，只接受 HTTP Digest，不接受会明文传密码的 HTTP Basic。
 */
static int require_http_digest_authentication(struct soap *soap)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    if (config == NULL)
        return SOAP_FAULT;
    if (config->username[0] == '\0')
        return SOAP_OK;

    if (soap->authrealm != NULL && soap->userid != NULL &&
        strcmp(soap->authrealm, config->authenticationRealm) == 0 &&
        strcmp(soap->userid, config->username) == 0 &&
        http_da_verify_post(soap, config->password) == SOAP_OK) {
        return SOAP_OK;
    }

    /* http_da 插件会依据 realm 生成 nonce/opaque 和 WWW-Authenticate: Digest。 */
    soap->authrealm = config->authenticationRealm;
    return 401;
}

#define ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap_context) \
    do { \
        const int authentication_result = require_http_digest_authentication((soap_context)); \
        if (authentication_result != SOAP_OK) \
            return authentication_result; \
    } while (0)

/**
 * @brief 检查是否收到了停止服务的请求
 * @param callback 用户提供的停止检查回调函数
 * @param context 回调函数所需的上下文指针
 * @return 如果已请求停止返回 1，否则返回 0
 */
static int stop_requested(OnvifSoapStopRequestedFn callback, void *context)
{
    return callback != NULL && callback(context) != 0;
}

/**
 * @brief 汇报服务启动结果
 * @param callback 用户提供的启动通知回调函数
 * @param context 回调函数所需的上下文指针
 * @param success 启动成功为 1，失败为 0
 * @param error_text 如果启动失败，此参数包含错误描述
 */
static void report_started(OnvifSoapStartedFn callback, void *context,
                           int success, const char *error_text)
{
    if (callback != NULL)
        callback(context, success, error_text);
}

/**
 * @brief 生成当前系统的 UTC 时间并填充到 SOAP 响应结构中
 * @param soap SOAP 上下文实例
 * @param output 用于保存输出时间结构的指针地址
 * @return 成功返回 SOAP_OK，分配内存失败或时间获取失败返回 SOAP_EOM
 */
static int populate_utc_time(struct soap *soap, struct tt__SystemDateTime **output)
{
    time_t now;
    struct tm utc;
    struct tt__SystemDateTime *system_time;
    struct tt__DateTime *date_time;
    struct tt__Date *date;
    struct tt__Time *utc_time;

    now = time(NULL);
    if (gmtime_r(&now, &utc) == NULL)
        return SOAP_EOM;

    system_time = (struct tt__SystemDateTime *)soap_calloc(soap, sizeof(*system_time));
    date_time = (struct tt__DateTime *)soap_calloc(soap, sizeof(*date_time));
    date = (struct tt__Date *)soap_calloc(soap, sizeof(*date));
    utc_time = (struct tt__Time *)soap_calloc(soap, sizeof(*utc_time));
    if (system_time == NULL || date_time == NULL || date == NULL || utc_time == NULL)
        return SOAP_EOM;

    system_time->DateTimeType = tt__SetDateTimeType__Manual;
    system_time->DaylightSavings = xsd__boolean__false_;
    system_time->UTCDateTime = date_time;
    date_time->Date = date;
    date_time->Time = utc_time;
    date->Year = utc.tm_year + 1900;
    date->Month = utc.tm_mon + 1;
    date->Day = utc.tm_mday;
    utc_time->Hour = utc.tm_hour;
    utc_time->Minute = utc.tm_min;
    utc_time->Second = utc.tm_sec;
    *output = system_time;
    return SOAP_OK;
}

/* Required by gSOAP's generic dispatcher. ONVIF Device Service does not
 * consume incoming Fault messages, so accepting one is sufficient here. */
/**
 * @brief 处理 SOAP 错误消息 (gSOAP 要求实现的回调)
 *
 * ONVIF Device 服务通常不接收 Fault 消息，此处提供空实现以满足编译要求。
 *
 * @param soap SOAP 上下文实例
 * @param faultcode 错误代码
 * @param faultstring 错误描述
 * @param faultactor 触发错误的参与者
 * @param detail 错误详情结构
 * @param code 错误代码结构
 * @param reason 错误原因结构
 * @param node 发生错误的节点
 * @param role 角色信息
 * @param detail12 SOAP 1.2 错误详情
 * @return 始终返回 SOAP_OK
 */
int SOAP_ENV__Fault(struct soap *soap, char *faultcode, char *faultstring,
                    char *faultactor, struct SOAP_ENV__Detail *detail,
                    struct SOAP_ENV__Code *code, struct SOAP_ENV__Reason *reason,
                    char *node, char *role, struct SOAP_ENV__Detail *detail12)
{
    (void)soap; (void)faultcode; (void)faultstring; (void)faultactor; (void)detail;
    (void)code; (void)reason; (void)node; (void)role; (void)detail12;
    return SOAP_OK;
}

/*
 * The following six globals are the intentionally implemented part of the
 * complete Device WSDL. gSOAP parses the request before it calls us and
 * serializes `response` after SOAP_OK is returned. All other generated Device
 * operations return SOAP_NO_METHOD from onvifUnimplementedOps.c.
 */
/**
 * @brief 获取设备基础信息
 *
 * ONVIF Device 核心 API 之一。向客户端返回设备的制造商、型号、固件版本号等信息。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体 (空)
 * @param response SOAP 响应结构体，将由该函数负责填充
 * @return 成功返回 SOAP_OK，否则返回 SOAP_FAULT 或 SOAP_EOM
 */
int __tds__GetDeviceInformation(struct soap *soap,
                                struct _tds__GetDeviceInformation *request,
                                struct _tds__GetDeviceInformationResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    if (config == NULL)
        return SOAP_FAULT;
    response->Manufacturer = soap_copy_text(soap, config->manufacturer);
    response->Model = soap_copy_text(soap, config->model);
    response->FirmwareVersion = soap_copy_text(soap, config->firmwareVersion);
    response->SerialNumber = soap_copy_text(soap, config->serialNumber);
    response->HardwareId = soap_copy_text(soap, config->hardwareId);
    return response->Manufacturer != NULL && response->Model != NULL &&
                   response->FirmwareVersion != NULL && response->SerialNumber != NULL &&
                   response->HardwareId != NULL
               ? SOAP_OK
               : SOAP_EOM;
}

/**
 * @brief 获取系统日期和时间
 *
 * 返回设备当前的 UTC 时间。许多 ONVIF 客户端在发送带签名的 SOAP 报文前会调用此接口同步时间。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，将填充系统时间
 * @return 成功返回 SOAP_OK，分配失败返回 SOAP_EOM
 */
int __tds__GetSystemDateAndTime(struct soap *soap,
                                struct _tds__GetSystemDateAndTime *request,
                                struct _tds__GetSystemDateAndTimeResponse *response)
{
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    return populate_utc_time(soap, &response->SystemDateAndTime);
}

/**
 * @brief 获取设备的 ONVIF 作用域 (Scopes)
 *
 * 作用域是客户端识别设备属性（如物理位置、设备名称等）的重要依据，空格分隔。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，返回所有固定 Scope 项
 * @return 成功返回 SOAP_OK
 */
int __tds__GetScopes(struct soap *soap, struct _tds__GetScopes *request,
                     struct _tds__GetScopesResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    const char *cursor;
    int count = 0;
    int index = 0;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    if (config == NULL)
        return SOAP_FAULT;

    for (cursor = config->scopes; *cursor != '\0';) {
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            break;
        ++count;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;
    }
    response->__sizeScopes = count;
    response->Scopes = (struct tt__Scope *)soap_calloc(soap, (size_t)count * sizeof(*response->Scopes));
    if (response->Scopes == NULL)
        return SOAP_EOM;

    for (cursor = config->scopes; *cursor != '\0';) {
        const char *begin;
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            break;
        begin = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;
        response->Scopes[index].ScopeDef = tt__ScopeDefinition__Fixed;
        response->Scopes[index].ScopeItem = soap_copy_text_range(soap, begin, (size_t)(cursor - begin));
        if (response->Scopes[index].ScopeItem == NULL)
            return SOAP_EOM;
        ++index;
    }
    return SOAP_OK;
}

/**
 * @brief 获取设备服务的能力集 (Capabilities)
 *
 * 返回与设备本身相关的能力（如网络、安全、系统）。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，将填充能力集数据
 * @return 成功返回 SOAP_OK
 */
int __tds__GetServiceCapabilities(struct soap *soap,
                                  struct _tds__GetServiceCapabilities *request,
                                  struct _tds__GetServiceCapabilitiesResponse *response)
{
    struct tds__DeviceServiceCapabilities *capabilities;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    capabilities = (struct tds__DeviceServiceCapabilities *)soap_calloc(soap, sizeof(*capabilities));
    if (capabilities == NULL)
        return SOAP_EOM;
    capabilities->Network = (struct tds__NetworkCapabilities *)soap_calloc(soap, sizeof(*capabilities->Network));
    capabilities->Security = (struct tds__SecurityCapabilities *)soap_calloc(soap, sizeof(*capabilities->Security));
    capabilities->System = (struct tds__SystemCapabilities *)soap_calloc(soap, sizeof(*capabilities->System));
    if (capabilities->Network == NULL || capabilities->Security == NULL || capabilities->System == NULL)
        return SOAP_EOM;
    /* Authentication is introduced only when the full ONVIF auth policy exists. */
    capabilities->System->DiscoveryResolve = soap_boolean(soap, xsd__boolean__false_);
    if (capabilities->System->DiscoveryResolve == NULL)
        return SOAP_EOM;
    response->Capabilities = capabilities;
    return SOAP_OK;
}

/**
 * @brief 获取设备支持的服务列表 (GetServices)
 *
 * 向客户端返回当前设备启用的所有 ONVIF 服务的端点 URL 和支持的版本号（包含 Device, Media, Media2）。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，将填充服务列表和对应版本
 * @return 成功返回 SOAP_OK
 */
int __tds__GetServices(struct soap *soap, struct _tds__GetServices *request,
                       struct _tds__GetServicesResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    struct tds__Service *services;
    struct tt__OnvifVersion *versions;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    if (config == NULL)
        return SOAP_FAULT;
    services = (struct tds__Service *)soap_calloc(soap, 3 * sizeof(*services));
    versions = (struct tt__OnvifVersion *)soap_calloc(soap, 3 * sizeof(*versions));
    if (services == NULL || versions == NULL)
        return SOAP_EOM;
    response->__sizeService = 3;
    response->Service = services;

    services[0].Namespace = soap_copy_text(soap, ONVIF_DEVICE_NAMESPACE);
    services[0].XAddr = soap_copy_text(soap, config->deviceServiceUrl);
    services[0].Version = &versions[0];
    versions[0].Major = 25;
    versions[0].Minor = 6;

    services[1].Namespace = soap_copy_text(soap, ONVIF_MEDIA_NAMESPACE);
    services[1].XAddr = soap_copy_text(soap, config->mediaServiceUrl);
    services[1].Version = &versions[1];
    versions[1].Major = 1;
    versions[1].Minor = 0;

    services[2].Namespace = soap_copy_text(soap, ONVIF_MEDIA2_NAMESPACE);
    services[2].XAddr = soap_copy_text(soap, config->media2ServiceUrl);
    services[2].Version = &versions[2];
    versions[2].Major = 20;
    versions[2].Minor = 12;

    return services[0].Namespace != NULL && services[0].XAddr != NULL &&
           services[1].Namespace != NULL && services[1].XAddr != NULL &&
           services[2].Namespace != NULL && services[2].XAddr != NULL
               ? SOAP_OK
               : SOAP_EOM;
}

/**
 * @brief 获取设备的兼容性能力集 (GetCapabilities)
 *
 * 返回各具体服务（例如 Media）的端点 URL 及其能力描述（如支持 RTP/RTSP/TCP 流媒体等）。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，填充旧版 Capabilities 接口需要的设备与媒体能力
 * @return 成功返回 SOAP_OK
 */
int __tds__GetCapabilities(struct soap *soap, struct _tds__GetCapabilities *request,
                           struct _tds__GetCapabilitiesResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    struct tt__Capabilities *capabilities;
    struct tt__DeviceCapabilities *device;
    struct tt__MediaCapabilities *media;
    struct tt__RealTimeStreamingCapabilities *streaming;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    if (config == NULL)
        return SOAP_FAULT;
    capabilities = (struct tt__Capabilities *)soap_calloc(soap, sizeof(*capabilities));
    device = (struct tt__DeviceCapabilities *)soap_calloc(soap, sizeof(*device));
    media = (struct tt__MediaCapabilities *)soap_calloc(soap, sizeof(*media));
    streaming = (struct tt__RealTimeStreamingCapabilities *)soap_calloc(soap, sizeof(*streaming));
    if (capabilities == NULL || device == NULL || media == NULL || streaming == NULL)
        return SOAP_EOM;
    device->XAddr = soap_copy_text(soap, config->deviceServiceUrl);
    media->XAddr = soap_copy_text(soap, config->mediaServiceUrl);
    streaming->RTP_USCORERTSP_USCORETCP = soap_boolean(soap, xsd__boolean__true_);
    if (device->XAddr == NULL || media->XAddr == NULL || streaming->RTP_USCORERTSP_USCORETCP == NULL)
        return SOAP_EOM;
    media->StreamingCapabilities = streaming;
    capabilities->Device = device;
    capabilities->Media = media;
    response->Capabilities = capabilities;
    return SOAP_OK;
}

/**
 * @brief 填充 AAC 音频编码配置 (Media v1)
 * @param soap SOAP 上下文实例
 * @param profile 要填充的 Profile 结构体
 * @param token 音频配置的唯一标识
 * @return 成功返回 SOAP_OK
 */
static int fill_aac_configuration(struct soap *soap, struct tt__Profile *profile, const char *token)
{
    struct tt__AudioEncoderConfiguration *audio =
        (struct tt__AudioEncoderConfiguration *)soap_calloc(soap, sizeof(*audio));
    if (audio == NULL)
        return SOAP_EOM;
    audio->Name = soap_copy_text(soap, "AAC-LC 48kHz");
    audio->token = soap_copy_text(soap, token);
    audio->Encoding = tt__AudioEncoding__AAC;
    audio->Bitrate = 128;
    audio->SampleRate = 48;
    audio->UseCount = 1;
    audio->SessionTimeout = soap_copy_text(soap, "PT60S");
    if (audio->Name == NULL || audio->token == NULL || audio->SessionTimeout == NULL)
        return SOAP_EOM;
    profile->AudioEncoderConfiguration = audio;
    return SOAP_OK;
}

/**
 * @brief 填充子码流 (H264 720p) 视频编码配置 (Media v1)
 * @param soap SOAP 上下文实例
 * @param profile 要填充的 Profile 结构体
 * @return 成功返回 SOAP_OK
 */
static int fill_h264_sub_configuration(struct soap *soap, struct tt__Profile *profile)
{
    struct tt__VideoEncoderConfiguration *video =
        (struct tt__VideoEncoderConfiguration *)soap_calloc(soap, sizeof(*video));
    struct tt__VideoResolution *resolution =
        (struct tt__VideoResolution *)soap_calloc(soap, sizeof(*resolution));
    struct tt__VideoRateControl *rate_control =
        (struct tt__VideoRateControl *)soap_calloc(soap, sizeof(*rate_control));
    struct tt__H264Configuration *h264 =
        (struct tt__H264Configuration *)soap_calloc(soap, sizeof(*h264));
    if (video == NULL || resolution == NULL || rate_control == NULL || h264 == NULL)
        return SOAP_EOM;
    video->Name = soap_copy_text(soap, "sub H264 1280x720");
    video->token = soap_copy_text(soap, "sub_video_h264");
    video->Encoding = tt__VideoEncoding__H264;
    video->Resolution = resolution;
    video->Quality = 5.0F;
    video->RateControl = rate_control;
    video->H264 = h264;
    video->UseCount = 1;
    video->SessionTimeout = soap_copy_text(soap, "PT60S");
    resolution->Width = 1280;
    resolution->Height = 720;
    rate_control->FrameRateLimit = 30;
    rate_control->EncodingInterval = 1;
    rate_control->BitrateLimit = 2048;
    h264->GovLength = 30;
    h264->H264Profile = tt__H264Profile__High;
    if (video->Name == NULL || video->token == NULL || video->SessionTimeout == NULL)
        return SOAP_EOM;
    profile->VideoEncoderConfiguration = video;
    return SOAP_OK;
}

/**
 * @brief 填充主码流 (H265 1080p, 兼容标记为 H264) 视频编码配置 (Media v1)
 * @param soap SOAP 上下文实例
 * @param profile 要填充的 Profile 结构体
 * @return 成功返回 SOAP_OK
 */
static int fill_h264_main_configuration(struct soap *soap, struct tt__Profile *profile)
{
    struct tt__VideoEncoderConfiguration *video =
        (struct tt__VideoEncoderConfiguration *)soap_calloc(soap, sizeof(*video));
    struct tt__VideoResolution *resolution =
        (struct tt__VideoResolution *)soap_calloc(soap, sizeof(*resolution));
    struct tt__VideoRateControl *rate_control =
        (struct tt__VideoRateControl *)soap_calloc(soap, sizeof(*rate_control));
    struct tt__H264Configuration *h264 =
        (struct tt__H264Configuration *)soap_calloc(soap, sizeof(*h264));
    if (video == NULL || resolution == NULL || rate_control == NULL || h264 == NULL)
        return SOAP_EOM;
    video->Name = soap_copy_text(soap, "main H265 1920x1080");
    video->token = soap_copy_text(soap, "main_video_h265");
    /* Media v1 tt__VideoEncoding defines only JPEG/MPEG4/H264. To allow Profile S clients
     * to recognize the video track and obtain its 1080p resolution while streaming H.265 via RTSP SDP,
     * declare H264 here for backward compatibility. Media2 provides native H265. */
    video->Encoding = tt__VideoEncoding__H264;
    video->Resolution = resolution;
    video->Quality = 5.0F;
    video->RateControl = rate_control;
    video->H264 = h264;
    video->UseCount = 1;
    video->SessionTimeout = soap_copy_text(soap, "PT60S");
    resolution->Width = 1920;
    resolution->Height = 1080;
    rate_control->FrameRateLimit = 30;
    rate_control->EncodingInterval = 1;
    rate_control->BitrateLimit = 4096;
    h264->GovLength = 30;
    h264->H264Profile = tt__H264Profile__High;
    if (video->Name == NULL || video->token == NULL || video->SessionTimeout == NULL)
        return SOAP_EOM;
    profile->VideoEncoderConfiguration = video;
    return SOAP_OK;
}

/**
 * @brief 填充完整的 Media Profile (Media v1)
 * @param soap SOAP 上下文实例
 * @param profile 要填充的 Profile 结构体
 * @param token Profile 唯一标识 (如 "main" 或 "sub")
 * @param name Profile 人类可读名称
 * @param is_sub 是否为子码流标志
 * @return 成功返回 SOAP_OK
 */
static int fill_media_profile(struct soap *soap, struct tt__Profile *profile,
                              const char *token, const char *name, int is_sub)
{
    profile->Name = soap_copy_text(soap, name);
    profile->token = soap_copy_text(soap, token);
    profile->fixed = soap_boolean(soap, xsd__boolean__true_);
    if (profile->Name == NULL || profile->token == NULL || profile->fixed == NULL)
        return SOAP_EOM;
    if (fill_aac_configuration(soap, profile, is_sub ? "sub_audio_aac" : "main_audio_aac") != SOAP_OK)
        return SOAP_EOM;
    return is_sub ? fill_h264_sub_configuration(soap, profile) : fill_h264_main_configuration(soap, profile);
}

/**
 * @brief 获取媒体服务能力 (Media v1 GetServiceCapabilities)
 *
 * 返回 Media v1 服务的各项能力，如最大 Profile 数量，以及支持通过 TCP 进行 RTSP 流传输。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，包含媒体能力信息
 * @return 成功返回 SOAP_OK
 */
int __trt__GetServiceCapabilities(struct soap *soap,
                                  struct _trt__GetServiceCapabilities *request,
                                  struct _trt__GetServiceCapabilitiesResponse *response)
{
    struct trt__Capabilities *capabilities;
    struct trt__ProfileCapabilities *profiles;
    struct trt__StreamingCapabilities *streaming;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    capabilities = (struct trt__Capabilities *)soap_calloc(soap, sizeof(*capabilities));
    profiles = (struct trt__ProfileCapabilities *)soap_calloc(soap, sizeof(*profiles));
    streaming = (struct trt__StreamingCapabilities *)soap_calloc(soap, sizeof(*streaming));
    if (capabilities == NULL || profiles == NULL || streaming == NULL)
        return SOAP_EOM;
    profiles->MaximumNumberOfProfiles = (int *)soap_calloc(soap, sizeof(*profiles->MaximumNumberOfProfiles));
    streaming->RTP_USCORERTSP_USCORETCP = soap_boolean(soap, xsd__boolean__true_);
    if (profiles->MaximumNumberOfProfiles == NULL || streaming->RTP_USCORERTSP_USCORETCP == NULL)
        return SOAP_EOM;
    *profiles->MaximumNumberOfProfiles = 2;
    capabilities->ProfileCapabilities = profiles;
    capabilities->StreamingCapabilities = streaming;
    response->Capabilities = capabilities;
    return SOAP_OK;
}

/**
 * @brief 获取所有媒体配置档案 (Media v1 GetProfiles)
 *
 * 向客户端返回设备上可用的所有 Profile（包含主码流与子码流）。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，包含 Profile 列表
 * @return 成功返回 SOAP_OK
 */
int __trt__GetProfiles(struct soap *soap, struct _trt__GetProfiles *request,
                       struct _trt__GetProfilesResponse *response)
{
    struct tt__Profile *profiles;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    profiles = (struct tt__Profile *)soap_calloc(soap, 2 * sizeof(*profiles));
    if (profiles == NULL)
        return SOAP_EOM;
    if (fill_media_profile(soap, &profiles[0], "main", "Main Stream (H265)", 0) != SOAP_OK ||
        fill_media_profile(soap, &profiles[1], "sub", "Sub Stream (H264)", 1) != SOAP_OK) {
        return SOAP_EOM;
    }
    response->__sizeProfiles = 2;
    response->Profiles = profiles;
    return SOAP_OK;
}

/**
 * @brief 获取指定的媒体配置档案 (Media v1 GetProfile)
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体，必须包含请求的 ProfileToken
 * @param response SOAP 响应结构体，包含匹配的 Profile 数据
 * @return 成功返回 SOAP_OK，找不到则返回 SOAP_FAULT
 */
int __trt__GetProfile(struct soap *soap, struct _trt__GetProfile *request,
                      struct _trt__GetProfileResponse *response)
{
    struct tt__Profile *profile;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    const int is_sub = request != NULL && request->ProfileToken != NULL &&
                       strcmp(request->ProfileToken, "sub") == 0;
    if (request == NULL || request->ProfileToken == NULL ||
        (strcmp(request->ProfileToken, "main") != 0 && !is_sub)) {
        return SOAP_FAULT;
    }
    profile = (struct tt__Profile *)soap_calloc(soap, sizeof(*profile));
    if (profile == NULL)
        return SOAP_EOM;
    if (fill_media_profile(soap, profile, is_sub ? "sub" : "main",
                           is_sub ? "Sub Stream (H264)" : "Main Stream (H265)", is_sub) != SOAP_OK) {
        return SOAP_EOM;
    }
    response->Profile = profile;
    return SOAP_OK;
}

/**
 * @brief 获取指定 Profile 的流媒体地址 (Media v1 GetStreamUri)
 *
 * 返回供客户端进行 RTSP 拉流的 URL 地址。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体，指定需要的 ProfileToken 等信息
 * @param response SOAP 响应结构体，包含流媒体 URI
 * @return 成功返回 SOAP_OK，匹配失败返回 SOAP_FAULT
 */
int __trt__GetStreamUri(struct soap *soap, struct _trt__GetStreamUri *request,
                        struct _trt__GetStreamUriResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    const char *rtsp_url;
    struct tt__MediaUri *media_uri;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    if (config == NULL || request == NULL || request->ProfileToken == NULL)
        return SOAP_FAULT;
    if (strcmp(request->ProfileToken, "main") == 0)
        rtsp_url = config->mainRtspUrl;
    else if (strcmp(request->ProfileToken, "sub") == 0)
        rtsp_url = config->subRtspUrl;
    else
        return SOAP_FAULT;
    media_uri = (struct tt__MediaUri *)soap_calloc(soap, sizeof(*media_uri));
    if (media_uri == NULL)
        return SOAP_EOM;
    media_uri->Uri = soap_copy_text(soap, rtsp_url);
    media_uri->InvalidAfterConnect = xsd__boolean__false_;
    media_uri->InvalidAfterReboot = xsd__boolean__false_;
    media_uri->Timeout = soap_copy_text(soap, "PT0S");
    if (media_uri->Uri == NULL || media_uri->Timeout == NULL)
        return SOAP_EOM;
    response->MediaUri = media_uri;
    return SOAP_OK;
}

/*
 * ONVIF Media2 (ver20 Media) Service Implementation
 */

/**
 * @brief 填充 Media2 视频编码器配置
 * @param soap SOAP 上下文实例
 * @param token 视频配置的唯一标识
 * @param name 视频配置的人类可读名称
 * @param encoding 编码格式 (如 "H264", "H265")
 * @param width 视频宽度
 * @param height 视频高度
 * @param fps 帧率限制
 * @param bitrate_kbps 码率限制 (kbps)
 * @return 视频编码配置指针，失败返回 NULL
 */
static struct tt__VideoEncoder2Configuration *fill_media2_video_encoder(
    struct soap *soap, const char *token, const char *name,
    const char *encoding, int width, int height, float fps, int bitrate_kbps)
{
    struct tt__VideoEncoder2Configuration *video =
        (struct tt__VideoEncoder2Configuration *)soap_calloc(soap, sizeof(*video));
    struct tt__VideoResolution2 *resolution =
        (struct tt__VideoResolution2 *)soap_calloc(soap, sizeof(*resolution));
    struct tt__VideoRateControl2 *rate_control =
        (struct tt__VideoRateControl2 *)soap_calloc(soap, sizeof(*rate_control));
    if (video == NULL || resolution == NULL || rate_control == NULL)
        return NULL;
    video->Name = soap_copy_text(soap, name);
    video->token = soap_copy_text(soap, token);
    video->UseCount = 1;
    video->Encoding = soap_copy_text(soap, encoding);
    video->Resolution = resolution;
    video->RateControl = rate_control;
    video->Quality = 5.0f;
    resolution->Width = width;
    resolution->Height = height;
    rate_control->FrameRateLimit = fps;
    rate_control->BitrateLimit = bitrate_kbps;
    if (video->Name == NULL || video->token == NULL || video->Encoding == NULL)
        return NULL;
    return video;
}

/**
 * @brief 填充 Media2 音频编码器配置
 * @param soap SOAP 上下文实例
 * @param token 音频配置的唯一标识
 * @param name 音频配置的人类可读名称
 * @param encoding 编码格式 (如 "MP4A-LATM")
 * @param bitrate_kbps 码率限制 (kbps)
 * @param sample_rate_khz 采样率 (kHz)
 * @return 音频编码配置指针，失败返回 NULL
 */
static struct tt__AudioEncoder2Configuration *fill_media2_audio_encoder(
    struct soap *soap, const char *token, const char *name,
    const char *encoding, int bitrate_kbps, int sample_rate_khz)
{
    struct tt__AudioEncoder2Configuration *audio =
        (struct tt__AudioEncoder2Configuration *)soap_calloc(soap, sizeof(*audio));
    if (audio == NULL)
        return NULL;
    audio->Name = soap_copy_text(soap, name);
    audio->token = soap_copy_text(soap, token);
    audio->UseCount = 1;
    audio->Encoding = soap_copy_text(soap, encoding);
    audio->Bitrate = bitrate_kbps;
    audio->SampleRate = sample_rate_khz;
    if (audio->Name == NULL || audio->token == NULL || audio->Encoding == NULL)
        return NULL;
    return audio;
}

/**
 * @brief 填充完整的 Media2 媒体配置档案
 * @param soap SOAP 上下文实例
 * @param profile 要填充的 Media2 Profile 结构体
 * @param token Profile 唯一标识
 * @param name Profile 人类可读名称
 * @param is_sub 是否为子码流
 * @return 成功返回 SOAP_OK
 */
static int fill_media2_profile(struct soap *soap, struct tr2__MediaProfile *profile,
                               const char *token, const char *name, int is_sub)
{
    struct tr2__ConfigurationSet *configs;
    profile->Name = soap_copy_text(soap, name);
    profile->token = soap_copy_text(soap, token);
    profile->fixed = soap_boolean(soap, xsd__boolean__true_);
    if (profile->Name == NULL || profile->token == NULL || profile->fixed == NULL)
        return SOAP_EOM;

    configs = (struct tr2__ConfigurationSet *)soap_calloc(soap, sizeof(*configs));
    if (configs == NULL)
        return SOAP_EOM;

    if (is_sub) {
        configs->VideoEncoder = fill_media2_video_encoder(
            soap, "sub_video_h264", "sub H264 1280x720", "H264", 1280, 720, 30.0f, 2048);
        configs->AudioEncoder = fill_media2_audio_encoder(
            soap, "sub_audio_aac", "AAC-LC 48kHz", "MP4A-LATM", 128, 48);
    } else {
        configs->VideoEncoder = fill_media2_video_encoder(
            soap, "main_video_h265", "main H265 1920x1080", "H265", 1920, 1080, 30.0f, 4096);
        configs->AudioEncoder = fill_media2_audio_encoder(
            soap, "main_audio_aac", "AAC-LC 48kHz", "MP4A-LATM", 128, 48);
    }
    if (configs->VideoEncoder == NULL || configs->AudioEncoder == NULL)
        return SOAP_EOM;

    profile->Configurations = configs;
    return SOAP_OK;
}

/**
 * @brief 获取媒体服务能力 (Media v2 GetServiceCapabilities)
 *
 * 返回 Media2 的流媒体能力及 Profile 数量限制。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，将填充能力信息
 * @return 成功返回 SOAP_OK
 */
int __tr2__GetServiceCapabilities(struct soap *soap,
                                  struct _tr2__GetServiceCapabilities *request,
                                  struct _tr2__GetServiceCapabilitiesResponse *response)
{
    struct tr2__Capabilities2 *capabilities;
    struct tr2__ProfileCapabilities *profiles;
    struct tr2__StreamingCapabilities *streaming;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    (void)request;
    capabilities = (struct tr2__Capabilities2 *)soap_calloc(soap, sizeof(*capabilities));
    profiles = (struct tr2__ProfileCapabilities *)soap_calloc(soap, sizeof(*profiles));
    streaming = (struct tr2__StreamingCapabilities *)soap_calloc(soap, sizeof(*streaming));
    if (capabilities == NULL || profiles == NULL || streaming == NULL)
        return SOAP_EOM;
    profiles->MaximumNumberOfProfiles = (int *)soap_calloc(soap, sizeof(*profiles->MaximumNumberOfProfiles));
    if (profiles->MaximumNumberOfProfiles == NULL)
        return SOAP_EOM;
    *profiles->MaximumNumberOfProfiles = 2;
    streaming->RTSPStreaming = soap_boolean(soap, xsd__boolean__true_);
    streaming->RTP_USCORERTSP_USCORETCP = soap_boolean(soap, xsd__boolean__true_);
    if (streaming->RTSPStreaming == NULL || streaming->RTP_USCORERTSP_USCORETCP == NULL)
        return SOAP_EOM;
    capabilities->ProfileCapabilities = profiles;
    capabilities->StreamingCapabilities = streaming;
    response->Capabilities = capabilities;
    return SOAP_OK;
}

/**
 * @brief 获取媒体配置档案 (Media v2 GetProfiles)
 *
 * 支持按 Token 获取指定的 Profile，或在不指定时返回全部。
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体，可选包含目标 Token
 * @param response SOAP 响应结构体，将填充相应的 Profile 列表
 * @return 成功返回 SOAP_OK
 */
int __tr2__GetProfiles(struct soap *soap, struct _tr2__GetProfiles *request,
                       struct _tr2__GetProfilesResponse *response)
{
    struct tr2__MediaProfile *profiles;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    if (request != NULL && request->Token != NULL && *request->Token != '\0') {
        const int is_sub = strcmp(request->Token, "sub") == 0;
        if (strcmp(request->Token, "main") != 0 && !is_sub)
            return SOAP_FAULT;
        profiles = (struct tr2__MediaProfile *)soap_calloc(soap, sizeof(*profiles));
        if (profiles == NULL)
            return SOAP_EOM;
        if (fill_media2_profile(soap, profiles, is_sub ? "sub" : "main",
                                is_sub ? "Sub Stream (H264)" : "Main Stream (H265)", is_sub) != SOAP_OK)
            return SOAP_EOM;
        response->__sizeProfiles = 1;
        response->Profiles = profiles;
        return SOAP_OK;
    }

    profiles = (struct tr2__MediaProfile *)soap_calloc(soap, 2 * sizeof(*profiles));
    if (profiles == NULL)
        return SOAP_EOM;
    if (fill_media2_profile(soap, &profiles[0], "main", "Main Stream (H265)", 0) != SOAP_OK ||
        fill_media2_profile(soap, &profiles[1], "sub", "Sub Stream (H264)", 1) != SOAP_OK) {
        return SOAP_EOM;
    }
    response->__sizeProfiles = 2;
    response->Profiles = profiles;
    return SOAP_OK;
}

/**
 * @brief 获取指定 Profile 的流媒体地址 (Media v2 GetStreamUri)
 *
 * @param soap SOAP 上下文实例
 * @param request SOAP 请求结构体
 * @param response SOAP 响应结构体，将填充 RTSP 拉流 URI
 * @return 成功返回 SOAP_OK
 */
int __tr2__GetStreamUri(struct soap *soap, struct _tr2__GetStreamUri *request,
                        struct _tr2__GetStreamUriResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    const char *rtsp_url;
    ONVIF_REQUIRE_AUTHENTICATED_REQUEST(soap);
    if (config == NULL || request == NULL || request->ProfileToken == NULL)
        return SOAP_FAULT;
    if (strcmp(request->ProfileToken, "main") == 0)
        rtsp_url = config->mainRtspUrl;
    else if (strcmp(request->ProfileToken, "sub") == 0)
        rtsp_url = config->subRtspUrl;
    else
        return SOAP_FAULT;
    response->Uri = soap_copy_text(soap, rtsp_url);
    return response->Uri != NULL ? SOAP_OK : SOAP_EOM;
}

/**
 * @brief 判断 WS-Discovery Probe 请求是否匹配本设备类型
 * @param types Probe 请求中指定的类型列表字符串
 * @return 匹配返回 1，不匹配返回 0
 */
static int probe_requests_this_device_type(const char *types)
{
    /* ODM 会同时探测 ONVIF 具体设备类型 dn:NetworkVideoTransmitter 与通用的
       tds:Device。后者不是“不匹配”，而是它发现任意 ONVIF Device Service 的方式。 */
    return types == NULL || *types == '\0' ||
           strstr(types, "NetworkVideoTransmitter") != NULL || strstr(types, ":Device") != NULL;
}

/**
 * @brief 获取当前 UDP 通信对端的地址，格式化为 soap.udp://host:port
 * @param soap SOAP 上下文实例
 * @param endpoint 用于保存结果的缓冲区
 * @param endpoint_size 缓冲区大小
 * @return 成功返回 0，失败返回 -1
 */
static int get_peer_udp_endpoint(const struct soap *soap, char *endpoint, size_t endpoint_size)
{
    char host[NI_MAXHOST] = {0};
    char service[NI_MAXSERV] = {0};
    const int result = getnameinfo((const struct sockaddr *)&soap->peer, soap->peerlen,
                                   host, sizeof(host), service, sizeof(service),
                                   NI_NUMERICHOST | NI_NUMERICSERV);
    const int written = snprintf(endpoint, endpoint_size, "soap.udp://%s:%s", host, service);
    return result == 0 && written > 0 && (size_t)written < endpoint_size ? 0 : -1;
}

/* gSOAP's wsddapi plugin requires all six hooks. We only act on Probe: this
 * IPC is a discovery target, not a discovery client and has no Resolve yet. */
void wsdd_event_Hello(struct soap *soap, unsigned int a, const char *b, unsigned int c,
                      const char *d, const char *e, const char *f, const char *g,
                      const char *h, const char *i, const char *j, unsigned int k)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; }
void wsdd_event_Bye(struct soap *soap, unsigned int a, const char *b, unsigned int c,
                    const char *d, const char *e, const char *f, const char *g,
                    const char *h, const char *i, const char *j, unsigned int *k)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; }

/**
 * @brief WS-Discovery Probe 报文处理钩子
 *
 * 当收到 UDP 广播发现请求时被 gSOAP 调用。若匹配本设备类型，则向发送方单播回复 ProbeMatch。
 *
 * @param soap SOAP 上下文实例
 * @param message_id 请求的消息 ID
 * @param reply_to 回复地址
 * @param types 请求查找的设备类型
 * @param scopes 请求查找的作用域
 * @param match_by 匹配规则
 * @param matches 回复结构体
 * @return SOAP_WSDD_ADHOC
 */
soap_wsdd_mode wsdd_event_Probe(struct soap *soap, const char *message_id,
                                const char *reply_to, const char *types,
                                const char *scopes, const char *match_by,
                                struct wsdd__ProbeMatchesType *matches)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    char reply_endpoint[NI_MAXHOST + NI_MAXSERV + 16] = {0};
    (void)reply_to; (void)scopes; (void)match_by;
    if (config == NULL || message_id == NULL || !probe_requests_this_device_type(types))
        return SOAP_WSDD_ADHOC;
    soap_wsdd_init_ProbeMatches(soap, matches);
    if (soap_wsdd_add_ProbeMatch(soap, matches, config->endpointReference,
                                 ONVIF_DISCOVERY_TYPES, config->scopes, NULL,
                                 config->deviceServiceUrl, 1) != SOAP_OK ||
        get_peer_udp_endpoint(soap, reply_endpoint, sizeof(reply_endpoint)) != 0 ||
        soap_wsdd_ProbeMatches(soap, reply_endpoint, soap_wsa_rand_uuid(soap), message_id,
                               soap_wsa_anonymousURI, matches) != SOAP_OK) {
        fprintf(stderr, "OnvifSoapService: reply ProbeMatches failed soap=%d errno=%d\n",
                soap->error, soap->errnum);
        return SOAP_WSDD_ADHOC;
    }
    return SOAP_WSDD_ADHOC;
}
void wsdd_event_ProbeMatches(struct soap *soap, unsigned int a, const char *b, unsigned int c,
                             const char *d, const char *e, struct wsdd__ProbeMatchesType *f)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
soap_wsdd_mode wsdd_event_Resolve(struct soap *soap, const char *a, const char *b,
                                  const char *c, struct wsdd__ResolveMatchType *d)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; return SOAP_WSDD_ADHOC; }
void wsdd_event_ResolveMatches(struct soap *soap, unsigned int a, const char *b, unsigned int c,
                               const char *d, const char *e, struct wsdd__ResolveMatchType *f)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }

/**
 * @brief 运行 ONVIF HTTP SOAP 设备服务的主循环
 *
 * 监听指定的 HTTP 端口，接收并响应 ONVIF 客户端的 SOAP 请求。
 * 该函数会阻塞当前线程直到被请求停止。
 *
 * @param config 服务配置参数
 * @param port 监听的 HTTP 端口
 * @param should_stop 检查是否需要停止的回调
 * @param stop_context 停止回调上下文
 * @param on_started 服务启动结果的回调
 * @param started_context 启动结果回调上下文
 * @return 成功返回 0，失败返回 -1
 */
int onvif_soap_run_device_service(const OnvifSoapServiceConfig *config, unsigned short port,
                                  OnvifSoapStopRequestedFn should_stop, void *stop_context,
                                  OnvifSoapStartedFn on_started, void *started_context)
{
    struct soap soap;
    char error[128];
    if (config == NULL || port == 0) {
        report_started(on_started, started_context, 0, "invalid Device Service configuration");
        return -1;
    }
    soap_init1(&soap, SOAP_XML_TREE);
    soap.user = (void *)config;
    /* ONVIF Core 的经典 Digest 兼容路径使用 RFC 2617 MD5。RTSP 的 live555
       Digest 也使用同一账号；以后整体升级 RFC 7616 SHA-256 时在此统一切换。 */
    if (soap_register_plugin_arg(&soap, http_da, http_da_md5()) != SOAP_OK) {
        report_started(on_started, started_context, 0, "register HTTP Digest plugin failed");
        soap_done(&soap);
        return -1;
    }
    /* Rebinding after an app restart must not wait for old TCP TIME_WAIT sockets. */
    soap.bind_flags = SO_REUSEADDR;
    if (!soap_valid_socket(soap_bind(&soap, NULL, port, 16))) {
        snprintf(error, sizeof(error), "bind HTTP port %u failed (soap=%d errno=%d)",
                 (unsigned int)port, soap.error, soap.errnum);
        report_started(on_started, started_context, 0, error);
        soap_done(&soap);
        return -1;
    }
    report_started(on_started, started_context, 1, "");
    /* Timed accept makes stop() bounded even when there are no HTTP clients. */
    soap.accept_timeout = 1;
    while (!stop_requested(should_stop, stop_context)) {
        if (!soap_valid_socket(soap_accept(&soap))) {
            if (soap.errnum != 0 && !stop_requested(should_stop, stop_context))
                fprintf(stderr, "OnvifSoapService: HTTP accept error soap=%d errno=%d\n", soap.error, soap.errnum);
            continue;
        }
        const int serve_result = soap_serve(&soap);
        /*
         * HTTP Digest 的第一包本来就应返回 401 challenge。它不是服务端故障，不能
         * 每次客户端认证都刷一条 SOAP fault；解析/业务等其他 SOAP 错误仍完整保留。
         */
        if (serve_result != SOAP_OK && soap.error != 401)
            soap_print_fault(&soap, stderr);
        soap_destroy(&soap);
        soap_end(&soap);
        soap.user = (void *)config;
    }
    soap_done(&soap);
    return 0;
}

/**
 * @brief 运行 ONVIF WS-Discovery 服务的主循环
 *
 * 监听 UDP 多播组，响应其他客户端发起的设备发现探测，并在启动时主动广播 Hello 报文。
 * 该函数会阻塞当前线程直到被请求停止。
 *
 * @param config 服务配置参数
 * @param should_stop 检查是否需要停止的回调
 * @param stop_context 停止回调上下文
 * @param on_started 服务启动结果的回调
 * @param started_context 启动结果回调上下文
 * @return 成功返回 0，失败返回 -1
 */
int onvif_soap_run_discovery_service(const OnvifSoapServiceConfig *config,
                                     OnvifSoapStopRequestedFn should_stop, void *stop_context,
                                     OnvifSoapStartedFn on_started, void *started_context)
{
    struct soap soap;
    struct ip_mreq membership;
    char error[128];
    if (config == NULL) {
        report_started(on_started, started_context, 0, "invalid WS-Discovery configuration");
        return -1;
    }
    soap_init1(&soap, SOAP_IO_UDP | SOAP_XML_TREE);
    soap.user = (void *)config;
    /*
     * WS-Discovery 固定使用公共组播端口 3702。快速网络换址重启时允许 socket 及时
     * 重绑；Linux 上也提升与其他同样正确设置 REUSEADDR 的组播监听者共存的兼容性。
     * 这不绕过真正独占该端口的进程，soap_bind 失败仍会如实上报。
     */
    soap.bind_flags = SO_REUSEADDR;
    if (soap_register_plugin(&soap, soap_wsa) != SOAP_OK ||
        !soap_valid_socket(soap_bind(&soap, NULL, ONVIF_WSDD_PORT, 16))) {
        snprintf(error, sizeof(error), "bind WS-Discovery UDP %d failed (soap=%d errno=%d)",
                 ONVIF_WSDD_PORT, soap.error, soap.errnum);
        report_started(on_started, started_context, 0, error);
        soap_done(&soap);
        return -1;
    }
    memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr.s_addr = inet_addr(ONVIF_WSDD_MULTICAST_ADDRESS);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(soap.master, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
        snprintf(error, sizeof(error), "join WS-Discovery multicast failed (errno=%d)", errno);
        report_started(on_started, started_context, 0, error);
        soap_done(&soap);
        return -1;
    }
    report_started(on_started, started_context, 1, "");
    /* ONVIF Target Service 上线时应主动 Hello。部分管理工具的刷新界面只监听
       Hello，或因 Windows 防火墙过滤了它自己发出的 Probe；主动公告能兼容这类实现。 */
    if (soap_wsdd_Hello(&soap, SOAP_WSDD_ADHOC,
                        "soap.udp://" ONVIF_WSDD_MULTICAST_ADDRESS ":3702",
                        soap_wsa_rand_uuid(&soap), NULL, config->endpointReference,
                        ONVIF_DISCOVERY_TYPES, config->scopes, NULL,
                        config->deviceServiceUrl, 1) != SOAP_OK) {
        fprintf(stderr, "OnvifSoapService: send Hello failed soap=%d errno=%d\n", soap.error, soap.errnum);
    } else {
        fprintf(stderr, "OnvifSoapService: sent Hello endpoint=%s xaddr=%s\n",
                config->endpointReference, config->deviceServiceUrl);
    }
    while (!stop_requested(should_stop, stop_context)) {
        const int result = soap_wsdd_listen(&soap, -200000);
        if (result != SOAP_OK && !stop_requested(should_stop, stop_context) && soap.errnum != 0)
            fprintf(stderr, "OnvifSoapService: WS-Discovery receive error soap=%d errno=%d\n", soap.error, soap.errnum);
    }
    soap_destroy(&soap);
    soap_end(&soap);
    soap_done(&soap);
    return 0;
}
