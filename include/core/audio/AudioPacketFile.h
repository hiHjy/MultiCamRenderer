#ifndef MCR_AUDIO_PACKET_FILE_H
#define MCR_AUDIO_PACKET_FILE_H

#include "AudioTypes.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 仅供 demo 使用的简单 Opus packet 文件，不是 Ogg/MP4 容器：
 * 文件头记录 codec/PCM 格式；每个 record 保存 PTS、包长与原始编码 payload。
 * 这样可以无歧义重放 Opus 包，且后续 RTSP 接入不会依赖该格式。
 */
typedef struct AudioPacketFileInfo {
    AudioCodec codec;
    AudioPcmFormat sourceFormat;
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
