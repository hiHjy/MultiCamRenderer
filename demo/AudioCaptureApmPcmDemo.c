/*
 * 采集 -> WebRTC APM(AGC) -> 写 PCM/WAV，全程不经过 Opus 编码。
 *
 * 和 AudioCaptureOpusDemo 的差别只在链路末端：那个把处理后的 PCM 送进 Opus
 * 编码器，这个直接落盘成 WAV，方便用耳朵和波形工具直接判断 AGC 的实际效果
 * （例如板端麦克风电平偏低时，到底靠 AGC 能补回多少）。
 *
 * 用法: audio_capture_apm_pcm_demo [输出文件] [录制秒数] [固定增益dB] [原始PCM文件]
 *       默认 audio_apm_capture.wav，10 秒，固定增益取 audio_apm_config_init() 的默认值
 *
 * 给了第 4 个参数时，除了 APM 输出，还会把未经处理的原始 PCM 写到该路径。
 * 两份文件来自同一次采集，因此可以直接 A/B 判断 APM 到底帮了忙还是帮了倒忙。
 *
 * 固定增益（gain_controller2.fixed_digital.gain_db）是确定性的一级，用来补硬件侧
 * 已知的固定衰减；AGC2 的自适应增益叠加在它之上，需要检测到语音才会推。
 */

#include "AudioApm.h"
#include "AudioCapture.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_bool g_stopRequested;

static void request_stop(int signalNumber) {
    (void)signalNumber;
    atomic_store(&g_stopRequested, true);
}

/* ------------------------------------------------------------------ */
/* 极简 WAV 写入：44 字节规范头 + S16_LE 交错 PCM，关闭时回填两处长度。*/
/* ------------------------------------------------------------------ */

/*
 * APM 处理的是单声道采集，但不少板子（实测 RV1126B 的 dailink-multicodecs）
 * 只有按双声道打开播放设备才出声，单声道文件 aplay 上去是静音。这个 demo 的
 * 目的就是"录下来听"，所以落盘时统一补成双声道（单声道数据复制到左右两个声道），
 * 省掉每次再转一遍的麻烦。分析波形时记得除以 2 个声道。
 */
#define WAV_OUTPUT_CHANNELS 2

typedef struct WavWriter {
    FILE *file;
    uint32_t sampleRate;
    uint16_t sourceChannels;
    uint16_t outputChannels;
    uint64_t dataBytes;
} WavWriter;

static void put_le32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)(value & 0xFFu);
    destination[1] = (uint8_t)((value >> 8) & 0xFFu);
    destination[2] = (uint8_t)((value >> 16) & 0xFFu);
    destination[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void put_le16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value & 0xFFu);
    destination[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static int wav_writer_open(WavWriter *writer, const char *path, const AudioPcmFormat *format) {
    uint8_t header[44];
    const uint16_t bitsPerSample = 16;
    uint16_t blockAlign;

    if (writer == NULL || path == NULL || format == NULL ||
        format->sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE) {
        return -EINVAL;
    }
    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, "wb");
    if (writer->file == NULL) {
        return -errno;
    }
    writer->sampleRate = format->sampleRate;
    writer->sourceChannels = format->channels;
    /* 单声道采集统一补成双声道落盘，见 WAV_OUTPUT_CHANNELS 的说明 */
    writer->outputChannels = (format->channels == 1) ? WAV_OUTPUT_CHANNELS : format->channels;
    blockAlign = (uint16_t)(writer->outputChannels * (bitsPerSample / 8));

    memset(header, 0, sizeof(header));
    memcpy(header + 0, "RIFF", 4);
    put_le32(header + 4, 36);                                            /* 关闭时回填 */
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    put_le32(header + 16, 16);
    put_le16(header + 20, 1);                                            /* PCM */
    put_le16(header + 22, writer->outputChannels);
    put_le32(header + 24, format->sampleRate);
    put_le32(header + 28, format->sampleRate * blockAlign);              /* byteRate */
    put_le16(header + 32, blockAlign);
    put_le16(header + 34, bitsPerSample);
    memcpy(header + 36, "data", 4);
    put_le32(header + 40, 0);                                            /* 关闭时回填 */

    if (fwrite(header, 1, sizeof(header), writer->file) != sizeof(header)) {
        fclose(writer->file);
        writer->file = NULL;
        return -EIO;
    }
    return 0;
}

static int wav_writer_write(WavWriter *writer, const AudioPcmFrame *frame) {
    const int16_t *samples;
    size_t frames;
    size_t done;

    if (writer->file == NULL || frame->data == NULL || frame->frames == 0 ||
        frame->format.channels != writer->sourceChannels) {
        return -EINVAL;
    }
    samples = (const int16_t *)frame->data;
    frames = frame->frames;

    if (writer->outputChannels == writer->sourceChannels) {
        const size_t bytes = frames * writer->outputChannels * sizeof(int16_t);
        if (fwrite(samples, 1, bytes, writer->file) != bytes) {
            return -EIO;
        }
        writer->dataBytes += bytes;
        return 0;
    }
    if (writer->sourceChannels != 1 || writer->outputChannels != 2) {
        return -ENOTSUP;
    }

    /* 单声道 -> 双声道：每个样本写两遍 */
    for (done = 0; done < frames;) {
        enum { kChunkFrames = 480 };
        int16_t chunk[kChunkFrames * 2];
        const size_t count = (frames - done) < kChunkFrames ? (frames - done) : kChunkFrames;
        size_t index;

        for (index = 0; index < count; ++index) {
            chunk[index * 2] = samples[done + index];
            chunk[index * 2 + 1] = samples[done + index];
        }
        if (fwrite(chunk, sizeof(int16_t) * 2, count, writer->file) != count) {
            return -EIO;
        }
        writer->dataBytes += count * 2 * sizeof(int16_t);
        done += count;
    }
    return 0;
}

static void wav_writer_close(WavWriter *writer) {
    uint8_t sizes[4];

    if (writer->file == NULL) {
        return;
    }
    /* 回填 RIFF chunkSize 与 data chunkSize，否则播放器读不出时长 */
    if (fseek(writer->file, 4, SEEK_SET) == 0) {
        put_le32(sizes, (uint32_t)(36 + writer->dataBytes));
        fwrite(sizes, 1, sizeof(sizes), writer->file);
    }
    if (fseek(writer->file, 40, SEEK_SET) == 0) {
        put_le32(sizes, (uint32_t)writer->dataBytes);
        fwrite(sizes, 1, sizeof(sizes), writer->file);
    }
    fclose(writer->file);
    writer->file = NULL;
}

/* ------------------------------------------------------------------ */
/* 采集回调：APM 处理完立刻落盘                                        */
/* ------------------------------------------------------------------ */

typedef struct CaptureContext {
    AudioApm apm;
    WavWriter writer;
    WavWriter rawWriter;
    int writeRaw;
    uint64_t writtenFrames;
    uint64_t outputFrames;
} CaptureContext;

static void on_captured_pcm(const AudioPcmFrame *frame, void *userData) {
    CaptureContext *context = (CaptureContext *)userData;
    AudioPcmFrame processed;

    if (context == NULL || frame == NULL) {
        return;
    }
    /* 先落原始 PCM：对比"处理前/处理后"时必须是同一段音频 */
    if (context->writeRaw && wav_writer_write(&context->rawWriter, frame) < 0) {
        fprintf(stderr, "AudioCaptureApmPcmDemo: 写入原始 PCM 失败\n");
        atomic_store(&g_stopRequested, true);
        return;
    }
    if (audio_apm_process_capture(&context->apm, frame, &processed) < 0) {
        fprintf(stderr, "AudioCaptureApmPcmDemo: APM 处理失败\n");
        atomic_store(&g_stopRequested, true);
        return;
    }
    /* processed.data 由 apm 持有，下次 process 前有效 —— 必须当场写掉 */
    if (wav_writer_write(&context->writer, &processed) < 0) {
        fprintf(stderr, "AudioCaptureApmPcmDemo: 写入 PCM 失败\n");
        atomic_store(&g_stopRequested, true);
        return;
    }
    context->writtenFrames += processed.frames;
    ++context->outputFrames;
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    const char *outputPath = argc >= 2 ? argv[1] : "audio_apm_capture.wav";
    const double durationSeconds = argc >= 3 ? strtod(argv[2], NULL) : 10.0;
    const int fixedGainDb = argc >= 4 ? atoi(argv[3]) : -1;
    /* 可选：同时落一份未经 APM 的原始 PCM，用于"同一段音频"的 A/B 对比 */
    const char *rawOutputPath = argc >= 5 ? argv[4] : NULL;
    AudioApmConfig apmConfig;
    AudioCaptureConfig captureConfig;
    CaptureContext context;
    AudioApmStatistics statistics;
    AudioCapture capture;
    struct sigaction signalAction;
    double startSeconds;
    double safetyDeadline;
    uint32_t sampleRate;
    int result;

    if (durationSeconds <= 0.0) {
        fprintf(stderr, "AudioCaptureApmPcmDemo: 录制秒数必须为正\n");
        return 1;
    }

    memset(&capture, 0, sizeof(capture));
    memset(&context, 0, sizeof(context));
    memset(&statistics, 0, sizeof(statistics));
    memset(&signalAction, 0, sizeof(signalAction));
    signalAction.sa_handler = request_stop;
    sigaction(SIGINT, &signalAction, NULL);
    sigaction(SIGTERM, &signalAction, NULL);
    atomic_init(&g_stopRequested, false);

    audio_capture_config_init(&captureConfig);
    result = audio_capture_open_auto(&capture, &captureConfig);
    if (result < 0) {
        fprintf(stderr, "AudioCaptureApmPcmDemo: 打开采集设备失败: %d\n", result);
        return 1;
    }

    /*
     * APM 固定按 10ms 工作，因此采集 period 必须是 sampleRate/100 的整数倍。
     * AudioCapture 默认 480 帧 @48kHz 即 10ms，正好满足要求。
     */
    audio_apm_config_init(&apmConfig);
    /* 这是专门用于听 APM 效果的 demo；即使产品默认关闭 APM，也必须显式打开。 */
    apmConfig.enableAudioProcessing = 1;
    if (fixedGainDb >= 0) {
        apmConfig.fixedDigitalGainDb = fixedGainDb;
    }
    result = audio_apm_open(&context.apm, &apmConfig, capture.actualFormat, capture.periodFrames);
    if (result < 0) {
        fprintf(stderr, "AudioCaptureApmPcmDemo: 初始化 APM 失败: %d\n", result);
        audio_capture_close(&capture);
        return 1;
    }

    result = wav_writer_open(&context.writer, outputPath, &capture.actualFormat);
    if (result < 0) {
        fprintf(stderr, "AudioCaptureApmPcmDemo: 无法创建 %s: %d\n", outputPath, result);
        audio_apm_close(&context.apm);
        audio_capture_close(&capture);
        return 1;
    }
    if (rawOutputPath != NULL) {
        result = wav_writer_open(&context.rawWriter, rawOutputPath, &capture.actualFormat);
        if (result < 0) {
            fprintf(stderr, "AudioCaptureApmPcmDemo: 无法创建 %s: %d\n", rawOutputPath, result);
            wav_writer_close(&context.writer);
            audio_apm_close(&context.apm);
            audio_capture_close(&capture);
            return 1;
        }
        context.writeRaw = 1;
    }

    printf("AudioCaptureApmPcmDemo: 设备=%s [%s] PCM=%uHz %uch S16_LE period=%lu\n",
           capture.deviceInfo.alsaName,
           capture.deviceInfo.pcmName,
           capture.actualFormat.sampleRate,
           capture.actualFormat.channels,
           (unsigned long)capture.periodFrames);
    printf("AudioCaptureApmPcmDemo: 输出=%s 目标时长=%.1fs（APM 处理后的 PCM，按 Ctrl+C 提前结束）\n",
           outputPath,
           durationSeconds);
    fflush(stdout);

    audio_capture_set_callback(&capture, on_captured_pcm, &context);
    result = audio_capture_start(&capture);
    if (result < 0) {
        fprintf(stderr, "AudioCaptureApmPcmDemo: 启动采集失败: %d\n", result);
        wav_writer_close(&context.rawWriter);
        wav_writer_close(&context.writer);
        audio_apm_close(&context.apm);
        audio_capture_close(&capture);
        return 1;
    }

    startSeconds = monotonic_seconds();
    /* 兜底：设备若中途不出数据，不能让进程永远挂着 */
    safetyDeadline = startSeconds + durationSeconds * 2.0 + 10.0;
    for (;;) {
        const struct timespec sleepDuration = {.tv_sec = 0, .tv_nsec = 50000000L};
        const uint64_t targetFrames =
            (uint64_t)(durationSeconds * (double)capture.actualFormat.sampleRate);

        if (atomic_load(&g_stopRequested) || context.writtenFrames >= targetFrames) {
            break;
        }
        if (monotonic_seconds() > safetyDeadline) {
            fprintf(stderr, "AudioCaptureApmPcmDemo: 超时（只录到 %.2fs），提前结束\n",
                    (double)context.writtenFrames / (double)capture.actualFormat.sampleRate);
            break;
        }
        nanosleep(&sleepDuration, NULL);
    }

    /* audio_capture_close() 会 memset 整个结构体，采样率必须先取出来 */
    sampleRate = capture.actualFormat.sampleRate;
    audio_capture_stop(&capture);
    audio_capture_close(&capture);
    audio_apm_get_statistics(&context.apm, &statistics);
    audio_apm_close(&context.apm);
    wav_writer_close(&context.rawWriter);
    wav_writer_close(&context.writer);

    printf("AudioCaptureApmPcmDemo: 已停止，写入 %llu 帧（%.2fs），文件=%s\n",
           (unsigned long long)context.writtenFrames,
           sampleRate != 0 ? (double)context.writtenFrames / (double)sampleRate : 0.0,
           outputPath);
    printf("AudioCaptureApmPcmDemo: APM processedSamples=%llu\n",
           (unsigned long long)statistics.processedSamples);
    return 0;
}
