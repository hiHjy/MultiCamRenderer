# webrtc-audio-processing（含 Abseil）

Google WebRTC 的独立音频处理模块（APM），供 `core/audio` 的 `AudioApm` 使用。

当前只用它的 **AGC2 + 降噪 + 高通**；AEC 尚未接入，`audio_apm_process_reverse()`
是为此预留的接口。

- 来源：`https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing`
- 版本：**2.1**（tag `v2.1`）
- 依赖：**abseil-cpp 20240722.0**（`github.com/abseil/abseil-cpp`）
- 许可证：均为 BSD-3-Clause / Apache-2.0，原始许可证文件见各自上游源码包

> 2.1 只发布 Meson 构建，且 AGC1（`gain_controller1`）已被 AGC2（`gain_controller2`）
> 取代。升级前的 0.3.1 只有 AGC1，没有降噪模块。

## 目录

```text
include/   上游头文件的传递闭包（600+ 个里只保留实际用到的 50 个）
lib/aarch64-rk3568/libmcr_webrtc_apm.a    # RK3568 Buildroot toolchain
lib/aarch64-rv1126b/libmcr_webrtc_apm.a   # RV1126B SDK toolchain
```

**Abseil 已合并进同一个静态库。** 不用 abseil 自带的 CMake，是因为
`add_subdirectory` 会产出 100 个静态库，两个架构加起来 200 个 `.a`，收纳和分发都很碎。

`include/` 里的文件按上游相对路径保留（`api/...`、`rtc_base/...`、`absl/...`），
因此只需要一个 include 根目录就能满足两种 `#include` 写法。

源码不在仓库里，见 `~/third_party-src/`；重新生成用 `tools/build-third-party.sh`
（该脚本会重算头文件闭包，所以以后 include 了新的上游头，重跑即可自动带上）。
