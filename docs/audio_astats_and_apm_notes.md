# ffmpeg astats 指标含义与 APM 的交互

这份笔记记录两件事：

1. `ffmpeg -af astats` 每一项到底在量什么（含几个**名字会误导人**的项）
2. 板端音频链路调完 WebRTC APM 之后，每一项会被 APM 的哪一级影响、该怎么看

对应代码：

```text
src/core/audio/AudioApm.cpp          APM 的 C 边界（AGC2 + 降噪 + 高通）
include/core/audio/AudioApm.h        配置项
demo/AudioCaptureApmPcmDemo.c        「采集 -> APM -> 写 WAV」，可同时落原始 PCM 做 A/B
tools/build-third-party.sh           重建 webrtc-audio-processing 2.1
```

用法：

```bash
ffmpeg -i test.wav -af "atrim=start=0.5,asetpts=PTS-STARTPTS,astats=reset=0" -f null -
```

**先 `atrim` 掐掉开头**，原因见第 1 节。

---

## 1. 先看一个真实的坑：整段数字会被开头的启动爆音带偏

同一段 5 秒采集，只差「有没有掐掉开头 0.5 秒」：

| 指标 | 整段 | 掐掉 0.5s 后 | 说明 |
|---|---|---|---|
| Peak level dB | **-0.63** | -28.89 | 差了 **28 dB** |
| RMS level dB | **-30.05** | -49.29 | 差了 **19 dB** |
| Crest factor | **29.58** | 10.47 | 差了 19 dB |
| Dynamic range | 95.70 | 67.44 | |
| Noise floor dB | -62.70 | -62.70 | 没变（见 2.4） |

整段那个 -0.63 dBFS 的「峰值」**不是声音**，是每次开录时模拟耦合电容充电的阶跃。
它把 peak/RMS/crest 全部拉高，让人误以为信号很饱满。

**结论：分析任何板端录音前先 `atrim` 掉前 0.3~0.5 秒。** 这个爆音是 codec 固有的，
不是麦克风的问题，也不该计入指标。

---

## 2. 逐项含义

分析窗口 `length` 默认 0.05 s，实际统计窗口是它的 5 倍 —— **0.25 s**
（`tc_samples = 5 * time_constant * sample_rate`，48 kHz 下 = 12000 样本）。
带「RMS peak/trough」的项是**逐窗口**算完再取极值，其余是整段累计。

### 2.1 电平类

| 项 | 含义 | 注意 |
|---|---|---|
| `Peak level dB` | 整段最大 \|样本\|，`20*log10(peak/32768)` | 会被单个瞬态带偏，配合 `Peak count` 看 |
| `RMS level dB` | 整段 RMS | 最常用的「响度」参考 |
| `RMS peak dB` | 各 0.25s 窗口 RMS 的**最大值** | 反映「最响的那 0.25 秒」 |
| `RMS trough dB` | 各 0.25s 窗口 RMS 的**最小值** | **实际底噪参考，比 `Noise floor dB` 可靠** |
| `Min/Max level` | 样本极值（原始整数值） | 看有没有撞到 ±32768 |
| `DC offset` | 样本均值，**归一化到满幅的比例**（不是原始值） | 实测：直流 1000 → `0.030519` = 1000/32768。非零说明有直流偏置 |
| `Bit depth` | 位深 | 不要和 `Dynamic range` 混 |

### 2.2 波形形状类

| 项 | 含义 | 注意 |
|---|---|---|
| `Crest factor` | **峰值 / RMS，线性比不是 dB** | 实测正弦 = `1.414`(√2)、直流 = `1.0`。**折算 dB 要自己 `20*log10()`**：连续发声段典型 10~15 dB（线性 3.2~5.6），含静音间隙的整段录音 20~30 dB（线性 10~32） |
| `Flat factor` | 波形「平顶」程度，**dB** | `0` = 干净；**`> 0` 表示有削顶**（实测限幅正弦 = 24.6）；`-inf` = 整段恒定（纯直流或静音，没有任何「段」结束过） |
| `Peak count` | 取到整段峰值/谷值的**样本个数** | 实测 1 kHz 正弦 2 秒 = 4000 = 2×1000×2 ✓。正常语音只有个位数；**很大说明削顶** |
| `Zero crossings` / rate | 过零次数 / 每样本过零率 | 换算频率：`rate × sample_rate / 2`。实测 0.008663 × 48000 / 2 ≈ **208 Hz**，说明能量集中在低频 |
| `Min/Mean/Max/RMS difference` | **相邻样本差分**的统计量 | 削顶会表现为 `Max difference` 骤降、`RMS difference` 变小 |

### 2.3 两个「名不副实」的项

**`Dynamic range`** —— 不是音频意义上的动态范围。源码公式是：

```text
20 * log10( 2 * max(|min|, |max|) / 最小非零|样本| )
```

即**峰值与「最小的非零样本」之比**，实质是这段信号用到了多少个有效位。
实测验证：直流 1000 → `6.02`（= 20log10(2)）✓；1 kHz 正弦幅度 10000 → `23.71` ✓。

所以整段 95.70 几乎顶到 16-bit 理论极限（96.3 dB），**不是因为信号动态大，
而是因为开头爆音造出一个大峰值、而安静段全是 ±1 样本**。

**`Noise floor dB`** —— 实测**不能当底噪用**。它是 ffmpeg 用一个 8192 桶直方图
统计 \|样本\| 得到的值，在**周期性强信号上会退化到接近峰值**：

| 测试信号 | Peak | Noise floor |
|---|---|---|
| 0/30000 交替 | -0.77 | **-0.77** |
| ±500 方波 | -36.33 | **-36.33** |
| 6000~10000 偏置正弦 | -10.31 | **-10.31** |
| 纯静音 | -inf | -inf |

**只有含安静段的真实录音它才反映底噪**（那一栏的 `-62.70` 还算合理，但也比
`RMS trough dB: -72.04` 高了 9 dB）。

> **实践建议：判断底噪看 `RMS trough dB`，不要看 `Noise floor dB`。**
> `Noise floor count` 是 ffmpeg 内部的窗口计数（实测前四种完全确定性信号都恒为
> 84001），没有分析价值。

---

## 3. APM 各级对指标的影响

当前默认配置（`audio_apm_config_init()`）：

```c
enableAdaptiveDigitalGain = 0;   // AGC2 自适应：实测在低信噪比采集上会喘振，关
fixedDigitalGainDb        = 0;   // 固定增益：默认不加
enableNoiseSuppression    = 1;   // 降噪：High
enableHighPassFilter      = 1;   // 高通：干掉 50Hz 工频
enableTransientSuppression= 0;
```

| APM 级 | 影响哪些指标 | 怎么表现 |
|---|---|---|
| **固定数字增益** `fixed_digital.gain_db` | `Peak`、`RMS`、`RMS peak/trough` **整体平移** | 确定性的：设 30 dB 各项就 +30 dB。但**底噪同步 +30 dB**，`Crest factor` 不变 |
| **AGC2 自适应** `adaptive_digital` | `RMS peak` 与 `RMS trough` 的**间距收窄** | 响的压、轻的抬。副作用是听感「喘振」，且**需要 VAD 检测到语音才推增益** —— 安静环境里几乎不动 |
| **降噪** `noise_suppression` | `RMS level` **下降**、`RMS trough` 明显下降、`Crest factor` 上升 | 移除稳态噪声能量。**注意：加了降噪之后 `gain=xx` 这类「输出/输入」比值不再等于增益**，因为分子被 NS 削掉了 |
| **高通** `high_pass_filter` | `DC offset` → 0、`Zero crossings rate` 上升、`Dynamic range` 上升 | 实测工频哼声是 50 Hz 的 -69.6 dBFS 尖峰，高通后 `RMS trough` 会明显下移 |
| **limiter**（AGC2 内置，始终启用） | `Peak` 被钉在目标附近、`Flat factor` 若 `> 0` 说明限幅过头 | 固定增益设 40 dB 时实测峰值 +0.17 dBFS、削顶样本 0.067%，此时应回调 |

### 3.1 「增益拉满反而更差」的机理

板端实测：`fixedDigitalGainDb = 30` 时各路指标都比原始好（峰值 -6.3 dBFS、零削顶、
`RMS trough` 下移），但**再往上到 40 就过载**。

**关键结论：底噪问题不能靠调增益解决。** 为了补电平把增益拉高 30 dB，底噪也同步
放大 30 dB，听感是「响但糊」。正确做法是**不加增益 + 开降噪**，实测听感明显优于原始采集。

---

## 4. 本项目实测参照值

RV1126B，48 kHz 单声道，安静室内（PGA 已置最大）：

| 场景 | Peak | RMS | RMS trough | 备注 |
|---|---|---|---|---|
| 原始采集（无 APM） | -39.3 | -52.2 | — | 掐掉爆音后 |
| APM，固定增益 30 dB | -6.3 | -20.5 | — | **零削顶**，甜点值 |
| APM，固定增益 40 dB | +0.2 | -10.0 | — | 削顶 0.067%，过载 |
| APM，无增益 + 降噪 High | — | — | 明显低于原始 | 听感最好的组合 |

语音质量判据（掐掉爆音后）：

- `Crest factor` **10~32**（即 20~30 dB）→ 含静音间隙的正常录音；若只有 3~5.6
  说明整段都是连续发声、没有安静段
- `Flat factor` **= 0** 且 `Peak count` 个位数 → 没有削顶
- `RMS trough` 与 `RMS peak` **相差 30~45 dB** → 语音间隙清晰，信噪比够用
  （实测样本：`RMS peak -31.42` / `RMS trough -72.04`，相差 40.6 dB）

---

## 5. 踩过的坑汇总

1. **不掐开头会得到完全错误的结论** —— 启动爆音能把 peak 抬高 28 dB
2. **`Noise floor dB` 不能当底噪** —— 周期性信号上它等于峰值，看 `RMS trough dB`
3. **`Crest factor` 是线性比** —— 1.414 是 √2，不是 1.414 dB
4. **`DC offset` 是归一化值** —— 1000 的直流显示 0.03，不是 1000
5. **`Dynamic range` 不是动态范围** —— 是「峰值 / 最小非零样本」，约等于有效位深
6. **`Flat factor = -inf`** 不是错误 —— 表示整段恒定（直流/静音），没有「段」结束过
7. **Abseil 与 webrtc 的 C++17 要求** —— RK3568 的 GCC 10.4 默认 C++14，见
   `third_party/webrtc-audio-processing/README.md`
