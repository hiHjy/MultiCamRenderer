/**
 * @file OnvifDeviceClient.c
 * @brief ONVIF 设备客户端实现
 *
 * 作为 ONVIF 发现后的下一步，连接到具体设备以获取 Media Service URL 和 RTSP 流地址。
 * 提供获取媒体服务地址以及通过 Media1 和 Media2 接口获取流描述文件（Profile）和流 URI 的功能。
 */

#include "OnvifDeviceClient.h"

#include "httpda.h"
#include "onvifStub.h"

#include <stdio.h>
#include <string.h>

/** ONVIF Media v1 服务命名空间 */
#define ONVIF_MEDIA_NAMESPACE "http://www.onvif.org/ver10/media/wsdl"
/** ONVIF Media v2 服务命名空间 */
#define ONVIF_MEDIA2_NAMESPACE "http://www.onvif.org/ver20/media/wsdl"

/* GetProfiles 后只需保留 token/name；避免在栈上为每条临时记录预留整份输出结构。 */
/**
 * @brief ONVIF Profile 的简要描述结构
 *
 * 仅用于缓存从 GetProfiles 响应中提取的标识信息。
 */
typedef struct OnvifProfileDescription {
    char token[ONVIF_CLIENT_TEXT_CAPACITY]; /**< Profile 唯一令牌标识 */
    char name[ONVIF_CLIENT_TEXT_CAPACITY];  /**< Profile 的人类可读名称 */
} OnvifProfileDescription;

/**
 * @brief 安全的字符串拷贝函数
 *
 * @param destination 目标缓冲区
 * @param capacity    目标缓冲区大小
 * @param source      源字符串
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
 * @brief 将 gSOAP 错误信息格式化并保存到目标缓冲区中
 *
 * @param destination 目标缓冲区
 * @param capacity    目标缓冲区大小
 * @param operation   发生错误的操作名称
 * @param soap        gSOAP 运行环境句柄
 */
static void set_soap_error(char* destination, size_t capacity, const char* operation,
                           struct soap* soap)
{
    const char* fault = soap != NULL ? soap_fault_string(soap) : NULL;
    (void)snprintf(destination, capacity, "%s failed (gSOAP=%d)%s%s", operation,
                   soap != NULL ? soap->error : SOAP_EOF,
                   fault != NULL && *fault != '\0' ? ": " : "", fault != NULL ? fault : "");
}

/**
 * @brief 初始化 gSOAP 运行环境
 *
 * @param soap gSOAP 运行环境句柄
 */
static void init_soap(struct soap* soap)
{
    soap_init1(soap, SOAP_XML_TREE);
    /*
     * http_da 会从 401 响应中保存 realm/nonce，再在第二次同一 SOAP 调用中生成
     * Authorization: Digest。每个公开 API 都是同步且独占一个 soap context，
     * 不会跨线程共享插件状态。
     */
    (void)soap_register_plugin(soap, http_da);
    // 设置超时时间（秒）
    soap->connect_timeout = 3;
    soap->send_timeout = 5;
    soap->recv_timeout = 5;
}

static int has_digest_credentials(const OnvifHttpDigestCredentials* credentials)
{
    return credentials != NULL && credentials->username != NULL && credentials->password != NULL &&
           credentials->username[0] != '\0';
}

/* 将 HTTP 401 转成上层可直接驱动 UI 的状态码，不让 C++/Qt 层解析英文错误文本。 */
static OnvifClientResult soap_failure_result(const struct soap* soap, int digest_retried)
{
    if (soap != NULL && soap->error == 401) {
        return digest_retried ? ONVIF_CLIENT_AUTHENTICATION_FAILED
                              : ONVIF_CLIENT_AUTHENTICATION_REQUIRED;
    }
    return ONVIF_CLIENT_REQUEST_FAILED;
}

/*
 * ONVIF HTTP Digest 的标准握手是“先请求一次拿到 401 challenge，再带 Digest 重试”。
 *
 * 空密码也是合法 Digest 凭证（HA1 会计算 username:realm:""），因此只要用户名非空、
 * password 指针存在就应重试。每个封装的 SOAP 调用只执行一次业务操作，故凭证仅需在重试期间保存；函数结束前
 * 必须 release，避免把账号密码残留在 gSOAP context 中。callExpression 依赖局部变量
 * soap，且必须是一次完整 soap_call_* 调用。
 */
#define ONVIF_CALL_WITH_OPTIONAL_DIGEST(callExpression, credentials, callResult, digestRetried) \
    do {                                                                                        \
        struct http_da_info digestInfo;                                                        \
        int digestSaved = 0;                                                                    \
        (digestRetried) = 0;                                                                    \
        memset(&digestInfo, 0, sizeof(digestInfo));                                             \
        (callResult) = (callExpression);                                                       \
        if ((callResult) != SOAP_OK && soap.error == 401 &&                                    \
            has_digest_credentials((credentials)) && soap.authrealm != NULL) {                 \
            http_da_save(&soap, &digestInfo, soap.authrealm, (credentials)->username,         \
                         (credentials)->password);                                              \
            digestSaved = 1;                                                                    \
            (digestRetried) = 1;                                                                \
            (callResult) = (callExpression);                                                   \
        }                                                                                       \
        if (digestSaved)                                                                        \
            http_da_release(&soap, &digestInfo);                                               \
    } while (0)

/**
 * @brief 清理并销毁 gSOAP 运行环境
 *
 * @param soap gSOAP 运行环境句柄
 */
static void cleanup_soap(struct soap* soap)
{
    soap_destroy(soap);
    soap_end(soap);
    soap_done(soap);
}

/**
 * @brief 获取设备的 Media 服务 URL
 *
 * 通过调用 Device 服务的 GetServices 接口，获取设备支持的 Media1 和 Media2 服务地址。
 *
 * @param deviceServiceUrl 设备的 Device 服务端点地址 (XAddr)
 * @param result           用于存储返回的 Media 服务 URL 和可能的错误信息的结构体
 * @return int             成功返回 0，失败返回 -1 并设置 result->error
 */
OnvifClientResult onvif_device_get_media_service_urls(
    const char* deviceServiceUrl, const OnvifHttpDigestCredentials* credentials,
    OnvifMediaServiceUrls* result)
{
    struct soap soap;
    struct _tds__GetServices request;
    struct _tds__GetServicesResponse response;
    int index;
    int callResult;
    int digestRetried;
    if (result == NULL)
        return ONVIF_CLIENT_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (deviceServiceUrl == NULL || *deviceServiceUrl == '\0') {
        copy_text(result->error, sizeof(result->error), "Device Service XAddr is empty");
        return ONVIF_CLIENT_INVALID_ARGUMENT;
    }

    init_soap(&soap);
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    // 不需要包含能力信息以减小响应体积
    request.IncludeCapability = xsd__boolean__false_;

    // 调用 Device.GetServices
    ONVIF_CALL_WITH_OPTIONAL_DIGEST(
        soap_call___tds__GetServices(&soap, deviceServiceUrl, NULL, &request, &response),
        credentials, callResult, digestRetried);
    if (callResult != SOAP_OK) {
        const OnvifClientResult failureResult = soap_failure_result(&soap, digestRetried);
        set_soap_error(result->error, sizeof(result->error), "Device.GetServices", &soap);
        cleanup_soap(&soap);
        return failureResult;
    }
    if (response.__sizeService > 0 && response.Service == NULL) {
        copy_text(result->error, sizeof(result->error), "Device.GetServices returned an invalid service list");
        cleanup_soap(&soap);
        return ONVIF_CLIENT_REQUEST_FAILED;
    }

    // 遍历服务列表，寻找 Media1 和 Media2 服务的 XAddr
    for (index = 0; index < response.__sizeService; ++index) {
        const struct tds__Service* service = &response.Service[index];
        if (service->Namespace == NULL)
            continue;
        if (strcmp(service->Namespace, ONVIF_MEDIA2_NAMESPACE) == 0)
            copy_text(result->media2ServiceUrl, sizeof(result->media2ServiceUrl), service->XAddr);
        else if (strcmp(service->Namespace, ONVIF_MEDIA_NAMESPACE) == 0)
            copy_text(result->mediaServiceUrl, sizeof(result->mediaServiceUrl), service->XAddr);
    }
    cleanup_soap(&soap);
    if (result->mediaServiceUrl[0] == '\0' && result->media2ServiceUrl[0] == '\0') {
        copy_text(result->error, sizeof(result->error), "Device.GetServices returned no Media Service");
        return ONVIF_CLIENT_REQUEST_FAILED;
    }
    return ONVIF_CLIENT_OK;
}

/**
 * @brief 通过 Media2 接口获取指定 Profile 的 RTSP 流地址并追加到结果中
 *
 * 调用 Media2.GetStreamUri 接口获取单个 profile 的 RTSP URL。
 *
 * @param endpoint 媒体服务地址
 * @param token    Profile 唯一标识
 * @param name     Profile 的人类可读名称
 * @param result   存储结果的数组包装结构
 * @return int     成功返回 0，发生严重错误（网络或协议错误）返回 -1
 */
static OnvifClientResult append_media2_profile(
    const char* endpoint, const char* token, const char* name,
    const OnvifHttpDigestCredentials* credentials, OnvifDeviceStreamProfiles* result)
{
    struct soap soap;
    struct _tr2__GetStreamUri request;
    struct _tr2__GetStreamUriResponse response;
    OnvifDeviceStreamProfile* output;
    int callResult;
    int digestRetried;
    if (result->count >= ONVIF_CLIENT_MAX_PROFILES)
        return ONVIF_CLIENT_OK;
    init_soap(&soap);
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    request.Protocol = "RtspUnicast";
    request.ProfileToken = (char*)token;

    // 调用 Media2.GetStreamUri
    ONVIF_CALL_WITH_OPTIONAL_DIGEST(
        soap_call___tr2__GetStreamUri(&soap, endpoint, NULL, &request, &response), credentials,
        callResult, digestRetried);
    if (callResult != SOAP_OK) {
        const OnvifClientResult failureResult = soap_failure_result(&soap, digestRetried);
        set_soap_error(result->error, sizeof(result->error), "Media2.GetStreamUri", &soap);
        cleanup_soap(&soap);
        return failureResult;
    }
    if (response.Uri == NULL || response.Uri[0] == '\0') {
        copy_text(result->error, sizeof(result->error), "Media2.GetStreamUri returned an empty RTSP URI");
        cleanup_soap(&soap);
        return ONVIF_CLIENT_REQUEST_FAILED;
    }

    // 成功获取，追加到结果列表
    output = &result->profiles[result->count++];
    copy_text(output->token, sizeof(output->token), token);
    copy_text(output->name, sizeof(output->name), name);
    copy_text(output->streamUri, sizeof(output->streamUri), response.Uri);
    output->fromMedia2 = 1;
    cleanup_soap(&soap);
    return ONVIF_CLIENT_OK;
}

/**
 * @brief 使用 Media2 接口获取所有流媒体配置信息
 *
 * 调用 Media2.GetProfiles 接口获取设备所有的 Profile 列表，
 * 然后逐个调用 Media2.GetStreamUri 获取流地址。
 *
 * @param mediaServiceUrl 设备的 Media2 服务端点地址 (XAddr)
 * @param result          用于存储返回的流描述列表的结构体
 * @return int            成功获取到至少一个流返回 0，否则返回 -1
 */
OnvifClientResult onvif_media2_get_stream_profiles(
    const char* mediaServiceUrl, const OnvifHttpDigestCredentials* credentials,
    OnvifDeviceStreamProfiles* result)
{
    struct soap soap;
    struct _tr2__GetProfiles request;
    struct _tr2__GetProfilesResponse response;
    OnvifProfileDescription descriptions[ONVIF_CLIENT_MAX_PROFILES];
    unsigned int descriptionCount = 0;
    int index;
    int callResult;
    int digestRetried;
    if (result == NULL)
        return ONVIF_CLIENT_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (mediaServiceUrl == NULL || *mediaServiceUrl == '\0') {
        copy_text(result->error, sizeof(result->error), "Media2 service URL is empty");
        return ONVIF_CLIENT_INVALID_ARGUMENT;
    }
    init_soap(&soap);
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));

    // 调用 Media2.GetProfiles
    ONVIF_CALL_WITH_OPTIONAL_DIGEST(
        soap_call___tr2__GetProfiles(&soap, mediaServiceUrl, NULL, &request, &response), credentials,
        callResult, digestRetried);
    if (callResult != SOAP_OK) {
        const OnvifClientResult failureResult = soap_failure_result(&soap, digestRetried);
        set_soap_error(result->error, sizeof(result->error), "Media2.GetProfiles", &soap);
        cleanup_soap(&soap);
        return failureResult;
    }
    if (response.__sizeProfiles > 0 && response.Profiles == NULL) {
        copy_text(result->error, sizeof(result->error), "Media2.GetProfiles returned an invalid profile list");
        cleanup_soap(&soap);
        return ONVIF_CLIENT_REQUEST_FAILED;
    }
    memset(descriptions, 0, sizeof(descriptions));

    // 提取 Profile 的 token 和 name 缓存到本地数组，避免 gSOAP 清理后数据丢失
    for (index = 0; index < response.__sizeProfiles && descriptionCount < ONVIF_CLIENT_MAX_PROFILES; ++index) {
        const struct tr2__MediaProfile* profile = &response.Profiles[index];
        if (profile->token == NULL || *profile->token == '\0')
            continue;
        copy_text(descriptions[descriptionCount].token, sizeof(descriptions[descriptionCount].token), profile->token);
        copy_text(descriptions[descriptionCount].name, sizeof(descriptions[descriptionCount].name), profile->Name);
        ++descriptionCount;
    }
    cleanup_soap(&soap);

    // 逐个获取每个 Profile 对应的 Stream URI
    for (index = 0; index < (int)descriptionCount; ++index) {
        const OnvifClientResult profileResult = append_media2_profile(
            mediaServiceUrl, descriptions[index].token, descriptions[index].name, credentials, result);
        if (profileResult != ONVIF_CLIENT_OK)
            return profileResult;
    }

    if (result->count == 0 && result->error[0] == '\0')
        copy_text(result->error, sizeof(result->error), "Media2.GetProfiles returned no usable stream profiles");
    return result->count > 0 ? ONVIF_CLIENT_OK : ONVIF_CLIENT_REQUEST_FAILED;
}

/**
 * @brief 通过 Media1 接口获取指定 Profile 的 RTSP 流地址并追加到结果中
 *
 * 调用 Media.GetStreamUri 接口获取流地址。
 * 注意：Media v1 接口的 GetStreamUri 必须在请求中包含 StreamSetup 信息。
 *
 * @param endpoint 媒体服务地址
 * @param token    Profile 唯一标识
 * @param name     Profile 的人类可读名称
 * @param result   存储结果的数组包装结构
 * @return int     成功返回 0，发生严重错误（网络或协议错误）返回 -1
 */
static OnvifClientResult append_media1_profile(
    const char* endpoint, const char* token, const char* name,
    const OnvifHttpDigestCredentials* credentials, OnvifDeviceStreamProfiles* result)
{
    struct soap soap;
    struct tt__Transport transport;
    struct tt__StreamSetup setup;
    struct _trt__GetStreamUri request;
    struct _trt__GetStreamUriResponse response;
    OnvifDeviceStreamProfile* output;
    int callResult;
    int digestRetried;
    if (result->count >= ONVIF_CLIENT_MAX_PROFILES)
        return ONVIF_CLIENT_OK;
    init_soap(&soap);
    memset(&transport, 0, sizeof(transport));
    memset(&setup, 0, sizeof(setup));
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));

    // Media1 必须通过 StreamSetup 指定所需的流类型（如 RTP_Unicast + RTSP）
    transport.Protocol = tt__TransportProtocol__RTSP;
    setup.Stream = tt__StreamType__RTP_Unicast;
    setup.Transport = &transport;
    request.StreamSetup = &setup;
    request.ProfileToken = (char*)token;

    // 调用 Media.GetStreamUri
    ONVIF_CALL_WITH_OPTIONAL_DIGEST(
        soap_call___trt__GetStreamUri(&soap, endpoint, NULL, &request, &response), credentials,
        callResult, digestRetried);
    if (callResult != SOAP_OK) {
        const OnvifClientResult failureResult = soap_failure_result(&soap, digestRetried);
        set_soap_error(result->error, sizeof(result->error), "Media.GetStreamUri", &soap);
        cleanup_soap(&soap);
        return failureResult;
    }
    if (response.MediaUri == NULL || response.MediaUri->Uri == NULL || response.MediaUri->Uri[0] == '\0') {
        copy_text(result->error, sizeof(result->error), "Media.GetStreamUri returned an empty RTSP URI");
        cleanup_soap(&soap);
        return ONVIF_CLIENT_REQUEST_FAILED;
    }

    // 成功获取，追加到结果列表
    output = &result->profiles[result->count++];
    copy_text(output->token, sizeof(output->token), token);
    copy_text(output->name, sizeof(output->name), name);
    copy_text(output->streamUri, sizeof(output->streamUri), response.MediaUri != NULL ? response.MediaUri->Uri : NULL);
    output->fromMedia2 = 0;
    cleanup_soap(&soap);
    return ONVIF_CLIENT_OK;
}

/**
 * @brief 使用 Media1 接口获取所有流媒体配置信息
 *
 * 调用 Media.GetProfiles 接口获取设备所有的 Profile 列表，
 * 然后逐个调用 Media.GetStreamUri 获取流地址。
 *
 * @param mediaServiceUrl 设备的 Media1 服务端点地址 (XAddr)
 * @param result          用于存储返回的流描述列表的结构体
 * @return int            成功获取到至少一个流返回 0，否则返回 -1
 */
OnvifClientResult onvif_media1_get_stream_profiles(
    const char* mediaServiceUrl, const OnvifHttpDigestCredentials* credentials,
    OnvifDeviceStreamProfiles* result)
{
    struct soap soap;
    struct _trt__GetProfiles request;
    struct _trt__GetProfilesResponse response;
    OnvifProfileDescription descriptions[ONVIF_CLIENT_MAX_PROFILES];
    unsigned int descriptionCount = 0;
    int index;
    int callResult;
    int digestRetried;
    if (result == NULL)
        return ONVIF_CLIENT_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    if (mediaServiceUrl == NULL || *mediaServiceUrl == '\0') {
        copy_text(result->error, sizeof(result->error), "Media service URL is empty");
        return ONVIF_CLIENT_INVALID_ARGUMENT;
    }
    init_soap(&soap);
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));

    // 调用 Media.GetProfiles
    ONVIF_CALL_WITH_OPTIONAL_DIGEST(
        soap_call___trt__GetProfiles(&soap, mediaServiceUrl, NULL, &request, &response), credentials,
        callResult, digestRetried);
    if (callResult != SOAP_OK) {
        const OnvifClientResult failureResult = soap_failure_result(&soap, digestRetried);
        set_soap_error(result->error, sizeof(result->error), "Media.GetProfiles", &soap);
        cleanup_soap(&soap);
        return failureResult;
    }
    if (response.__sizeProfiles > 0 && response.Profiles == NULL) {
        copy_text(result->error, sizeof(result->error), "Media.GetProfiles returned an invalid profile list");
        cleanup_soap(&soap);
        return ONVIF_CLIENT_REQUEST_FAILED;
    }
    memset(descriptions, 0, sizeof(descriptions));

    // 提取 Profile 的 token 和 name 缓存到本地数组，避免 gSOAP 清理后数据丢失
    for (index = 0; index < response.__sizeProfiles && descriptionCount < ONVIF_CLIENT_MAX_PROFILES; ++index) {
        const struct tt__Profile* profile = &response.Profiles[index];
        if (profile->token == NULL || *profile->token == '\0')
            continue;
        copy_text(descriptions[descriptionCount].token, sizeof(descriptions[descriptionCount].token), profile->token);
        copy_text(descriptions[descriptionCount].name, sizeof(descriptions[descriptionCount].name), profile->Name);
        ++descriptionCount;
    }
    cleanup_soap(&soap);

    // 逐个获取每个 Profile 对应的 Stream URI
    for (index = 0; index < (int)descriptionCount; ++index) {
        const OnvifClientResult profileResult = append_media1_profile(
            mediaServiceUrl, descriptions[index].token, descriptions[index].name, credentials, result);
        if (profileResult != ONVIF_CLIENT_OK)
            return profileResult;
    }

    if (result->count == 0 && result->error[0] == '\0')
        copy_text(result->error, sizeof(result->error), "Media.GetProfiles returned no usable stream profiles");
    return result->count > 0 ? ONVIF_CLIENT_OK : ONVIF_CLIENT_REQUEST_FAILED;
}
