#pragma once

/*
 * gSOAP needs global C entry points for generated ONVIF operations.  This is
 * the small C boundary hidden behind the C++ OnvifServer; application code
 * should include OnvifServer.hpp instead of this header.
 */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ONVIF_SOAP_URL_CAPACITY = 256,       /* SOAP 服务 URL 的最大字符数 */
    ONVIF_SOAP_TEXT_CAPACITY = 96,       /* 文本字符串的最大字符数 */
    ONVIF_SOAP_ENDPOINT_CAPACITY = 128,  /* 标识设备的端点引用 (UUID 等) 长度 */
    ONVIF_SOAP_SCOPES_CAPACITY = 384,    /* 范围信息 (Scopes) 的最大长度 */
    ONVIF_SOAP_USERNAME_CAPACITY = 64,   /* ONVIF/RTSP 共用账号用户名最大长度 */
    ONVIF_SOAP_PASSWORD_CAPACITY = 128,  /* ONVIF/RTSP 共用账号密码最大长度 */
    ONVIF_SOAP_REALM_CAPACITY = 96,      /* HTTP Digest 认证域最大长度 */
};

/* 定义 ONVIF SOAP 服务的各项基础配置 */
typedef struct OnvifSoapServiceConfig {
    char deviceServiceUrl[ONVIF_SOAP_URL_CAPACITY];  /* 设备服务暴露的 URL */
    char mediaServiceUrl[ONVIF_SOAP_URL_CAPACITY];   /* Media1 服务暴露的 URL */
    char media2ServiceUrl[ONVIF_SOAP_URL_CAPACITY];  /* Media2 服务暴露的 URL */
    char mainRtspUrl[ONVIF_SOAP_URL_CAPACITY];       /* 对应主码流的真实 RTSP URL */
    char subRtspUrl[ONVIF_SOAP_URL_CAPACITY];        /* 对应子码流的真实 RTSP URL */
    char endpointReference[ONVIF_SOAP_ENDPOINT_CAPACITY]; /* 设备的端点引用 (UUID) */
    char manufacturer[ONVIF_SOAP_TEXT_CAPACITY];     /* 制造商名称 */
    char model[ONVIF_SOAP_TEXT_CAPACITY];            /* 设备型号 */
    char firmwareVersion[ONVIF_SOAP_TEXT_CAPACITY];  /* 固件版本 */
    char serialNumber[ONVIF_SOAP_TEXT_CAPACITY];     /* 序列号 */
    char hardwareId[ONVIF_SOAP_TEXT_CAPACITY];       /* 硬件 ID */
    /* Space-separated ONVIF scope URIs, shared by GetScopes and Discovery. */
    /* 空格分隔的 ONVIF 范围 URI 字符串，供 GetScopes 和 WS-Discovery 共享 */
    char scopes[ONVIF_SOAP_SCOPES_CAPACITY];
    /* username/password 同时为空时显式关闭认证；只设置其一视为非法配置。 */
    char username[ONVIF_SOAP_USERNAME_CAPACITY];
    char password[ONVIF_SOAP_PASSWORD_CAPACITY];
    char authenticationRealm[ONVIF_SOAP_REALM_CAPACITY];
} OnvifSoapServiceConfig;

/*
 * 停止请求的回调函数定义
 * 返回非 0 表示外部请求服务终止，返回 0 表示继续运行。
 */
typedef int (*OnvifSoapStopRequestedFn)(void *context);

/*
 * 服务启动完成事件的回调函数定义
 * success 为非 0 意味着端口绑定和监听成功；失败时 errorText 包含报错原因。
 */
typedef void (*OnvifSoapStartedFn)(void *context, int success, const char *errorText);

/* These calls block until stopRequested returns non-zero. They report whether
 * the listening socket was successfully created through onStarted. */
/*
 * 启动设备服务的 HTTP/SOAP 监听死循环。
 * 该函数会一直阻塞，直到 stopRequested 返回非 0。通过 onStarted 回调通知是否成功创建了监听套接字。
 * @param config SOAP 服务配置结构体
 * @param port 需监听的本地端口
 * @param stopRequested 检查是否应停止运行的钩子函数
 * @param stopContext 传给 stopRequested 的用户上下文参数
 * @param onStarted 启动结果异步上报的回调
 * @param startedContext 传给 onStarted 的用户上下文参数
 * @return 错误码或退出状态
 */
int onvif_soap_run_device_service(const OnvifSoapServiceConfig *config,
                                  unsigned short port,
                                  OnvifSoapStopRequestedFn stopRequested,
                                  void *stopContext,
                                  OnvifSoapStartedFn onStarted,
                                  void *startedContext);

/*
 * 启动 WS-Discovery 组播发现服务的 UDP 监听循环。
 * 该函数同样阻塞执行，直到 stopRequested 返回非 0，并在启动时调用 onStarted。
 * @param config SOAP 服务配置结构体
 * @param stopRequested 检查是否应停止运行的钩子函数
 * @param stopContext 传给 stopRequested 的用户上下文参数
 * @param onStarted 启动结果异步上报的回调
 * @param startedContext 传给 onStarted 的用户上下文参数
 * @return 错误码或退出状态
 */
int onvif_soap_run_discovery_service(const OnvifSoapServiceConfig *config,
                                     OnvifSoapStopRequestedFn stopRequested,
                                     void *stopContext,
                                     OnvifSoapStartedFn onStarted,
                                     void *startedContext);

#ifdef __cplusplus
}
#endif
