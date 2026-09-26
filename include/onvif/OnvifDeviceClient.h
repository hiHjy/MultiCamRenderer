/*
 * 此文件定义了 ONVIF 设备客户端的公共 C 接口。
 * 提供了获取 Media Service URL 和流 Profile 的功能。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 客户端文本字符串的最大容量 */
#define ONVIF_CLIENT_TEXT_CAPACITY 512
/* 客户端支持的最大 Profile 数量 */
#define ONVIF_CLIENT_MAX_PROFILES 16

/*
 * ONVIF Device/Media 请求的机器可读结果。
 *
 * 上层绝不能解析 error 文本判断认证状态：发现成功后先匿名读取 Device Service，若
 * 返回 AUTHENTICATION_REQUIRED 就弹出账号密码输入框；用户输入后重试，若仍返回
 * AUTHENTICATION_FAILED 才提示密码错误。其余失败统一交给错误页面/重试策略处理。
 */
typedef enum OnvifClientResult {
    ONVIF_CLIENT_OK = 0,
    ONVIF_CLIENT_INVALID_ARGUMENT = -1,
    ONVIF_CLIENT_AUTHENTICATION_REQUIRED = -2,
    ONVIF_CLIENT_AUTHENTICATION_FAILED = -3,
    ONVIF_CLIENT_REQUEST_FAILED = -4,
} OnvifClientResult;

/* C bridge 的输出全部是值对象，C++ 层无需持有 gSOAP 分配的临时响应内存。 */
/* 表示媒体服务 URL 的结构体 */
typedef struct OnvifMediaServiceUrls {
    char mediaServiceUrl[ONVIF_CLIENT_TEXT_CAPACITY];  /* Media1 服务的 URL */
    char media2ServiceUrl[ONVIF_CLIENT_TEXT_CAPACITY]; /* Media2 服务的 URL */
    char error[ONVIF_CLIENT_TEXT_CAPACITY];            /* 错误信息，为空表示成功 */
} OnvifMediaServiceUrls;

/*
 * ONVIF Device/Media 的 HTTP Digest 凭证。
 *
 * 两个指针只在一次同步 API 调用期间读取，调用方无需长期持有。传 NULL 表示先以
 * 匿名方式请求；这对没有开启认证的第三方 ONVIF 设备仍然兼容。
 */
typedef struct OnvifHttpDigestCredentials {
    const char* username;
    const char* password;
} OnvifHttpDigestCredentials;

/* 表示 ONVIF 设备流 Profile 的结构体 */
typedef struct OnvifDeviceStreamProfile {
    char token[ONVIF_CLIENT_TEXT_CAPACITY];      /* Profile 唯一标识符 (token) */
    char name[ONVIF_CLIENT_TEXT_CAPACITY];       /* Profile 的人类可读名称 */
    char streamUri[ONVIF_CLIENT_TEXT_CAPACITY];  /* 对应的流媒体 URI (如 RTSP) */
    int fromMedia2;                              /* 标识是否从 Media2 获取（1是，0否） */
} OnvifDeviceStreamProfile;

/* 包含多个流 Profile 的集合 */
typedef struct OnvifDeviceStreamProfiles {
    unsigned int count;                                           /* 获取到的 Profile 数量 */
    OnvifDeviceStreamProfile profiles[ONVIF_CLIENT_MAX_PROFILES]; /* Profile 数组 */
    char error[ONVIF_CLIENT_TEXT_CAPACITY];                       /* 错误信息，为空表示成功 */
} OnvifDeviceStreamProfiles;

/*
 * 从设备服务中获取 Media 服务的 URL
 * @param deviceServiceUrl 设备的设备服务 URL
 * @param credentials 可选的 HTTP Digest 账号密码
 * @param result 获取到的媒体服务 URL 结果
 * @return OnvifClientResult；可据 AUTHENTICATION_REQUIRED / AUTHENTICATION_FAILED
 *         决定是否向用户请求或重新请求凭证。
 */
OnvifClientResult onvif_device_get_media_service_urls(
    const char* deviceServiceUrl, const OnvifHttpDigestCredentials* credentials,
    OnvifMediaServiceUrls* result);
/*
 * 通过 Media2 服务获取媒体流 Profile
 * @param mediaServiceUrl Media2 服务的 URL
 * @param credentials 可选的 HTTP Digest 账号密码
 * @param result 获取到的流 Profile 结果集
 * @return OnvifClientResult
 */
OnvifClientResult onvif_media2_get_stream_profiles(
    const char* mediaServiceUrl, const OnvifHttpDigestCredentials* credentials,
    OnvifDeviceStreamProfiles* result);
/*
 * 通过 Media1 服务获取媒体流 Profile
 * @param mediaServiceUrl Media1 服务的 URL
 * @param credentials 可选的 HTTP Digest 账号密码
 * @param result 获取到的流 Profile 结果集
 * @return OnvifClientResult
 */
OnvifClientResult onvif_media1_get_stream_profiles(
    const char* mediaServiceUrl, const OnvifHttpDigestCredentials* credentials,
    OnvifDeviceStreamProfiles* result);

#ifdef __cplusplus
}
#endif
