# live555

这个目录是 MultiCamRenderer 使用的 live555 第三方依赖，不包含项目自己的
RTSP 封装代码。

- `include/`：live555 的四组公开头文件。
- `lib/aarch64/`：为 RK aarch64 SDK 编译的静态库：`liveMedia`、`groupsock`、
  `BasicUsageEnvironment`、`UsageEnvironment`。

项目封装的拉流客户端源码位于 `drm/live555/`。构建 RTSP + MPP 学习 demo 时使用
`drm/wsl-build-rtsp-mpp-demo.sh`；它只引用本目录的 live555 头文件和静态库，不再
依赖另一份 live555 工程或 SDK 内的 live555 动态库。
