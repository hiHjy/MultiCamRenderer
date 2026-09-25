# live555（统一依赖）

本目录只使用一套未经项目修改的 upstream 源码：`live.2021.05.03`。

- 来源：`https://github.com/lengfeld/live555-unofficial-git-archive`
- 固定 tag：`v2021.05.03-tree`
- 与 RK3568 板子原有 `libliveMedia.so.94` 的版本一致。
- 许可证：LGPL；原始许可证文件见上游源码包。

目录中的 `include/` 是该源码包原样导出的头文件，不能手工修改。
静态库以同一源码分别使用目标 SDK 的工具链构建：

```text
lib/aarch64-rk3568/   # RK3568 Buildroot toolchain
lib/aarch64-rv1126b/  # RV1126B SDK toolchain
```

构建脚本会显式传入 `MCR_LIVE555_TARGET`，因此不会再发生“新头文件配旧静态库”的混搭。

## 端口快速重启

构建静态库时必须定义 `ALLOW_RTSP_SERVER_PORT_REUSE=1`。live555 默认会在
`GenericMediaServer` 创建监听 socket 前关闭 `SO_REUSEADDR`，这样 RTSP 服务刚停止后，
旧 IPv4 连接仍处于 `TIME_WAIT` 时可能无法重新绑定 8554；IPv6 socket 又能单独绑定成功，
最终表现为程序启动成功、却只监听 `:::8554`，IPv4 客户端连接被拒绝。

该宏保留 `SO_REUSEADDR`，用于正常的“停止后立即重启”。它不允许两个存活的 RTSP Server
共同占用同一端口；已有监听者时新服务仍会启动失败。
