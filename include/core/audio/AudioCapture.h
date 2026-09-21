#ifndef MCR_AUDIO_CAPTURE_H
#define MCR_AUDIO_CAPTURE_H

/*
 * ALSA 采集：自动发现设备 -> 打开 PCM -> 起一个采集线程 -> 按周期回调 PCM。
 *
 * 这一层只负责"把麦克风数据按稳定节奏取出来"，不做编码、不做 APM。上层要接
 * APM / Opus / RTSP 都在回调里做。
 *
 * 线程模型：open/close 由调用线程执行；start 之后数据由内部采集线程推送，
 * **回调运行在采集线程上**，所以回调里不能阻塞，也不能直接调 audio_capture_stop()
 * 之外的会等待线程退出的接口。
 */

#include "AudioTypes.h"

#include <alsa/asoundlib.h>
#ifndef __cplusplus
#include <pthread.h>
#include <stdatomic.h>
#endif
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioDeviceInfo {
    char alsaName[64];   /* ALSA 设备名，如 "plughw:1,0"。open 时用它 */
    char cardId[64];     /* 声卡短 id，如 "rockchiprk809"。用于日志和识别 */
    char cardName[128];  /* 声卡全称，如 "rockchip-rk809" */
    char pcmName[128];   /* PCM 设备名，如 "dailink-multicodecs ..." */
    int cardIndex;       /* 声卡号；未发现时 -1 */
    int deviceIndex;     /* 声卡内的设备号；未发现时 -1 */
} AudioDeviceInfo;

/*
 * 从 ALSA 物理设备中选择第一个可采集 PCM，不依赖易变化的固定 card 编号。
 *
 * 优先选「同时具备 playback 的全双工 PCM」——那通常是板载 codec；找不到才回退到
 * 首个仅采集设备。这样在插入 USB 摄像头（常枚举为 card0）时不会误选到它。
 *
 * 成功返回 0，无可用设备返回 -ENODEV，参数非法返回 -EINVAL。
 */
int audio_capture_discover_default_device(AudioDeviceInfo *deviceInfo);

/*
 * PCM 数据回调。在采集线程上被调用，每收到一个周期（periodFrames 帧）触发一次。
 *
 * frame->data 指向内部缓冲区，**回调返回后即失效**，异步使用必须自己拷贝。
 * 返回 0 继续采集；返回非 0 的语义当前未定义（实现里忽略返回值）。
 */
typedef void (*AudioPcmCallback)(const AudioPcmFrame *frame, void *userData);

typedef struct AudioCaptureConfig {
    /* 期望的采样率/声道/样本格式。注意是"期望"：ALSA 走的是 *_near 协商，
       实际值可能不同，必须从 AudioCapture::actualFormat 读。 */
    uint32_t requestedSampleRate;
    uint16_t requestedChannels;
    AudioSampleFormat requestedFormat;
    /* 期望的周期帧数，决定回调频率和延迟。默认 480 帧 = 48kHz 下 10ms。
       APM 要求帧数是 sampleRate/100 的整数倍（10ms 对齐），10ms 正好满足。 */
    snd_pcm_uframes_t requestedPeriodFrames;
} AudioCaptureConfig;

#ifndef __cplusplus
typedef struct AudioCapture {
    /* ALSA 采集 PCM 句柄。open_auto 打开并 prepare；采集线程用它 readi；
       stop 用 snd_pcm_drop 打断阻塞中的 readi；close 负责释放。 */
    snd_pcm_t *pcmHandle;

    /* 采集线程本体。start 创建，stop 里 pthread_join。 */
    pthread_t thread;

    /* 停止请求。stop() 置 true，采集线程每轮循环开头检查。
       置位后还会调用 snd_pcm_drop() 把阻塞在 snd_pcm_readi 的线程唤醒 ——
       只靠标志位是叫不醒阻塞在驱动里的 readi 的。 */
    atomic_bool stopRequested;

    /* 运行状态标志。start 置 true、线程退出置 false。
       注意：目前只写不读（外部没有消费者），保留作为状态观察点。 */
    atomic_bool running;

    /* 数据回调及其上下文，由 audio_capture_set_callback 设置；NULL 表示不推送。 */
    AudioPcmCallback callback;
    void *callbackUserData;

    /* 自动发现到的设备信息，open_auto 里填充。 */
    AudioDeviceInfo deviceInfo;

    /* 实际协商成功的格式。**下游（APM/编码器）必须用这个值，不能用请求值** ——
       requested* 只是期望，ALSA 可能给出不同的采样率或声道数。 */
    AudioPcmFormat actualFormat;

    /* 实际协商成功的周期帧数。每次 readi 读这么多帧，也是 buffer 的容量依据。 */
    snd_pcm_uframes_t periodFrames;

    /* 采集缓冲区，大小 = periodFrames * channels * sizeof(int16_t)。
       回调收到的 frame.data 就指向这里，所以回调返回后数据失效。 */
    uint8_t *buffer;
    size_t bufferBytes;

    /* 下一块数据的微秒时间戳（CLOCK_MONOTONIC 基准，非墙钟）。
       首次成功读取时以"当前时间 - 一块时长"起算，之后按实际帧数递增；
       因此流内单调，但不代表真实采集时刻（没有对齐到驱动时间戳）。

	   note:CLOCK_MONOTONIC和std::chrono::steady_clock都是单调时钟，指的是
	   通常从系统启动那一刻开始计时（但标准不保证）

	  */
    uint64_t nextTimestampUs;

    /* 帧数转微秒时除不尽的**余数**，留到下次一起算，避免整数除法累积漂移
       （48kHz 下 960 帧 = 20000us 整除，但 44.1kHz 之类就除不尽）。 */
    uint64_t timestampRemainder;

    /* 时间戳时钟是否已基准化。首次成功读取时置 1。
       **XRUN / recover 之后会清 0**，让下一块重新对齐到当前时间 —— 因为数据流
       的连续性已经断了，继续累加会和真实时间越差越远。 */
    int clockInitialized;

    /* 采集线程是否已创建。start 用它防止重复启动（已启动返回 -EBUSY），
       stop 用它判断是否需要 join。 */
    int threadCreated;
} AudioCapture;
#else
/* C++ 侧不直接依赖 C11 atomic/pthread 字段；必须经下方 API 操作这个 C 对象。 */
typedef struct AudioCapture AudioCapture;
#endif

/* 填入默认配置：48000Hz / 1ch / S16_LE / period 480 帧（10ms）。 */
void audio_capture_config_init(AudioCaptureConfig *config);

/* 设置 PCM 回调及其上下文。可在 start 前调用。
   实现里没有加锁，所以应当在 start 之前设好，不要在采集过程中改。 */
void audio_capture_set_callback(AudioCapture *capture, AudioPcmCallback callback, void *userData);

/*
 * 自动发现设备并打开、配置采集 PCM（但不启动采集）。
 * config 传 NULL 表示用默认配置。
 *
 * 失败返回负 errno；成功返回 0，此时 actualFormat / periodFrames 已填好。
 */
int audio_capture_open_auto(AudioCapture *capture, const AudioCaptureConfig *config);

/*
 * 给 C++ Pipeline 使用的对象生命周期与查询接口。C 调用方仍可像以前一样在栈上定义
 * AudioCapture；C++ 不可见内部 C11 atomic 布局，使用 create/destroy 取得不透明指针。
 */
AudioCapture *audio_capture_create(void);
void audio_capture_destroy(AudioCapture *capture);
AudioPcmFormat audio_capture_actual_format(const AudioCapture *capture);
snd_pcm_uframes_t audio_capture_period_frames(const AudioCapture *capture);

/* 启动采集线程。要求已 open_auto 成功。重复启动返回 -EBUSY。 */
int audio_capture_start(AudioCapture *capture);

/* 停止并 join 采集线程。可重复调用；未启动时直接返回。 */
void audio_capture_stop(AudioCapture *capture);

/* 停止采集、释放 PCM 句柄和缓冲区，并把整个结构体清零。
   清零意味着 close 之后不能再读 actualFormat 等字段。 */
void audio_capture_close(AudioCapture *capture);

#ifdef __cplusplus
}
#endif

#endif
