#include "AudioPacketFile.h"
#include "AudioPlaybackManager.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static atomic_bool g_stopRequested;

static uint64_t monotonic_now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
}

static void request_stop(int signalNumber) {
    (void)signalNumber;
    atomic_store(&g_stopRequested, true);
}

/*
 * packet 文件读取极快；这里按文件中的相对 PTS 节奏喂给 manager，才能验证真实播放
 * 队列而不是瞬间灌满它。网络接入时这个节奏由协议/通话层负责。
 */
static void wait_until_us(uint64_t deadlineUs) {
    while (!atomic_load(&g_stopRequested)) {
        const uint64_t nowUs = monotonic_now_us();
        struct timespec sleepTime;
        uint64_t remainingUs;

        if (nowUs >= deadlineUs) {
            return;
        }
        remainingUs = deadlineUs - nowUs;
        sleepTime.tv_sec = (time_t)(remainingUs / 1000000ULL);
        sleepTime.tv_nsec = (long)((remainingUs % 1000000ULL) * 1000ULL);
        nanosleep(&sleepTime, NULL);
    }
}

int main(int argc, char **argv) {
    const char *inputPath = argc >= 2 ? argv[1] : "audio_capture.opus";
    AudioPacketFileReader reader;
    AudioPlaybackManager manager;
    AudioPlaybackManagerConfig config;
    struct sigaction signalAction;
    uint64_t firstPacketTimestampUs = 0;
    uint64_t playbackStartUs = 0;
    size_t enqueuedPackets = 0;
    int result = 0;

    memset(&reader, 0, sizeof(reader));
    memset(&manager, 0, sizeof(manager));
    memset(&signalAction, 0, sizeof(signalAction));
    signalAction.sa_handler = request_stop;
    sigaction(SIGINT, &signalAction, NULL);
    sigaction(SIGTERM, &signalAction, NULL);
    atomic_init(&g_stopRequested, false);

    result = audio_packet_file_reader_open(&reader, inputPath);
    if (result < 0) {
        fprintf(stderr, "AudioPlaybackManagerDemo: 无法打开 %s: %d\n", inputPath, result);
        return 1;
    }
    audio_playback_manager_config_init(&config);
    config.codec = reader.info.codec;
    config.outputFormat = reader.info.sourceFormat;
    config.requestedPeriodFrames = config.outputFormat.sampleRate / 100;
    result = audio_playback_manager_init(&manager, &config);
    if (result < 0) {
        fprintf(stderr, "AudioPlaybackManagerDemo: 初始化播放 manager 失败: %d\n", result);
        audio_packet_file_reader_close(&reader);
        return 1;
    }
    result = audio_playback_manager_start(&manager);
    if (result < 0) {
        fprintf(stderr, "AudioPlaybackManagerDemo: 启动播放线程失败: %d\n", result);
        audio_playback_manager_close(&manager);
        audio_packet_file_reader_close(&reader);
        return 1;
    }

    printf("AudioPlaybackManagerDemo: file=%s codec=%d PCM=%uHz/%uch，Ctrl+C 停止\n",
           inputPath, reader.info.codec, reader.info.sourceFormat.sampleRate,
           reader.info.sourceFormat.channels);
    while (!atomic_load(&g_stopRequested)) {
        AudioEncodedPacket packet;
        result = audio_packet_file_reader_read(&reader, &packet);
        if (result == 0) {
            break;
        }
        if (result < 0) {
            fprintf(stderr, "AudioPlaybackManagerDemo: 读取编码包失败: %d\n", result);
            break;
        }
        if (enqueuedPackets == 0) {
            firstPacketTimestampUs = packet.timestampUs;
            playbackStartUs = monotonic_now_us();
        }
        wait_until_us(playbackStartUs + (packet.timestampUs - firstPacketTimestampUs));
        if (atomic_load(&g_stopRequested)) {
            break;
        }
        result = audio_playback_manager_enqueue_packet(&manager, &packet);
        if (result < 0) {
            fprintf(stderr, "AudioPlaybackManagerDemo: 入队编码包失败: %d\n", result);
            break;
        }
        ++enqueuedPackets;
    }

    /* 给最后已入队的少量音频一个短暂播放窗口；不是 drain 无限队列。 */
    if (result >= 0 && !atomic_load(&g_stopRequested)) {
        struct timespec finishDelay = {.tv_sec = 0, .tv_nsec = 250000000L};
        nanosleep(&finishDelay, NULL);
    }
    audio_playback_manager_stop(&manager);
    printf("AudioPlaybackManagerDemo: 入队=%zu 解码=%llu 播放=%llu frames 队列淘汰=%llu ALSA失败=%llu\n",
           enqueuedPackets,
           (unsigned long long)manager.decodedPacketCount,
           (unsigned long long)manager.playedPcmFrames,
           (unsigned long long)manager.droppedPacketCount,
           (unsigned long long)manager.playbackWriteFailures);
    audio_playback_manager_close(&manager);
    audio_packet_file_reader_close(&reader);
    return result < 0 ? 1 : 0;
}
