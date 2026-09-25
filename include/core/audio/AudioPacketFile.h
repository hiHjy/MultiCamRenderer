#ifndef MCR_AUDIO_PACKET_FILE_H
#define MCR_AUDIO_PACKET_FILE_H

#include "AudioTypes.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 仅供 demo 使用的简单压缩音频 packet 文件，不是 Ogg/MP4 容器：
 * 文件头记录 codec/PCM 格式与准确帧样本数；每个 record 保存 PTS、包长与原始 access unit。
 * 它用于验证 AudioPipeline 编解码闭环，正式 RTSP 不依赖该格式。
 */
typedef struct AudioPacketFileInfo {
    /* 文件内 access unit 的编码类型，当前支持 Opus 与 AAC-LC。 */
    AudioCodec codec;
    /* 生成这些压缩包时的 PCM 格式。 */
    AudioPcmFormat sourceFormat;
    /* 每个压缩包覆盖的准确每声道 sample 数；AAC-LC 为 1024。 */
    uint32_t frameSamples;
    /* frameSamples 换算得到的近似微秒时长，兼容旧 Opus 测试文件。 */
    uint32_t frameDurationUs;
} AudioPacketFileInfo;

typedef struct AudioPacketFileWriter {
    FILE *file;
    AudioPacketFileInfo info;
} AudioPacketFileWriter;

typedef struct AudioPacketFileReader {
    FILE *file;
    AudioPacketFileInfo info;
    uint8_t *packetBuffer;
    size_t packetBufferCapacity;
} AudioPacketFileReader;

int audio_packet_file_writer_open(AudioPacketFileWriter *writer,
                                  const char *path,
                                  const AudioPacketFileInfo *info);
int audio_packet_file_writer_write(AudioPacketFileWriter *writer,
                                   const AudioEncodedPacket *packet);
void audio_packet_file_writer_close(AudioPacketFileWriter *writer);

int audio_packet_file_reader_open(AudioPacketFileReader *reader, const char *path);
/* 返回 1 表示读到一包，0 表示正常 EOF，负数表示文件损坏或 I/O 错误。 */
int audio_packet_file_reader_read(AudioPacketFileReader *reader, AudioEncodedPacket *packet);
void audio_packet_file_reader_close(AudioPacketFileReader *reader);

#ifdef __cplusplus
}
#endif

#endif
