# HTTP Digest 认证原理 —— 用人话讲

## 你的核心疑问

> "正常流程不应该是先失败一次，然后我手动输入账号密码，然后再保存吗？"

**答案是：这两步确实都有，只是分在不同的层。**

- **"输入账号密码"** → 是在**应用层**做的，调用方传 `OnvifHttpDigestCredentials` 进来
- **"先失败一次再重试"** → 是在**协议层**做的，这是 **HTTP Digest 的标准握手流程**，不是 Bug

## 为什么必须"先失败一次"？

用一个现实类比：

```
你去一个需要门禁卡的大楼：

❌ 错误理解：你直接刷卡 → 成功/失败
✅ 实际流程：
   1. 你先走到门口 → 门禁屏幕显示一个随机数字（nonce）
   2. 你用"卡号 + 这个随机数字"算出一个验证码
   3. 你输入验证码 → 门禁验证通过

关键点：你不能在还没看到随机数字的时候就算验证码！
因为每次随机数字都不一样，这样即使有人偷看了你上次的验证码也没用。
```

HTTP Digest 的逻辑**完全一样**：

```mermaid
sequenceDiagram
    participant C as 你的客户端
    participant S as 对方摄像头

    Note over C: 我有用户名和密码，但不能直接发！
    C->>S: ① 普通请求 GetServices（不带认证）
    S-->>C: ② 401 Unauthorized<br/>realm="MultiCamRenderer ONVIF"<br/>nonce="abc123随机值"
    Note over C: 收到了！用 MD5(用户名:realm:密码:nonce:...) 算哈希
    C->>S: ③ 同样的 GetServices + Authorization: Digest<br/>username="admin", nonce="abc123",<br/>response="计算出的哈希值"
    S-->>C: ④ 200 OK + 正常的响应数据
```

### 四步握手的每一步在代码里对应什么

| 步骤 | 发生了什么 | 代码在哪 |
|---|---|---|
| ① 第一次请求 | `soap_call___tds__GetServices()` 第一次调用 | 宏的第 111 行 `(callResult) = (callExpression);` |
| ② 收到 401 | gSOAP 的 `http_da` 插件自动从响应头里提取 `realm` 和 `nonce` | `soap.error == 401 && soap.authrealm != NULL` |
| ③ 带 Digest 重试 | `http_da_save()` 把密码+nonce 算好，再次调用同一个请求 | 宏的第 114-117 行 |
| ④ 成功 | 服务端验证哈希一致，返回正常数据 | `callResult == SOAP_OK` |

## 你的代码里的宏展开后长这样

```c
// ONVIF_CALL_WITH_OPTIONAL_DIGEST 展开后等价于：

// 第一次调用（不带认证）
callResult = soap_call___tds__GetServices(&soap, url, NULL, &req, &resp);

// 如果收到 401 并且调用方给了用户名密码
if (callResult != SOAP_OK && soap.error == 401 &&
    有用户名密码 && soap.authrealm != NULL) {

    // 用 "密码 + 服务端给的nonce" 计算 Digest
    http_da_save(&soap, &digestInfo, soap.authrealm, "admin", "password123");

    // 第二次调用（自动带上 Authorization: Digest 头）
    callResult = soap_call___tds__GetServices(&soap, url, NULL, &req, &resp);
}

// 用完立即清理，不让密码残留在内存
http_da_release(&soap, &digestInfo);
```

## 为什么不能直接发密码？

```
❌ Basic 认证（不安全）：
   网络包: Authorization: Basic YWRtaW46cGFzc3dvcmQ=  (Base64的admin:password)
   → 随便一个抓包工具就能看到明文密码

✅ Digest 认证（安全）：
   网络包: Authorization: Digest username="admin",
           nonce="abc123", response="7f2b3c4d..."
   → 网络上只有哈希值，看不到密码
   → 每次 nonce 不同，重放旧包也没用
```

哈希计算公式（简化版）：
$$\text{response} = \text{MD5}(\text{MD5}(用户名:realm:密码) : nonce : \text{MD5}(方法:URI))$$

服务端也有密码原文，用同样的公式算一遍，两边结果一致就说明客户端确实知道正确的密码。

## 服务端怎么验证的？

你的服务端有两套认证，用的**同一个用户名密码**：

### 1. ONVIF (HTTP Digest)
在 `OnvifSoapService.c` 的 `onvif_soap_run_device_service()` 里，gSOAP 的 `http_da` 插件自动处理：
- 收到请求 → 检查有没有 `Authorization` 头
- 没有 → 回 401 + nonce
- 有 → 验证哈希 → 通过就调你的 `__tds__GetServices()`

### 2. RTSP (Digest)
在 `Live555RtspServer.cpp` 里，live555 自带的认证：
```cpp
m_authDb = new UserAuthenticationDatabase;
m_authDb->addUserRecord("admin", "password123");
m_server = RTSPServer::createNew(*m_env, m_rtspPort, m_authDb, ...);
```
live555 的 RTSP 服务器内部做完全一样的 Digest 挑战流程，只不过是在 RTSP 协议上而不是 HTTP 上。VLC 连你的 RTSP 流时弹出的密码框，就是 RTSP Digest 挑战触发的。

### 配置来源
```
OnvifServerConfig
  ├─ username = "admin"          ← 应用层设置
  ├─ password = "xxx"            ← 应用层设置
  ├─ authenticationRealm = "MultiCamRenderer ONVIF"
  │
  ↓ 传递到
  │
  ├─ OnvifSoapServiceConfig.username/password  → ONVIF HTTP Digest
  └─ Live555 UserAuthenticationDatabase        → RTSP Digest
```

## 为什么每次 API 调用都要重新走 401？

因为你的代码**每个 API 都新建一个 soap context**：

```
GetServices:  new soap → 401 → Digest 重试 → 成功 → 销毁 soap
GetProfiles:  new soap → 401 → Digest 重试 → 成功 → 销毁 soap
GetStreamUri: new soap → 401 → Digest 重试 → 成功 → 销毁 soap
```

每次 soap context 销毁了，之前的 nonce 就没了。下一次调用又得重新走一遍 401 挑战。

> **这其实是更安全的做法。** 每次都拿新 nonce，完全杜绝了 nonce 重放的可能。代价就是每个操作多一次 HTTP 往返，但在局域网里这个延迟可以忽略不计。

## 你需要手动做什么？

**什么都不需要。** 认证对你来说是透明的：

1. **做客户端**（连别人的摄像头）：调用时传个 `OnvifHttpDigestCredentials` 就行
   ```c
   OnvifHttpDigestCredentials creds = { "admin", "camera123" };
   onvif_device_get_media_service_urls(url, &creds, &result);
   // 内部自动处理 401 → Digest 重试
   ```

2. **做服务端**（让别人连你）：在 `OnvifServerConfig` 里设好用户名密码就行
   ```cpp
   config.username = "admin";
   config.password = "mypassword";
   // gSOAP 和 live555 自动验证所有进来的请求
   ```

3. **不设密码**：`username` 和 `password` 都留空 → 认证自动关闭，匿名访问
