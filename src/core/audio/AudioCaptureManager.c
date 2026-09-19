#include "AudioCaptureManager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void clear_playback_references_locked(AudioCaptureManager *manager) {
    manager->playbackReferenceHead = 0;
    manager->playbackReferenceCount = 0;
}

static void clear_playback_references(AudioCaptureManager *manager) {
    if (manager == NULL || !manager->playbackReferenceMutexInitialized) {
        return;
    }
    pthread_mutex_lock(&manager->playbackReferenceMutex);
    clear_playback_references_locked(manager);
    pthread_mutex_unlock(&manager->playbackReferenceMutex);
}

static void release_playback_reference_storage(AudioCaptureManager *manager) {
    if (manager == NULL) {
        return;
    }
    free(manager->playbackReferenceStorage);
    free(manager->playbackReferenceProcessingBuffer);
    manager->playbackReferenceStorage = NULL;
    manager->playbackReferenceProcessingBuffer = NULL;
    manager->playbackReferenceFrameBytes = 0;
    manager->playbackReferenceFrames = 0;
    clear_playback_references(manager);
}

/* 只允许采集线程调用。用实际协商采样率换算，不能用请求值。 */
static void reset_warmup_discard(AudioCaptureManager *manager) {
    const uint64_t numerator = (uint64_t)manager->config.warmupDiscardDurationMs *
                               manager->capture.actualFormat.sampleRate;

    /* 向上取整，确保配置 500ms 时绝不会少丢一帧。 */
    manager->warmupFramesRemaining = (numerator + 999ULL) / 1000ULL;
    manager->warmupDiscardedFrames = 0;
}

static int allocate_playback_reference_storage(AudioCaptureManager *manager) {
    const size_t referenceFrames = manager->capture.actualFormat.sampleRate / 100;
    const size_t referenceBytes = referenceFrames * manager->capture.actualFormat.channels * sizeof(int16_t);

    if (referenceFrames == 0 || referenceBytes == 0) {
        return -EINVAL;
    }

    release_playback_reference_storage(manager);
    manager->playbackReferenceStorage = (uint8_t *)calloc(
        AUDIO_CAPTURE_MANAGER_PLAYBACK_REFERENCE_QUEUE_CAPACITY, referenceBytes);
    manager->playbackReferenceProcessingBuffer = (uint8_t *)malloc(referenceBytes);
    if (manager->playbackReferenceStorage == NULL || manager->playbackReferenceProcessingBuffer == NULL) {
        release_playback_reference_storage(manager);
        return -ENOMEM;
    }
    manager->playbackReferenceFrameBytes = referenceBytes;
    manager->playbackReferenceFrames = referenceFrames;
    return 0;
}

/* 只允许采集线程调用：这里与 ProcessCapture() 串行，APM 不会被播放线程并发访问。 */
static void apply_pending_apm_reconfiguration(AudioCaptureManager *manager) {
    AudioApmConfig requestedConfig;
    AudioApm replacement;
    AudioApm oldApm;
    int result;

    pthread_mutex_lock(&manager->apmConfigMutex);
    if (!manager->apmReconfigureRequested) {
        pthread_mutex_unlock(&manager->apmConfigMutex);
        return;
    }
    requestedConfig = manager->pendingApmConfig;
    manager->apmReconfigureRequested = 0;
    pthread_mutex_unlock(&manager->apmConfigMutex);

    memset(&replacement, 0, sizeof(replacement));
    result = audio_apm_open(&replacement,
                            &requestedConfig,
                            manager->capture.actualFormat,
                            manager->capture.periodFrames);
    if (result < 0) {
        /* 不能先 close 旧 APM：重建失败时继续保持正在工作的普通采集链。 */
        fprintf(stderr, "AudioCaptureManager: 重建 APM%s失败: %d，保留旧配置\n",
                requestedConfig.enableEchoCancellation ? "（启用 AEC3）" : "（关闭 AEC3）",
                result);
        return;
    }

    oldApm = manager->apm;
    manager->apm = replacement;
    audio_apm_close(&oldApm);

    pthread_mutex_lock(&manager->apmConfigMutex);
    manager->config.apm = requestedConfig;
    pthread_mutex_unlock(&manager->apmConfigMutex);
    clear_playback_references(manager);
    /* 切换 AEC/普通模式也会重建内部处理状态，重新走一次短预热避免模式边界爆音。 */
    reset_warmup_discard(manager);
    fprintf(stdout, "AudioCaptureManager: APM 已切换，AEC3=%s\n",
            requestedConfig.enableEchoCancellation ? "on" : "off");
}

/* 只允许采集线程调用。取不到 reference 是正常状态，直接处理麦克风 PCM。 */
static void drain_playback_references(AudioCaptureManager *manager) {
    while (manager->config.apm.enableEchoCancellation) {
        AudioPcmFrame referenceFrame;
        size_t index;
        int result;

        pthread_mutex_lock(&manager->playbackReferenceMutex);
        if (manager->playbackReferenceCount == 0) {
            pthread_mutex_unlock(&manager->playbackReferenceMutex);
            return;
        }
        index = manager->playbackReferenceHead;
        memcpy(manager->playbackReferenceProcessingBuffer,
               manager->playbackReferenceStorage + index * manager->playbackReferenceFrameBytes,
               manager->playbackReferenceFrameBytes);
        referenceFrame.data = manager->playbackReferenceProcessingBuffer;
        referenceFrame.frames = manager->playbackReferenceFrames;
        referenceFrame.format = manager->capture.actualFormat;
        referenceFrame.timestampUs = manager->playbackReferenceTimestampsUs[index];
        manager->playbackReferenceHead =
            (index + 1) % AUDIO_CAPTURE_MANAGER_PLAYBACK_REFERENCE_QUEUE_CAPACITY;
        --manager->playbackReferenceCount;
        pthread_mutex_unlock(&manager->playbackReferenceMutex);

        result = audio_apm_process_reverse(&manager->apm, &referenceFrame);
        if (result < 0) {
            /* 不让 reference 异常中断上行；AEC 会在后续正常帧到来时继续收敛。 */
            fprintf(stderr, "AudioCaptureManager: AEC3 reference PCM 处理失败: %d\n", result);
        }
    }
}

/*
 * 只允许采集线程调用。返回 1 表示这整个 PCM frame 都还在预热期，调用方应直接返回；
 * 返回 0 时 frame 已调整为剩余的可输出部分。这样即使 ALSA period 不能整除预热时长，
 * 也不会多丢一整块音频。
 */
static int discard_warmup_pcm(AudioCaptureManager *manager, AudioPcmFrame *frame) {
    size_t discardedFrames;

    if (manager->warmupFramesRemaining == 0) {
        return 0;
    }
    discardedFrames = frame->frames < manager->warmupFramesRemaining
                          ? frame->frames
                          : (size_t)manager->warmupFramesRemaining;
    manager->warmupFramesRemaining -= discardedFrames;
    manager->warmupDiscardedFrames += discardedFrames;
    if (discardedFrames == frame->frames) {
        return 1;
    }
    frame->data += discardedFrames * frame->format.channels * sizeof(int16_t);
    frame->frames -= discardedFrames;
    frame->timestampUs += (uint64_t)discardedFrames * 1000000ULL / frame->format.sampleRate;
    return 0;
}

static int forward_encoded_packet(const AudioEncodedPacket *packet, void *userData) {
    AudioCaptureManager *manager = (AudioCaptureManager *)userData;
    if (manager == NULL || manager->callback == NULL) {
        return 0;
    }
    return manager->callback(packet, manager->callbackUserData);
}

static void encode_captured_pcm(const AudioPcmFrame *frame, void *userData) {
    AudioCaptureManager *manager = (AudioCaptureManager *)userData;
    AudioPcmFrame processedFrame;
    int result;

    if (manager == NULL || frame == NULL) {
        return;
    }
    apply_pending_apm_reconfiguration(manager);

    /*
     * 监控默认关闭 APM：预热期直接丢原始 PCM，之后直接编码。此处不调用
     * audio_apm_process_capture()，既不创建 WebRTC APM，也不做 bypass 的统计遍历。
     * config.apm 运行时只由当前采集线程在 apply_pending...() 中修改，故这里无需加锁。
     */
    if (!manager->config.apm.enableAudioProcessing) {
        processedFrame = *frame;
        if (discard_warmup_pcm(manager, &processedFrame)) {
            return;
        }
        result = audio_encoder_push_pcm(&manager->encoder, &processedFrame);
        if (result < 0) {
            fprintf(stderr, "AudioCaptureManager: encode PCM failed: %d\n", result);
        }
        return;
    }

    /* APM 开启时，预热帧必须先经过 APM，供降噪/AEC 等内部状态完成收敛。 */
    drain_playback_references(manager);
    result = audio_apm_process_capture(&manager->apm, frame, &processedFrame);
    if (result < 0) {
        fprintf(stderr, "AudioCaptureManager: APM 处理 PCM 失败: %d\n", result);
        return;
    }

    /* 预热帧已送 APM 收敛，但不会送入编码器。 */
    if (discard_warmup_pcm(manager, &processedFrame)) {
        return;
    }
    result = audio_encoder_push_pcm(&manager->encoder, &processedFrame);
    if (result < 0) {
        fprintf(stderr, "AudioCaptureManager: encode PCM failed: %d\n", result);
    }
}

void audio_capture_manager_config_init(AudioCaptureManagerConfig *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    audio_capture_config_init(&config->capture);
    audio_apm_config_init(&config->apm);
    audio_encoder_config_init(&config->encoder);
    config->warmupDiscardDurationMs = 500;
}

int audio_capture_manager_init(AudioCaptureManager *manager,
                               const AudioCaptureManagerConfig *config) {
    if (manager == NULL) {
        return -EINVAL;
    }
    memset(manager, 0, sizeof(*manager));
    audio_capture_manager_config_init(&manager->config);
    if (config != NULL) {
        manager->config = *config;
    }
    {
        const int mutexResult = pthread_mutex_init(&manager->playbackReferenceMutex, NULL);
        if (mutexResult != 0) {
            return -mutexResult;
        }
    }
    manager->playbackReferenceMutexInitialized = 1;
    {
        const int mutexResult = pthread_mutex_init(&manager->apmConfigMutex, NULL);
        if (mutexResult != 0) {
            pthread_mutex_destroy(&manager->playbackReferenceMutex);
            manager->playbackReferenceMutexInitialized = 0;
            return -mutexResult;
        }
    }
    manager->apmConfigMutexInitialized = 1;
    manager->pendingApmConfig = manager->config.apm;
    manager->initialized = 1;
    return 0;
}

void audio_capture_manager_set_packet_callback(AudioCaptureManager *manager,
                                               AudioEncodedPacketCallback callback,
                                               void *userData) {
    if (manager == NULL) {
        return;
    }
    manager->callback = callback;
    manager->callbackUserData = userData;
}

int audio_capture_manager_start(AudioCaptureManager *manager) {
    AudioApmConfig apmConfig;
    int result;
    if (manager == NULL || !manager->initialized) {
        return -EINVAL;
    }

    result = audio_capture_open_auto(&manager->capture, &manager->config.capture);
    if (result < 0) {
        return result;
    }

    result = allocate_playback_reference_storage(manager);
    if (result < 0) {
        audio_capture_close(&manager->capture);
        return result;
    }

    audio_encoder_set_packet_callback(&manager->encoder, forward_encoded_packet, manager);
    result = audio_encoder_init(&manager->encoder,
                                &manager->config.encoder,
                                &manager->capture.actualFormat);
    if (result < 0) {
        release_playback_reference_storage(manager);
        audio_capture_close(&manager->capture);
        return result;
    }

    pthread_mutex_lock(&manager->apmConfigMutex);
    apmConfig = manager->config.apm;
    pthread_mutex_unlock(&manager->apmConfigMutex);
    result = audio_apm_open(&manager->apm,
                            &apmConfig,
                            manager->capture.actualFormat,
                            manager->capture.periodFrames);
    if (result < 0) {
        audio_encoder_close(&manager->encoder);
        release_playback_reference_storage(manager);
        audio_capture_close(&manager->capture);
        return result;
    }

    reset_warmup_discard(manager);

    audio_capture_set_callback(&manager->capture, encode_captured_pcm, manager);
    /* 从这里起把 manager 视为运行中：若外部恰好在 pthread_create 前后请求 AEC，
       请求会登记为 pending，由首个采集回调安全应用，而不会静默覆盖刚打开的 APM。 */
    pthread_mutex_lock(&manager->apmConfigMutex);
    manager->started = 1;
    pthread_mutex_unlock(&manager->apmConfigMutex);
    result = audio_capture_start(&manager->capture);
    if (result < 0) {
        pthread_mutex_lock(&manager->apmConfigMutex);
        manager->started = 0;
        pthread_mutex_unlock(&manager->apmConfigMutex);
        audio_apm_close(&manager->apm);
        audio_encoder_close(&manager->encoder);
        release_playback_reference_storage(manager);
        audio_capture_close(&manager->capture);
        return result;
    }
    return 0;
}

void audio_capture_manager_stop(AudioCaptureManager *manager) {
    if (manager == NULL) {
        return;
    }
    audio_capture_stop(&manager->capture);
    pthread_mutex_lock(&manager->apmConfigMutex);
    manager->started = 0;
    pthread_mutex_unlock(&manager->apmConfigMutex);
    clear_playback_references(manager);
}

int audio_capture_manager_request_echo_cancellation(AudioCaptureManager *manager, int enabled) {
    AudioApmConfig requestedConfig;

    if (manager == NULL || !manager->initialized) {
        return -EINVAL;
    }

    pthread_mutex_lock(&manager->apmConfigMutex);
    requestedConfig = manager->apmReconfigureRequested ? manager->pendingApmConfig : manager->config.apm;
    /*
     * 该接口就是“进入/退出通话模式”的完整切换：默认监控是原始 PCM，通话才创建
     * APM 并开 AEC3；通话结束后回到原始 PCM。调用方不必先操作总开关再操作 AEC。
     */
    requestedConfig.enableAudioProcessing = enabled != 0;
    requestedConfig.enableEchoCancellation = enabled != 0;
    if (!manager->started) {
        manager->config.apm = requestedConfig;
        manager->pendingApmConfig = requestedConfig;
        manager->apmReconfigureRequested = 0;
        pthread_mutex_unlock(&manager->apmConfigMutex);
        return 0;
    }
    manager->pendingApmConfig = requestedConfig;
    manager->apmReconfigureRequested = 1;
    pthread_mutex_unlock(&manager->apmConfigMutex);
    return 0;
}

int audio_capture_manager_push_playback_reference(AudioCaptureManager *manager,
                                                  const AudioPcmFrame *frame) {
    size_t tail;
    int echoCancellationEnabled;

    if (manager == NULL || frame == NULL || frame->data == NULL || !manager->initialized) {
        return -EINVAL;
    }

    /*
     * PlaybackManager 可以无条件调用本函数；真正是否需要保存扬声器 reference
     * 由采集侧当前生效的 APM 配置决定。这样 AEC 启停不会在播放线程上额外切状态。
     */
    pthread_mutex_lock(&manager->apmConfigMutex);
    echoCancellationEnabled = manager->config.apm.enableAudioProcessing &&
                              manager->config.apm.enableEchoCancellation;
    pthread_mutex_unlock(&manager->apmConfigMutex);
    if (!echoCancellationEnabled) {
        return 0;
    }

    if (manager->playbackReferenceStorage == NULL || manager->playbackReferenceFrameBytes == 0 ||
        frame->frames != manager->playbackReferenceFrames ||
        frame->format.sampleRate != manager->capture.actualFormat.sampleRate ||
        frame->format.channels != manager->capture.actualFormat.channels ||
        frame->format.sampleFormat != manager->capture.actualFormat.sampleFormat) {
        return -EPIPE;
    }

    pthread_mutex_lock(&manager->playbackReferenceMutex);
    if (manager->playbackReferenceCount == AUDIO_CAPTURE_MANAGER_PLAYBACK_REFERENCE_QUEUE_CAPACITY) {
        manager->playbackReferenceHead =
            (manager->playbackReferenceHead + 1) % AUDIO_CAPTURE_MANAGER_PLAYBACK_REFERENCE_QUEUE_CAPACITY;
        --manager->playbackReferenceCount;
        ++manager->droppedPlaybackReferenceFrames;
    }
    tail = (manager->playbackReferenceHead + manager->playbackReferenceCount) %
           AUDIO_CAPTURE_MANAGER_PLAYBACK_REFERENCE_QUEUE_CAPACITY;
    memcpy(manager->playbackReferenceStorage + tail * manager->playbackReferenceFrameBytes,
           frame->data,
           manager->playbackReferenceFrameBytes);
    manager->playbackReferenceTimestampsUs[tail] = frame->timestampUs;
    ++manager->playbackReferenceCount;
    pthread_mutex_unlock(&manager->playbackReferenceMutex);
    return 0;
}

void audio_capture_manager_close(AudioCaptureManager *manager) {
    if (manager == NULL) {
        return;
    }
    audio_capture_manager_stop(manager);
    audio_capture_close(&manager->capture);
    audio_apm_close(&manager->apm);
    audio_encoder_flush(&manager->encoder);
    audio_encoder_close(&manager->encoder);
    release_playback_reference_storage(manager);
    if (manager->apmConfigMutexInitialized) {
        pthread_mutex_destroy(&manager->apmConfigMutex);
    }
    if (manager->playbackReferenceMutexInitialized) {
        pthread_mutex_destroy(&manager->playbackReferenceMutex);
    }
    memset(manager, 0, sizeof(*manager));
}
