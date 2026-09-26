# Demo 目录导航

每个 demo 都是独立验证某一段链路的工具；CMake target 名与历史保持不变，整理目录
不影响原来的构建和运行命令。

| 目录 | 内容 |
| --- | --- |
| `audio/` | ALSA 采集/播放、AudioPipeline、APM、AAC/Opus、音频压力与诊断工具 |
| `camera/` | V4L2 探测、单路采集、CamManager 自动恢复压力测试 |
| `codec/` | 不经网络的 MPP H264/H265 编码器、解码器验证 |
| `memory/` | DMA-BUF 分配器验证 |
| `rtsp/` | RTSP 拉流、Annex-B 落盘、MPP 解码、AAC 播放/录 WAV、Stream 恢复与管理 |
| `onvif/` | WS-Discovery Target/Probe、Device Service、发现并读取 Profile 的客户端 |
| `osd/` | FreeType 文本 bitmap 和 RGA OSD 合成验证 |
| `rga/` | RGA copy、裁剪、缩放、旋转、格式转换验证 |
| `frame_lease/` | `FrameLease` 生命周期和归还语义验证 |

## 查找原则

- 想验证一个硬件模块，优先从对应目录的“最小 demo”开始，例如 `codec/`、`camera/`。
- 想验证完整数据路径，使用 `rtsp/` 或 `audio/` 的 pipeline demo。
- 所有 demo 的 target 定义集中在根目录 `CMakeLists.txt` 的 `MCR_BUILD_DEMOS` 区域。
