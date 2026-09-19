# 已知问题

本文档汇总项目当前**尚未解决**的问题，作为收口清单。

约定：

- 问题修掉之后从本文档删除，结论挪进对应的专题笔记或 `DEVELOPMENT_LOG.md`
- 每条问题写清：**现象 → 证据 → 根因 → 归属 → 下一步**
- 根因在驱动/SDK 侧的，**不用应用层代码规避**，只记录并等驱动侧处理

---

## 索引

| # | 问题 | 影响面 | 归属 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | [RV1126B 播放只支持双声道](#1-rv1126b-播放通路只支持双声道) | RV1126B 全部音频播放 | 驱动 | 已定位，未修复 |
| 2 | [RK3568 播放 XRUN 风暴](#2-rk3568-播放-xrun-风暴) | RK3568 播放链路 | 待定位 | 排查中 |
| 3 | [RV1126B codec 控件被关流重置](#3-rv1126b-的-codec-控件会被关流和上电流程重置) | RV1126B 采集/播放 | 驱动 | 已规避 |
| 4 | [`Power Amplifier` 控件让 amixer/alsactl abort](#4-power-amplifier-控件会让-amixer--alsactl-直接-abort) | 板端调试工具 | 驱动 | 已规避 |
| — | [附：`speaker-test` 计时异常](#附一个还没解释的现象) | 未确认 | 未查证 | 观察中 |

---

## 1. RV1126B 播放通路只支持双声道

**影响**：RV1126B 上所有单声道音频播放都是静音（`aplay`、`speaker-test`、应用播放链路）。
**归属**：驱动 / 机器驱动（Rockchip SDK），**不是应用该绕的坑**。
**状态**：已定位到具体代码，未修复。
**涉及**：`sound/soc/codecs/rk_dsm.c`、`sound/soc/codecs/rk3506_codec.c`、`sound/soc/rockchip/rockchip_sai.c`

### 现象

```text
aplay -D plughw:0,0 -v generated.wav                  -> channels : 1    静音
aplay -D plughw:0,0 -v generated-long-48k-stereo.wav  -> channels : 2    有声
speaker-test -D plughw:0,0 -c 1 -t sine -f 1000 -l 1  -> 静音
speaker-test -D plughw:0,0 -c 2 -t sine -f 1000 -l 1  -> 有声
```

同一个 `generated.wav` 在 RK3568 上播放正常，所以不是文件本身或 ALSA 工具的问题。

### 根因

板端声卡是 `rockchip,rv1126b-acodec`（card id `rockchiprv1126b`），由
`rockchip,multicodecs-card` 组织，CPU DAI 为 `sai2`：

```text
rockchip,codec = <&audio_codec_pmu>, <&audio_codec>, <&acdcdig_dsm>;
```

三个 codec 的声道声明：

| 文件 | 角色 | 声道声明 |
| --- | --- | --- |
| `codecs/rk3506_codec.c` | 麦克风 ADC（`audio_codec` / `audio_codec_pmu`） | **只有 `.capture` 1~2ch，没有 `.playback`** |
| `codecs/rk_dsm.c` | 喇叭功放（`acdcdig_dsm`） | `.playback` **2~2** |
| `rockchip/rockchip_sai.c` | CPU DAI（`sai2`，TDM 控制器） | `.playback` 1~512 |

这条链路上唯一能出声的 DAI 是 DSM 功放，而它物理上就是双声道时隙。

但 **DSM 的 2 声道约束没有传到 ALSA 层**。板端 `hw:0,0` 报的是：

```text
CHANNELS: [1 512]
```

512 来自 `rockchip_sai.c:1407`（SAI 的 TDM 能力），ASoC 没能把它和 DSM 的 2~2 求成交集。
后果是 ALSA 的 plug 层认为单声道属于「设备原生支持」，**不做 upmix**，单声道样本被原样送进驱动。

而 `rk_dsm_hw_params()` 从头到尾没有读过 `params_channels()`，它只按采样率配时钟、按格式配位宽，
I2S 接收固定按双时隙配置 —— 单声道数据进了双时隙帧，结果是静音。

### 下一步

三个可选方向，都不改应用：

1. 等 Rockchip 修：让 DSM 的声道约束正确暴露到 PCM，或在 `rk_dsm_hw_params()` 里按
   `params_channels()` 配置
2. 板级 ALSA 配置：在 `asound.conf` 的 plug 层显式约束为双声道
3. 应用侧如需播放单声道素材，由**调用方**自行上混成双声道再交给播放层，而不是让
   `AudioPlayback` 偷偷改声道数

> **决定：不在 `AudioPlayback` 里加「一律请求双声道」这类迁就逻辑。**
> `audio_playback_config_init()` 保持 1 声道默认值，`audio_playback_open_auto()` 依旧按调用方
> 请求的声道数协商。曾经试改过，已回退（见 `DEVELOPMENT_LOG.md` 2026-09-19）。

---

## 2. RK3568 播放 XRUN 风暴

**影响**：`audio_opus_playback_demo` 在 RK3568 上无法正常播放，声音几乎出不来。
**归属**：待定位（设备与参数已排除，疑在 demo 的播放路径）。
**状态**：排查中。用户已要求暂时搁置。

### 现象

播放 4.94 秒的 Opus 文件：

```text
247 个包 / 246 次 XRUN / 829ms 跑完        （正常应约 4940ms）
```

XRUN 是 ALSA 的缓冲区跑穿。播放方向是 underrun：DMA 把缓冲里的数据读完了，应用还没送来新的；
采集方向是 overrun：应用取得太慢，新数据覆盖旧数据。两个方向都返回 `-EPIPE`（`-32`）。

写入循环里的调试输出呈**严格奇偶交替**：

```text
write#0 ret=960  state=RUNNING
write#1 ret=-32  state=XRUN
write#2 ret=960  state=RUNNING
write#3 ret=-32  state=XRUN
```

之所以是「跑完」而不是「卡住」，是因为现有代码在 `-EPIPE` 分支里调 `snd_pcm_prepare()` 把缓冲区
整个清空后立刻重试，缓冲永远填不满，`snd_pcm_writei` 也就永远不阻塞。

### 已排除

同一块板、同一组 ALSA 参数（period 960 / buffer 3840）：

| 测试 | XRUN | 耗时（5 秒音频） |
| --- | --- | --- |
| 最小程序 `alsapb` 走 `plughw:1,0` | **0** | 5000.2 ms ✅ |
| 最小程序 `alsapb` 走 `default` | 2 | 4970 ms ✅ |
| **`audio_opus_playback_demo`** | **246** | **829 ms** ❌ |

设备和参数都没问题，问题出在 demo 的播放路径里。

### 下一步

用同一份 Opus 文件跑 `alsapb` 复现，再逐步收敛到 `AudioPlayback` / `AudioDecoder` 的
具体分支。重点看：

- `-EPIPE` 分支里 `snd_pcm_prepare()` 的返回值被忽略、且立刻 `continue` 重试
- 固定的 960 帧写入粒度与设备实际周期是否匹配

---

## 3. RV1126B 的 codec 控件会被关流和上电流程重置

**影响**：采集电平异常、播放静音，实测掉 **17dB**。
**归属**：驱动。不是麦克风或硬件问题（早期「麦克风偏置没打开」的结论已被推翻）。
**状态**：已在脚本里规避。

每次采集/播放结束后，ACodec 的数字增益和开关会被复位：

```text
Digital Gain       -> 0     (-95dB)
PGA Gain           -> 16    （原本 31）
ADC Switch         -> off
DAC Digital Volume -> 0     （静音）
```

**规避方式**：板端脚本（`/root/audio-test/apm-listen.sh`）每轮开录前都重设一遍。正式应用接入时
同样需要在打开流之后、开始读写之前重设这些控件。

---

## 4. `Power Amplifier` 控件会让 amixer / alsactl 直接 abort

**影响**：板端无法用标准 ALSA 工具列/改控件。
**归属**：驱动。
**状态**：已规避。

DSM 的 `Power Amplifier` 是 `SOC_ENUM_EXT`，读 TLV 时返回 `EINVAL`，`amixer` 和 `alsactl` 会直接
中止（整个工具挂掉，不是单条报错）。

**规避方式**：单独写了 `ctldump`（只做 `SNDRV_CTL_IOCTL_ELEM_*` 的裸调用）供板端列控件和读写。

另外这个开关是 **DAPM 托管**的，写进去会立刻被拉回 `off`，只在播放期间为 `on` —— 所以
「`spk switch` 打不开」是正常现象，不是故障。

---

## 附：一个还没解释的现象

`speaker-test` 打印的 `Time per period` 和理论周期时长对不上：

| 配置 | 理论周期 | 实测 |
| --- | --- | --- |
| 单声道，period 4096 帧 @48kHz | 85.3 ms | **1.408 s** |
| 双声道，period 2048 帧 @48kHz | 42.7 ms | **5.888 s** |

两个数都远大于理论值，但双声道确实听得到正常声音。可能是 `speaker-test` 自身的计时口径问题，
也可能是驱动侧时钟或周期上报有偏差，**尚未查证**。

---

## 相关文档

- `docs/audio_astats_and_apm_notes.md` —— ffmpeg `astats` 各项指标含义、APM 各级对指标的影响、
  已解决的调音结论（不加增益 + 降噪 High + 高通）
- `DEVELOPMENT_LOG.md` —— 按日期的开发记录，已解决问题的完整过程在这里
