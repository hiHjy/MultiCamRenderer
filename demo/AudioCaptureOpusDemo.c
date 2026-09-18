#include "AudioInputManager.h"
#include "AudioPacketFile.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static atomic_bool g_stopRequested;

typedef struct PacketStatistics {
    size_t packets;
    uint64_t previousTimestampUs;
    const char *outputPath;
    AudioPacketFileWriter writer;
} PacketStatistics;

static void request_stop(int signalNumber) {
    (void)signalNumber;
    atomic_store(&g_stopRequested, true);
}

static int on_opus_packet(const AudioEncodedPacket *packet, void *userData) {
    PacketStatistics *statistics = (PacketStatistics *)userData;
    uint64_t timestampDeltaUs = 0;

    if (packet == NULL || statistics == NULL) {
        return 0;
    }
    if (statistics->writer.file == NULL) {
        const AudioPacketFileInfo fileInfo = {
            .codec = packet->codec,
            .sourceFormat = packet->sourceFormat,
            .frameDurationUs = packet->durationUs,
        };
        if (audio_packet_file_writer_open(&statistics->writer, statistics->outputPath, &fileInfo) < 0) {
            fprintf(stderr, "AudioCaptureOpusDemo: 无法创建 %s\n", statistics->outputPath);
            return -1;
        }
    }
    if (audio_packet_file_writer_write(&statistics->writer, packet) < 0) {
        fprintf(stderr, "AudioCaptureOpusDemo: 写入 Opus 包失败\n");
        return -1;
    }
    if (statistics->packets > 0) {
        timestampDeltaUs = packet->timestampUs - statistics->previousTimestampUs;
    }
    ++statistics->packets;
    statistics->previousTimestampUs = packet->timestampUs;

    if (statistics->packets == 1 || statistics->packets % 50 == 0) {
        printf("AudioCaptureOpusDemo: packet=%zu bytes=%zu pts=%llu duration=%u delta=%llu\n",
               statistics->packets,
               packet->size,
               (unsigned long long)packet->timestampUs,
               packet->durationUs,
               (unsigned long long)timestampDeltaUs);
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    AudioInputManager manager;
    AudioInputManagerConfig config;
    PacketStatistics statistics;
    AudioApmStatistics apmStatistics;
    struct sigaction signalAction;

    memset(&statistics, 0, sizeof(statistics));
    statistics.outputPath = argc >= 2 ? argv[1] : "audio_capture.opus";
    memset(&signalAction, 0, sizeof(signalAction));
    signalAction.sa_handler = request_stop;
    sigaction(SIGINT, &signalAction, NULL);
    sigaction(SIGTERM, &signalAction, NULL);
    atomic_init(&g_stopRequested, false);

    audio_input_manager_config_init(&config);
    if (audio_input_manager_init(&manager, &config) < 0) {
        fprintf(stderr, "AudioCaptureOpusDemo: 初始化音频管理器失败\n");
        return 1;
    }
    audio_input_manager_set_packet_callback(&manager, on_opus_packet, &statistics);
    if (audio_input_manager_start(&manager) < 0) {
        fprintf(stderr, "AudioCaptureOpusDemo: 自动打开采集设备或 Opus 编码器失败\n");
        audio_input_manager_close(&manager);
        return 1;
    }

    printf("AudioCaptureOpusDemo: 正在采集并编码 Opus 到 %s，按 Ctrl+C 停止\n",
           statistics.outputPath);
    while (!atomic_load(&g_stopRequested)) {
        const struct timespec sleepDuration = {.tv_sec = 0, .tv_nsec = 100000000L};
        nanosleep(&sleepDuration, NULL);
    }

    audio_input_manager_stop(&manager);
    audio_apm_get_statistics(&manager.apm, &apmStatistics);
    audio_input_manager_close(&manager);
    audio_packet_file_writer_close(&statistics.writer);
    printf("AudioCaptureOpusDemo: 已停止，累计 Opus 包=%zu，文件=%s\n",
           statistics.packets,
           statistics.outputPath);
    if (apmStatistics.processedSamples != 0) {
        const double inputMean = (double)apmStatistics.inputAbsoluteSampleSum /
                                 (double)apmStatistics.processedSamples;
        const double outputMean = (double)apmStatistics.outputAbsoluteSampleSum /
                                  (double)apmStatistics.processedSamples;
        printf("AudioCaptureOpusDemo: APM inputMean=%.1f outputMean=%.1f gain=%.2fx\n",
               inputMean,
               outputMean,
               inputMean > 0.0 ? outputMean / inputMean : 0.0);
    }
    return 0;
}
