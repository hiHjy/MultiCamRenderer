# MPP 编码参数笔记

这份笔记记录当前 `MppEncoder` 封装里和 H264/H265 编码质量最相关的参数理解。它不是最终调参结论，而是后续做 IPC / RTSP / NVR 码流配置时的基准。

当前项目目标是：先保证编码链路稳定、参数语义清楚，再逐步做画质和码率细调。

## 1. 当前编码器配置本质

当前底层 C 封装大致是：

```cpp
rc:mode       = MPP_ENC_RC_MODE_CBR
fps           = enc->fps
gop           = enc->gop
bps_target    = enc->bps
bps_max       = enc->bps * 17 / 16
bps_min       = enc->bps * 15 / 16

qp_init       = -1
qp_min        = 10
qp_max        = 48
qp_min_i      = 10
qp_max_i      = 48
qp_ip         = 2

header_mode   = MPP_ENC_HEADER_MODE_EACH_IDR
```

H264 额外开启：

```cpp
h264:profile  = 100
h264:level    = 40
h264:cabac_en = 1
h264:trans8x8 = 1
```

这表示 H264 使用 High Profile、CABAC 和 8x8 transform。对 RK3568 硬件编码来说，这些配置有利于压缩效率，除非要兼容非常老的解码端，否则可以保留。

## 2. CBR 的含义

`MPP_ENC_RC_MODE_CBR` 是恒定码率模式。它的目标是让输出平均码率尽量接近：

```cpp
rc:bps_target
```

所以如果 H264 和 H265 都设置成 `1500000`，两者文件大小通常会接近。H265 的优势不是在同样 CBR 目标下自动变小，而是在同样码率下画质更好，或者在更低码率下达到接近 H264 的画质。

当前 CBR 边界：

```cpp
bps_max = bps * 17 / 16
bps_min = bps * 15 / 16
```

也就是目标码率上下约 6.25%。这个范围比较窄，适合网络传输、固定带宽、NVR 录像这类需要码率可控的场景。

如果后续更重视复杂场景画质，可以考虑增加 VBR / AVBR 配置入口；但当前第一版继续 CBR 是合理的。

## 3. H264 和 H265 码率档位

当前经验结论：

```text
H265 同等主观画质下，码率可以比 H264 低约 30%~40%。
```

项目里已经加入 `MppBitratePreset`：

```cpp
enum class MppBitratePreset {
    Low,
    Medium,
    High,
    VeryHigh,
};
```

当 `MppEncoderConfig::bitrate > 0` 时，使用外部传入的精确码率。

当 `bitrate == 0` 时，按分辨率、fps、codec 和档位自动估算码率：

```text
base = width * height * fps / 8

H264 Low      = base * 2 / 3
H264 Medium   = base
H264 High     = base * 3 / 2
H264 VeryHigh = base * 2

H265 Low      = H264 Low * 65%
H265 Medium   = H264 Medium * 65%
H265 High     = H264 High * 65%
H265 VeryHigh = H264 VeryHigh * 65%
```

当前建议档位理解：

```text
Low:
  带宽紧张，画质可适当牺牲。

Medium:
  默认档，适合作为 UI 和 demo 的通用配置。

High:
  更重视画质，适合录像或本地网络。

VeryHigh:
  测试、画质优先、带宽不敏感。
```

以 `640x480@30fps` 为例：

```text
base = 640 * 480 * 30 / 8 = 1,152,000 bit/s

H264 Low:      768,000 bit/s
H264 Medium: 1,152,000 bit/s
H264 High:   1,728,000 bit/s
H264 VeryHigh: 2,304,000 bit/s

H265 Low:      499,200 bit/s
H265 Medium:   748,800 bit/s
H265 High:   1,123,200 bit/s
H265 VeryHigh: 1,497,600 bit/s
```

用户实测里，`H265 900k` 与 `H264 1500k` 的主观画质接近，而 `H264 900k` 明显更糊，这符合 H265 更高压缩效率的预期。

## 4. GOP 的取舍

`rc:gop` 表示关键帧间隔，单位是帧。

```text
30fps, gop=30:
  约每 1 秒一个 IDR。

30fps, gop=60:
  约每 2 秒一个 IDR。

30fps, gop=15:
  约每 0.5 秒一个 IDR。
```

GOP 越小：

```text
优点:
  新客户端接入更快看到画面。
  网络丢包后恢复更快。
  拖动/seek 更方便。

缺点:
  I 帧更多，码率开销更大。
  同等码率下 P 帧可用空间更少，压缩效率下降。
```

GOP 越大：

```text
优点:
  压缩效率更高。
  同等码率下画质可能更稳。

缺点:
  中途接入等待 IDR 时间更长。
  丢包后恢复慢。
```

当前建议：

```text
实时预览 / 低延迟:
  gop = fps

普通 IPC 推流 / NVR 录像:
  gop = fps * 2

强调省码率:
  gop = fps * 3 或 fps * 4
```

编码器已经支持 `requestKeyFrame()`，后续 RTSP 客户端丢参考帧或新接入时，可以通过控制通道请求下一帧 IDR。

## 5. QP 当前先不主动调整

QP 是量化参数：

```text
QP 小:
  画质好，数据大。

QP 大:
  画质差，数据小。
```

当前范围：

```cpp
qp_init  = -1
qp_min   = 10
qp_max   = 48
qp_min_i = 10
qp_max_i = 48
qp_ip    = 2
```

`qp_init = -1` 表示让 MPP 码率控制算法自己决定初始 QP。当前先不主动调整 QP，避免过早引入画质、码率和复杂场景之间的三方矛盾。

后续如果出现复杂场景下画面太糊，可以考虑收紧：

```cpp
qp_max = 45;
```

或者：

```cpp
qp_max = 42;
```

但代价是复杂场景下可能更难严格守住目标码率。这个需要结合真实摄像头、动画测试片、网络带宽和录像文件大小一起测。

## 6. Header Mode

当前使用：

```cpp
MPP_ENC_HEADER_MODE_EACH_IDR
```

含义是每个 IDR 前输出参数集。

H264 对应：

```text
SPS / PPS
```

H265 对应：

```text
VPS / SPS / PPS
```

这对裸 Annex-B、RTSP、网络传输、中途开始解码非常重要。客户端只要等到下一个 IDR，就能拿到参数集并开始解码。这个配置建议长期保留。

## 7. Stride 不能乱填

`prep:*` 描述输入图像内存布局：

```cpp
prep:width
prep:height
prep:hor_stride
prep:ver_stride
prep:format
```

`width/height` 是真实画面尺寸，`hor_stride/ver_stride` 是内存布局。它们不一定总相等。

例如：

```text
width  = 1919
stride = 1920
```

或者硬件对齐后：

```text
width  = 1920
stride = 2048
```

这里填错会导致花屏、绿边、颜色错位、编码失败等问题。后续所有 V4L2 / RGA / MPP 链路都必须按真实 stride 传递。

## 8. 后续待验证

后续建议用同一段动态动画素材分别测试：

```text
H264 Medium / High
H265 Medium / High
H264 1.5Mbps
H265 0.9Mbps
GOP = fps
GOP = fps * 2
GOP = fps * 4
```

重点观察：

```text
1. 复杂运动时是否糊。
2. I 帧大小是否过大。
3. GOP 变长后恢复速度能否接受。
4. 同档位 H265 是否明显省码率。
5. 裸流和封装 MP4 后 ffplay / ffprobe 表现是否一致。
```

当前策略先保持克制：只区分 H264/H265 默认码率，不动 QP，不引入 VBR/AVBR，等真实 RTSP/IPC 场景跑起来后再细调。
