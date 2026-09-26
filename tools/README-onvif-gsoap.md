# ONVIF / gSOAP：生成、编译与目录边界

## 先区分两件事

gSOAP 在本项目有两个阶段，不能混为一谈：

```text
WSL 开发主机一次性生成
  ONVIF WSDL/XSD
    -> wsdl2h + soapcpp2
    -> generated/onvif/ 下的 C/H binding

交叉编译最终 IPC 程序
  手写 ONVIF 服务/客户端代码 + gSOAP runtime/plugin + generated binding
    -> ipc_app
```

`wsdl2h`、`soapcpp2` 是 **主机生成工具**，只在 WSL x86_64 上运行；不会被复制到 RK3568 或
RV1126B，也不参与板端运行。板端没有 gSOAP 守护进程或动态库依赖：gSOAP runtime 被静态编进
`ipc_app`。

## 最终编译实际需要什么

| 类别 | 目录/文件 | 用途 | 是否提交 |
| --- | --- | --- | --- |
| gSOAP runtime | `third_party/gsoap/src/stdsoap2.c` | SOAP XML、HTTP、socket 基础运行时 | 是 |
| gSOAP plugins | `third_party/gsoap/plugin/{threads,wsaapi,wsddapi,httpda,smdevp}.c` | 线程、WS-Addressing、WS-Discovery、HTTP Digest | 是 |
| gSOAP headers/import | `third_party/gsoap/include/`、`third_party/gsoap/import/` | 编译 runtime、plugin、生成 binding 所需头文件 | 是 |
| ONVIF 协议定义 | `third_party/onvif-specs/wsdl/` | 重新生成 binding 的 WSDL/XSD 固定快照 | 是 |
| 自动生成 binding | `generated/onvif/ws-discovery/`、`generated/onvif/device/` | SOAP 序列化/反序列化、client/server 路由 | 否，自动生成 |
| 手写协议逻辑 | `include/onvif/`、`src/onvif/` | ONVIF 业务实现、客户端 C API、服务生命周期 | 是 |
| Digest 加密库 | 目标 SDK 的 OpenSSL `libssl`、`libcrypto` | `httpda` 的 HTTP Digest 算法 | SDK 提供 |

实际 source 清单由根目录 `CMakeLists.txt` 的 `mcr_gsoap_wsdd`、
`mcr_onvif_client_gsoap_bindings`、`mcr_onvif_gsoap_bindings` 三个静态库定义。不要手工把
`generated/` 放进 Git：它已在 `.gitignore`，因为它可由项目内固定的 WSDL 与固定版本工具复现。

## 一键重新生成

在项目根目录执行：

```bash
bash tools/generate-onvif-bindings.sh
```

它依次执行：

```text
tools/generate-onvif-wsdd.sh
  -> generated/onvif/ws-discovery/

tools/generate-onvif-device-bindings.sh
  -> generated/onvif/device/
```

适用场景：

- 更新 `third_party/onvif-specs/` 的 WSDL/XSD 快照；
- 更新 gSOAP host generator；
- 改动 `tools/gsoap-typemap.dat`；
- 怀疑本地 `generated/onvif/` 损坏时。

两个底层脚本也可以单独运行，但通常优先使用总脚本。它们依赖项目自带的：

```text
third_party/gsoap/tools/linux-x86_64/wsdl2h
third_party/gsoap/tools/linux-x86_64/soapcpp2
```

因此当前脚本只支持 WSL/Linux x86_64 主机；如果以后在 ARM 主机开发，需要先提供同版本的 ARM host
generator，再改脚本路径。它不是交叉编译器，不能拿板端二进制替代。

> 注意：当前 ONVIF 官方 `onvif.xsd` 通过绝对 URL 引用了少量 OASIS/W3C schema。`wsdl2h` 在重新生成
> Device/Media binding 时会解析这些引用，因此**重新生成阶段当前需要开发主机可联网**。这不影响后续
> `./wsl-build-ipc.sh` 的离线交叉编译；生成结果保留在本地 `generated/` 中即可。若未来需要完全离线
> 重生成，应先将这些上游 schema 固定到 `third_party/onvif-specs/`，并把 schemaLocation 改为本地路径。

## CMake 的自动行为

`generated/` 是忽略目录。第一次 clone 后直接执行：

```bash
./wsl-build-ipc.sh
```

CMake 检测到下列核心文件缺失时，会自动调用相应脚本：

```text
generated/onvif/ws-discovery/wsddC.c
generated/onvif/device/onvifC.c
```

所以**首次构建不需要手动生成**。但 CMake 不会根据 WSDL 的修改时间自动强制重生成；你改过 WSDL、typemap
或 generator 后，应主动执行 `bash tools/generate-onvif-bindings.sh`，然后再构建。

## Digest 的额外要求

ONVIF Device/Media client 与 server 都启用 gSOAP `httpda`，所以需要 OpenSSL。RV1126B 的构建脚本
`wsl-build-ipc.sh` 已传入：

```text
-DMCR_ONVIF_OPENSSL_ROOT=<RV1126B SDK rootfs>
```

根目录 `CMakeLists.txt` 从该位置取得 `include/openssl/`、`lib/libssl.so`、`lib/libcrypto.so`。
RK3568 或 WSL 构建若没有设置此项，则使用系统/对应 SDK 的 `find_path`、`find_library` 查找。

## 生成后必须知道的一点

`generated/onvif/` 里的文件是 gSOAP 机器生成代码，**不要手改**。需要新增 ONVIF 操作时：

1. 在生成的 `onvifStub.h` 找到该操作的固定 C 函数签名；
2. 从 `onvifUnimplementedOps.c` 移除对应的 `SOAP_NO_METHOD` 占位函数；
3. 在 `src/onvif/OnvifSoapService.c` 实现业务函数；
4. 重新构建并用客户端验证。

详见 [`docs/gsoap_architecture.md`](../docs/gsoap_architecture.md)。HTTP Digest 的握手过程见
[`docs/digest_auth_explained.md`](../docs/digest_auth_explained.md)。
