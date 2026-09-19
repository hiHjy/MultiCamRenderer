#include "AudioPlaybackManager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t pcm_frame_bytes(const AudioPcmFormat *format) {
    if (format == NULL || format->sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE ||
        format->channels == 0) {
        return 0;
    }
    return (size_t)format->channels * sizeof(int16_t);
}

static size_t frames_per_10ms(const AudioPcmFormat *format) {
    if (format == NULL || format->sampleRate == 0 || format->sampleRate % 100 != 0) {
        return 0;
    }
    return format->sampleRate / 100;
}

static void clear_packet_queue_locked(AudioPlaybackManager *manager) {
    manager->queueHead = 0;
    manager->queueCount = 0;
    manager->playbackStarted = 0;
}

static void clear_pending_pcm(AudioPlaybackManager *manager) {
    manager->pendingPcmFrames = 0;
    manager->pendingPcmTimestampUs = 0;
    manager->pendingPcmTimestampRemainder = 0;
}

static void advance_pending_pcm_timestamp(AudioPlaybackManager *manager, size_t frames) {
    const uint64_t numerator = (uint64_t)frames * 1000000ULL +
                               manager->pendingPcmTimestampRemainder;
    manager->pendingPcmTimestampUs += numerator / manager->config.outputFormat.sampleRate;
    manager->pendingPcmTimestampRemainder = numerator % manager->config.outputFormat.sampleRate;
}

static int play_pending_10ms_blocks(AudioPlaybackManager *manager) {
    const size_t blockFrames = frames_per_10ms(&manager->config.outputFormat);
    const size_t frameBytes = pcm_frame_bytes(&manager->config.outputFormat);

    if (blockFrames == 0 || frameBytes == 0) {
        return -EINVAL;
    }

    while (manager->pendingPcmFrames >= blockFrames) {
        const AudioPcmFrame block = {
            .data = manager->pendingPcmStorage,
            .frames = blockFrames,
            .format = manager->config.outputFormat,
            .timestampUs = manager->pendingPcmTimestampUs,
        };
        const size_t remainingFrames = manager->pendingPcmFrames - blockFrames;
        int result;

        /* AEC 需要的是项目内单声道 PCM，而不是 ALSA plug 转成物理双声道之后的样本。 */
        if (manager->config.captureManager != NULL) {
            result = audio_capture_manager_push_playback_reference(manager->config.captureManager, &block);
            /* Capture 尚未启动或刚停止时没有 reference storage，这是正常的生命周期交错。 */
            if (result < 0 && result != -EPIPE) {
                fprintf(stderr, "AudioPlaybackManager: 投递 AEC reference 失败: %d\n", result);
            }
        }

        result = audio_playback_write_pcm(&manager->playback, &block);
        if (result < 0) {
            ++manager->playbackWriteFailures;
            return result;
        }
        manager->playedPcmFrames += blockFrames;
        advance_pending_pcm_timestamp(manager, blockFrames);

        if (remainingFrames != 0) {
            memmove(manager->pendingPcmStorage,
                    manager->pendingPcmStorage + blockFrames * frameBytes,
                    remainingFrames * frameBytes);
        }
        manager->pendingPcmFrames = remainingFrames;
    }
    return 0;
}

/* 此回调只由 PlaybackManager 线程在 audio_decoder_decode_packet() 内同步触发。 */
static int consume_decoded_pcm(const AudioPcmFrame *frame, void *userData) {
    AudioPlaybackManager *manager = (AudioPlaybackManager *)userData;
    const size_t frameBytes = pcm_frame_bytes(frame == NULL ? NULL : &frame->format);
    const size_t capacityFrames = AUDIO_PLAYBACK_MANAGER_MAX_PENDING_PCM_FRAMES;

    if (manager == NULL || frame == NULL || frame->data == NULL || frame->frames == 0 ||
        frameBytes == 0 || frame->format.sampleRate != manager->config.outputFormat.sampleRate ||
        frame->format.channels != manager->config.outputFormat.channels ||
        frame->format.sampleFormat != manager->config.outputFormat.sampleFormat ||
        manager->pendingPcmStorage == NULL) {
        return -EINVAL;
    }

    if (manager->pendingPcmFrames == 0) {
        manager->pendingPcmTimestampUs = frame->timestampUs;
        manager->pendingPcmTimestampRemainder = 0;
    }
    if (frame->frames > capacityFrames - manager->pendingPcmFrames) {
        /* 不接受异常大的解码输出，防止它破坏固定容量的实时 PCM 缓冲区。 */
        clear_pending_pcm(manager);
        return -EMSGSIZE;
    }
    memcpy(manager->pendingPcmStorage + manager->pendingPcmFrames * frameBytes,
           frame->data,
           frame->frames * frameBytes);
    manager->pendingPcmFrames += frame->frames;
    return play_pending_10ms_blocks(manager);
}

static void *playback_thread_main(void *argument) {
    AudioPlaybackManager *manager = (AudioPlaybackManager *)argument;

    while (manager != NULL) {
        AudioEncodedPacket packet;
        size_t packetSize;

        pthread_mutex_lock(&manager->queueMutex);
        while (!manager->stopRequested &&
               (manager->queueCount == 0 ||
                (!manager->playbackStarted &&
                 manager->queueCount < AUDIO_PLAYBACK_MANAGER_STARTUP_QUEUED_PACKETS))) {
            pthread_cond_wait(&manager->queueCondition, &manager->queueMutex);
        }
        if (manager->stopRequested) {
            pthread_mutex_unlock(&manager->queueMutex);
            break;
        }

        /*
         * 第一轮至少有 60ms PCM 可连续写入 ALSA，声卡不会只拿到 10/20ms 就起播。
         * 之后 queueCount 可以降到 1；未来网络 jitter/PLC 层负责保证持续供包。
         */
        manager->playbackStarted = 1;

        packetSize = manager->packetSizes[manager->queueHead];
        memcpy(manager->dequeueBuffer,
               manager->packetStorage + manager->queueHead * manager->config.maxPacketBytes,
               packetSize);
        packet.codec = manager->config.codec;
        packet.data = manager->dequeueBuffer;
        packet.size = packetSize;
        packet.timestampUs = manager->packetTimestampsUs[manager->queueHead];
        packet.durationUs = manager->packetDurationsUs[manager->queueHead];
        packet.sourceFormat = manager->config.outputFormat;
        manager->queueHead = (manager->queueHead + 1) % manager->config.maxQueuedPackets;
        --manager->queueCount;
        pthread_mutex_unlock(&manager->queueMutex);

        if (audio_decoder_decode_packet(&manager->decoder, &packet) < 0) {
            fprintf(stderr, "AudioPlaybackManager: 解码 %s 包失败，bytes=%zu\n",
                    packet.codec == AUDIO_CODEC_OPUS ? "Opus" : "audio", packet.size);
            continue;
        }
        ++manager->decodedPacketCount;
    }
    return NULL;
}

void audio_playback_manager_config_init(AudioPlaybackManagerConfig *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->codec = AUDIO_CODEC_OPUS;
    config->outputFormat.sampleRate = 48000;
    config->outputFormat.channels = 1;
    config->outputFormat.sampleFormat = AUDIO_SAMPLE_FORMAT_S16_LE;
    config->requestedPeriodFrames = 480;
    config->maxQueuedPackets = AUDIO_PLAYBACK_MANAGER_DEFAULT_MAX_QUEUED_PACKETS;
    config->maxPacketBytes = AUDIO_PLAYBACK_MANAGER_DEFAULT_MAX_PACKET_BYTES;
}

int audio_playback_manager_init(AudioPlaybackManager *manager,
                                const AudioPlaybackManagerConfig *config) {
    AudioPlaybackManagerConfig effectiveConfig;
    AudioPlaybackConfig playbackConfig;
    size_t frameBytes;
    int result;

    if (manager == NULL) {
        return -EINVAL;
    }
    memset(manager, 0, sizeof(*manager));
    audio_playback_manager_config_init(&effectiveConfig);
    if (config != NULL) {
        effectiveConfig = *config;
    }
    frameBytes = pcm_frame_bytes(&effectiveConfig.outputFormat);
    if (effectiveConfig.codec == AUDIO_CODEC_UNKNOWN || frameBytes == 0 ||
        frames_per_10ms(&effectiveConfig.outputFormat) == 0 ||
        effectiveConfig.requestedPeriodFrames == 0 || effectiveConfig.maxQueuedPackets == 0 ||
        effectiveConfig.maxPacketBytes == 0) {
        return -EINVAL;
    }
    if (effectiveConfig.maxQueuedPackets > SIZE_MAX / effectiveConfig.maxPacketBytes) {
        return -EOVERFLOW;
    }

    manager->config = effectiveConfig;
    result = pthread_mutex_init(&manager->queueMutex, NULL);
    if (result != 0) {
        return -result;
    }
    result = pthread_cond_init(&manager->queueCondition, NULL);
    if (result != 0) {
        pthread_mutex_destroy(&manager->queueMutex);
        return -result;
    }
    manager->queueSynchronizationInitialized = 1;

    manager->packetStorage = (uint8_t *)calloc(effectiveConfig.maxQueuedPackets,
                                                effectiveConfig.maxPacketBytes);
    manager->packetSizes = (size_t *)calloc(effectiveConfig.maxQueuedPackets, sizeof(size_t));
    manager->packetTimestampsUs = (uint64_t *)calloc(effectiveConfig.maxQueuedPackets, sizeof(uint64_t));
    manager->packetDurationsUs = (uint32_t *)calloc(effectiveConfig.maxQueuedPackets, sizeof(uint32_t));
    manager->dequeueBuffer = (uint8_t *)malloc(effectiveConfig.maxPacketBytes);
    manager->pendingPcmStorage = (uint8_t *)malloc(
        AUDIO_PLAYBACK_MANAGER_MAX_PENDING_PCM_FRAMES * frameBytes);
    if (manager->packetStorage == NULL || manager->packetSizes == NULL ||
        manager->packetTimestampsUs == NULL || manager->packetDurationsUs == NULL ||
        manager->dequeueBuffer == NULL || manager->pendingPcmStorage == NULL) {
        audio_playback_manager_close(manager);
        return -ENOMEM;
    }

    audio_playback_config_init(&playbackConfig);
    playbackConfig.requestedFormat = effectiveConfig.outputFormat;
    playbackConfig.requestedPeriodFrames = effectiveConfig.requestedPeriodFrames;
    /*
     * APM reference 仍以 10ms 分块，但 ALSA 驱动需要略大的真实硬件队列吸收调度抖动。
     * 8 × 10ms = 80ms；它不替代将来通话层的 jitter buffer，也不会让编码包队列增长。
     */
    playbackConfig.requestedBufferFrames = effectiveConfig.requestedPeriodFrames * 8;
    result = audio_playback_open_auto(&manager->playback, &playbackConfig);
    if (result < 0) {
        audio_playback_manager_close(manager);
        return result;
    }

    audio_decoder_set_pcm_callback(&manager->decoder, consume_decoded_pcm, manager);
    result = audio_decoder_init(&manager->decoder, effectiveConfig.codec, &effectiveConfig.outputFormat);
    if (result < 0) {
        audio_playback_manager_close(manager);
        return result;
    }
    manager->initialized = 1;
    return 0;
}

int audio_playback_manager_start(AudioPlaybackManager *manager) {
    int result;

    if (manager == NULL || !manager->initialized) {
        return -EINVAL;
    }
    pthread_mutex_lock(&manager->queueMutex);
    if (manager->threadCreated) {
        pthread_mutex_unlock(&manager->queueMutex);
        return -EBUSY;
    }
    clear_packet_queue_locked(manager);
    clear_pending_pcm(manager);
    manager->stopRequested = 0;

    /*
     * 在 queueMutex 持有期间完成 pthread_create，并在成功后再发布 threadCreated。
     * stop() 会先取得同一把锁，因此不会在“pthread_create 尚未返回、pthread_t 还未
     * 有效”的窗口调用 pthread_join。
     */
    result = pthread_create(&manager->playbackThread, NULL, playback_thread_main, manager);
    if (result != 0) {
        pthread_mutex_unlock(&manager->queueMutex);
        return -result;
    }
    manager->threadCreated = 1;
    pthread_mutex_unlock(&manager->queueMutex);
    return 0;
}

int audio_playback_manager_enqueue_packet(AudioPlaybackManager *manager,
                                          const AudioEncodedPacket *packet) {
    size_t tail;

    if (manager == NULL || packet == NULL || packet->data == NULL || packet->size == 0 ||
        !manager->initialized || packet->codec != manager->config.codec ||
        packet->size > manager->config.maxPacketBytes) {
        return -EINVAL;
    }
    pthread_mutex_lock(&manager->queueMutex);
    if (!manager->threadCreated || manager->stopRequested) {
        pthread_mutex_unlock(&manager->queueMutex);
        return -EPIPE;
    }
    if (manager->queueCount == manager->config.maxQueuedPackets) {
        /* 语音实时性优先：淘汰最早、最晚才会播放到的编码包。 */
        manager->queueHead = (manager->queueHead + 1) % manager->config.maxQueuedPackets;
        --manager->queueCount;
        ++manager->droppedPacketCount;
    }
    tail = (manager->queueHead + manager->queueCount) % manager->config.maxQueuedPackets;
    memcpy(manager->packetStorage + tail * manager->config.maxPacketBytes, packet->data, packet->size);
    manager->packetSizes[tail] = packet->size;
    manager->packetTimestampsUs[tail] = packet->timestampUs;
    manager->packetDurationsUs[tail] = packet->durationUs;
    ++manager->queueCount;
    pthread_cond_signal(&manager->queueCondition);
    pthread_mutex_unlock(&manager->queueMutex);
    return 0;
}

void audio_playback_manager_stop(AudioPlaybackManager *manager) {
    int shouldJoin = 0;

    if (manager == NULL || !manager->queueSynchronizationInitialized) {
        return;
    }
    pthread_mutex_lock(&manager->queueMutex);
    if (manager->threadCreated) {
        manager->stopRequested = 1;
        clear_packet_queue_locked(manager);
        pthread_cond_broadcast(&manager->queueCondition);
        shouldJoin = 1;
    }
    pthread_mutex_unlock(&manager->queueMutex);

    if (shouldJoin) {
        pthread_join(manager->playbackThread, NULL);
        pthread_mutex_lock(&manager->queueMutex);
        manager->threadCreated = 0;
        clear_pending_pcm(manager);
        pthread_mutex_unlock(&manager->queueMutex);
    }
}

void audio_playback_manager_close(AudioPlaybackManager *manager) {
    if (manager == NULL) {
        return;
    }
    audio_playback_manager_stop(manager);
    audio_decoder_close(&manager->decoder);
    audio_playback_close(&manager->playback);
    free(manager->packetStorage);
    free(manager->packetSizes);
    free(manager->packetTimestampsUs);
    free(manager->packetDurationsUs);
    free(manager->dequeueBuffer);
    free(manager->pendingPcmStorage);
    if (manager->queueSynchronizationInitialized) {
        pthread_cond_destroy(&manager->queueCondition);
        pthread_mutex_destroy(&manager->queueMutex);
    }
    memset(manager, 0, sizeof(*manager));
}
