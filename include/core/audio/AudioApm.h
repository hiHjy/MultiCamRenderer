#ifndef MCR_AUDIO_APM_H
#define MCR_AUDIO_APM_H

#include "AudioTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WebRTC APM 的 C 边界（webrtc-audio-processing 2.1，只启用 AGC2）。
 *
 * 音频核心目前仍以 C 为主；这里不让 WebRTC 的 C++ 类型泄露到采集、编码或以后
 * 的 RTSP 模块。一个 AudioApm 对应一路麦克风上行流，只能由该路采集线程调用。
 *
 * 关于 AGC2：2.x 用 gain_controller2 取代了老的 AGC1（gain_controller1）。
 * AGC2 由"自适应数字增益 + 固定数字增益 + limiter"三级组成，能对电平远低于
 * 正常范围的采集信号做几十 dB 的提升，而不是像 AGC1 那样把接近噪声底的输入
 * 判定成噪声后拒绝推增益。因此这里只暴露 AGC2 的参数。
 */
typedef struct AudioApmConfig {
    /* 自适应数字增益开关：自动把信号拉到合适电平，上限 maxGainDb。 */
    int enableAdaptiveDigitalGain;
    /* 自适应增益上限（dB）。AGC2 内部限制为 [0, 50]。 */
    int maxGainDb;
    /* 起始自适应增益（dB）：越大收敛越快，代价是开头几百毫秒会冲一下。 */
    int initialGainDb;
    /* 自适应增益变化速度上限（dB/s）：越大收敛越快，越小越平滑。 */
    int maxGainChangeDbPerSecond;
    /* 目标余量（dB）：留给定值，避免持续顶到 limiter。 */
    int headroomDb;
    /* 自适应增益的输出噪声上限（dBFS，负数）：抑制安静时把底噪一起推上来。 */
    int maxOutputNoiseLevelDbfs;
    /* 固定数字增益（dB）：在自适应之前施加的已知增益，用于补硬件侧的固定衰减，
       AGC 只能慢慢收敛，这一级是立刻生效的。 */
    int fixedDigitalGainDb;

    /*
     * 以下三级用来对付"增益拉高之后底噪跟着变响"的问题。板端采集的信噪比本来
     * 就不宽裕，单靠固定增益会把底噪一起放大，必须配合降噪和高通才可用。
     */

    /* WebRTC 降噪（NS）开关。 */
    int enableNoiseSuppression;
    /* 降噪强度：0=Low 1=Moderate 2=High 3=VeryHigh。 */
    int noiseSuppressionLevel;
    /* 高通滤波开关：滤掉 50Hz 工频哼声和低频隆隆声。 */
    int enableHighPassFilter;
    /* 瞬态抑制开关：压制敲击、键盘一类突发瞬态。 */
    int enableTransientSuppression;
} AudioApmConfig;

/* 用于运行期健康检查的累计电平，不持有任何 PCM 数据。 */
typedef struct AudioApmStatistics {
    uint64_t processedFrames;
    uint64_t processedSamples;
    uint64_t inputAbsoluteSampleSum;
    uint64_t outputAbsoluteSampleSum;
} AudioApmStatistics;

typedef struct AudioApm {
    void *implementation;
    AudioPcmFormat format;
    size_t maximumFramesPerProcess;
    int initialized;
} AudioApm;

void audio_apm_config_init(AudioApmConfig *config);
int audio_apm_open(AudioApm *apm,
                   const AudioApmConfig *config,
                   AudioPcmFormat format,
                   size_t maximumFramesPerProcess);

/*
 * 处理麦克风 PCM，并把处理后的数据交给 output。input/output 的 timestamp 相同；
 * output.data 由 apm 持有，到下一次 processCapture() 或 close() 前有效。
 *
 * APM 固定按 10ms 帧工作。因此输入帧数必须是 sampleRate / 100 的整数倍。
 */
int audio_apm_process_capture(AudioApm *apm,
                              const AudioPcmFrame *input,
                              AudioPcmFrame *output);

/*
 * 为未来回声消除预留：播放到本地扬声器前的 PCM 应送入这里作为 reverse stream。
 * 当前 AGC-only 阶段不启用 AEC，因此该函数只是校验/接收，尚未被播放链调用。
 */
int audio_apm_process_reverse(AudioApm *apm, const AudioPcmFrame *frame);
void audio_apm_get_statistics(const AudioApm *apm, AudioApmStatistics *statistics);
void audio_apm_close(AudioApm *apm);

#ifdef __cplusplus
}
#endif

#endif
