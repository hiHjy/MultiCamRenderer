#pragma once

/* Private C bridge: gSOAP's wsddapi requires process-wide C callback symbols.
 * Keep that constraint here, then let the public C++ client expose std::string
 * and std::vector without leaking gSOAP types into application code.
 *
 * 私有 C 语言桥接层：由于 gSOAP 的 wsddapi 要求在进程全局暴露 C 风格的回调函数符号，
 * 因此在这里保持这一约束，随后让公共的 C++ 客户端暴露 std::string 和 std::vector，
 * 从而避免将 gSOAP 的内部类型泄露到应用程序代码中。本文件定义了底层 Probe 操作的数据结构。
 */

#ifdef __cplusplus
extern "C" {
#endif

// 定义各种字符串的最大容量以及最大匹配设备数
enum {
    ONVIF_WSDD_MAX_MATCHES = 32,      // 最大支持的设备匹配数量
    ONVIF_WSDD_EPR_CAPACITY = 128,    // 终端节点引用(EndpointReference)的字符串最大容量
    ONVIF_WSDD_URL_CAPACITY = 256,    // 设备服务 URL 的字符串最大容量
    ONVIF_WSDD_TYPES_CAPACITY = 128,  // 支持的设备类型(Types)的字符串最大容量
    ONVIF_WSDD_SCOPES_CAPACITY = 512, // 设备作用域(Scopes)的字符串最大容量
    ONVIF_WSDD_ERROR_CAPACITY = 160,  // 错误信息字符串的最大容量
};

// 表示单个 WS-Discovery 探测匹配结果
typedef struct OnvifWsddProbeMatch {
    char endpointReference[ONVIF_WSDD_EPR_CAPACITY]; // 设备的唯一标识符（终端节点引用地址）
    char deviceServiceUrl[ONVIF_WSDD_URL_CAPACITY];  // 设备的设备服务 XAddr（连接 URL）
    char types[ONVIF_WSDD_TYPES_CAPACITY];           // 设备支持的 ONVIF 服务类型列表
    char scopes[ONVIF_WSDD_SCOPES_CAPACITY];         // 设备所在的逻辑作用域列表
} OnvifWsddProbeMatch;

// 表示一次 WS-Discovery 探测操作的整体返回结果
typedef struct OnvifWsddProbeResult {
    unsigned int count;                                  // 成功探测到的设备数量
    OnvifWsddProbeMatch matches[ONVIF_WSDD_MAX_MATCHES]; // 保存所有匹配设备的数组
    char error[ONVIF_WSDD_ERROR_CAPACITY];               // 若探测出错，存放详细错误信息；否则为空
} OnvifWsddProbeResult;

/* Returns 0 when Probe was sent and the listening interval completed (including
 * an ordinary no-device timeout); returns -1 for socket/plugin/protocol errors.
 *
 * 执行 WS-Discovery 探测操作。
 * 当成功发送 Probe 并完成监听间隔时（包括未发现设备的普通超时），返回 0；
 * 当发生套接字、插件或协议错误时，返回 -1。
 *
 * @param timeoutMs 探测等待超时时间（毫秒）
 * @param result    用于接收探测结果和匹配设备列表的输出参数
 */
int onvif_wsdd_probe(unsigned int timeoutMs, OnvifWsddProbeResult* result);

#ifdef __cplusplus
}
#endif
