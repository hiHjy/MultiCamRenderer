#include "AudioDecoder.h"
#include "AudioPacketFile.h"
#include "AudioPlayback.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_bool g_stopRequested;

typedef struct PlaybackContext {
    AudioPlayback playback;
    size_t decodedPackets;
    size_t playedFrames;
    uint64_t pcmAbsoluteSum;
    size_t pcmSamples;
    int pcmPeak;
} PlaybackContext;

static void request_stop(int signalNumber) {
    (void)signalNumber;
    atomic_store(&g_stopRequested, true);
}

static int play_decoded_pcm(const AudioPcmFrame *frame, void *userData) {
    PlaybackContext *context = (PlaybackContext *)userData;
    const int result = context == NULL ? -1 : audio_playback_write_pcm(&context->playback, frame);
    if (result < 0) {
        fprintf(stderr, "AudioOpusPlaybackDemo: ALSA 播放 PCM 失败: %d\n", result);
        return result;
    }
    if (frame->format.sampleFormat == AUDIO_SAMPLE_FORMAT_S16_LE) {
        const int16_t *samples = (const int16_t *)frame->data;
        const size_t sampleCount = frame->frames * frame->format.channels;
        for (size_t index = 0; index < sampleCount; ++index) {
            const int value = samples[index] < 0 ? -(int)samples[index] : (int)samples[index];
            context->pcmAbsoluteSum += (uint64_t)value;
            if (value > context->pcmPeak) {
                context->pcmPeak = value;
            }
        }
        context->pcmSamples += sampleCount;
    }
    context->playedFrames += frame->frames;
    return 0;
}

int main(int argc, char **argv) {
    const char *inputPath = argc >= 2 ? argv[1] : "audio_capture.opus";
    AudioPacketFileReader reader;
    AudioDecoder decoder;
    AudioPlaybackConfig playbackConfig;
    PlaybackContext context;
    struct sigaction signalAction;
    int result;

    memset(&reader, 0, sizeof(reader));
    memset(&decoder, 0, sizeof(decoder));
    memset(&context, 0, sizeof(context));
    memset(&signalAction, 0, sizeof(signalAction));
    signalAction.sa_handler = request_stop;
    sigaction(SIGINT, &signalAction, NULL);
    sigaction(SIGTERM, &signalAction, NULL);
    atomic_init(&g_stopRequested, false);

    result = audio_packet_file_reader_open(&reader, inputPath);
    if (result < 0) {
        fprintf(stderr, "AudioOpusPlaybackDemo: 无法打开 %s: %d\n", inputPath, result);
        return 1;
    }
    printf("AudioOpusPlaybackDemo: file=%s codec=Opus source=%uHz/%uch frame=%uus\n",
           inputPath,
           reader.info.sourceFormat.sampleRate,
           reader.info.sourceFormat.channels,
           reader.info.frameDurationUs);

    audio_playback_config_init(&playbackConfig);
    playbackConfig.requestedFormat = reader.info.sourceFormat;
    playbackConfig.requestedPeriodFrames =
        (snd_pcm_uframes_t)((uint64_t)reader.info.sourceFormat.sampleRate *
                             reader.info.frameDurationUs / 1000000ULL);
    result = audio_playback_open_auto(&context.playback, &playbackConfig);
    if (result < 0) {
        fprintf(stderr, "AudioOpusPlaybackDemo: 自动打开播放设备失败: %d\n", result);
        audio_packet_file_reader_close(&reader);
        return 1;
    }

    audio_decoder_set_pcm_callback(&decoder, play_decoded_pcm, &context);
    result = audio_decoder_init(&decoder, reader.info.codec, &reader.info.sourceFormat);
    if (result < 0) {
        fprintf(stderr, "AudioOpusPlaybackDemo: 初始化 Opus 解码器失败: %d\n", result);
        audio_playback_close(&context.playback);
        audio_packet_file_reader_close(&reader);
        return 1;
    }

    while (!atomic_load(&g_stopRequested)) {
        AudioEncodedPacket packet;
        result = audio_packet_file_reader_read(&reader, &packet);
        if (result == 0) {
            break;
        }
        if (result < 0) {
            fprintf(stderr, "AudioOpusPlaybackDemo: 读取 Opus 包失败: %d\n", result);
            break;
        }
        result = audio_decoder_decode_packet(&decoder, &packet);
        if (result < 0) {
            fprintf(stderr, "AudioOpusPlaybackDemo: 解码 Opus 包失败: %d\n", result);
            break;
        }
        ++context.decodedPackets;
        if (context.decodedPackets == 1 || context.decodedPackets % 50 == 0) {
            printf("AudioOpusPlaybackDemo: packet=%zu pts=%llu playedFrames=%zu\n",
                   context.decodedPackets,
                   (unsigned long long)packet.timestampUs,
                   context.playedFrames);
            fflush(stdout);
        }
    }

    audio_decoder_close(&decoder);
    audio_playback_close(&context.playback);
    audio_packet_file_reader_close(&reader);
    printf("AudioOpusPlaybackDemo: 已停止，解码包=%zu，播放 PCM=%zu frames，"
           "decoded meanAbs=%.1f peak=%d\n",
           context.decodedPackets,
           context.playedFrames,
           context.pcmSamples == 0 ? 0.0 :
                                        (double)context.pcmAbsoluteSum / (double)context.pcmSamples,
           context.pcmPeak);
    return result < 0 ? 1 : 0;
}
