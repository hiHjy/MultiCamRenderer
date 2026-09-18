#include "AudioPacketFile.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {
    kAudioPacketFileVersion = 1,
    kMaximumPacketBytes = 1024 * 1024,
};

typedef struct AudioPacketFileHeader {
    char magic[8];
    uint32_t version;
    uint32_t codec;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t sampleFormat;
    uint32_t frameDurationUs;
} AudioPacketFileHeader;

typedef struct AudioPacketRecordHeader {
    uint64_t timestampUs;
    uint32_t size;
} AudioPacketRecordHeader;

static const char kAudioPacketFileMagic[8] = {'M', 'C', 'R', 'O', 'P', 'U', 'S', '1'};

int audio_packet_file_writer_open(AudioPacketFileWriter *writer,
                                  const char *path,
                                  const AudioPacketFileInfo *info) {
    AudioPacketFileHeader header;

    if (writer == NULL || path == NULL || info == NULL || info->codec != AUDIO_CODEC_OPUS ||
        info->sourceFormat.sampleRate == 0 || info->sourceFormat.channels == 0 ||
        info->sourceFormat.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE || info->frameDurationUs == 0) {
        return -EINVAL;
    }
    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, "wb");
    if (writer->file == NULL) {
        return -errno;
    }

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, kAudioPacketFileMagic, sizeof(header.magic));
    header.version = kAudioPacketFileVersion;
    header.codec = (uint32_t)info->codec;
    header.sampleRate = info->sourceFormat.sampleRate;
    header.channels = info->sourceFormat.channels;
    header.sampleFormat = (uint32_t)info->sourceFormat.sampleFormat;
    header.frameDurationUs = info->frameDurationUs;
    if (fwrite(&header, sizeof(header), 1, writer->file) != 1) {
        audio_packet_file_writer_close(writer);
        return -EIO;
    }
    writer->info = *info;
    return 0;
}

int audio_packet_file_writer_write(AudioPacketFileWriter *writer,
                                   const AudioEncodedPacket *packet) {
    AudioPacketRecordHeader recordHeader;

    if (writer == NULL || writer->file == NULL || packet == NULL || packet->data == NULL ||
        packet->codec != writer->info.codec || packet->size == 0 || packet->size > UINT32_MAX) {
        return -EINVAL;
    }
    recordHeader.timestampUs = packet->timestampUs;
    recordHeader.size = (uint32_t)packet->size;
    if (fwrite(&recordHeader, sizeof(recordHeader), 1, writer->file) != 1 ||
        fwrite(packet->data, 1, packet->size, writer->file) != packet->size) {
        return -EIO;
    }
    return 0;
}

void audio_packet_file_writer_close(AudioPacketFileWriter *writer) {
    if (writer == NULL) {
        return;
    }
    if (writer->file != NULL) {
        fclose(writer->file);
    }
    memset(writer, 0, sizeof(*writer));
}

int audio_packet_file_reader_open(AudioPacketFileReader *reader, const char *path) {
    AudioPacketFileHeader header;

    if (reader == NULL || path == NULL) {
        return -EINVAL;
    }
    memset(reader, 0, sizeof(*reader));
    reader->file = fopen(path, "rb");
    if (reader->file == NULL) {
        return -errno;
    }
    if (fread(&header, sizeof(header), 1, reader->file) != 1 ||
        memcmp(header.magic, kAudioPacketFileMagic, sizeof(header.magic)) != 0 ||
        header.version != kAudioPacketFileVersion || header.codec != AUDIO_CODEC_OPUS ||
        header.sampleRate == 0 || header.channels == 0 ||
        header.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE || header.frameDurationUs == 0) {
        audio_packet_file_reader_close(reader);
        return -EINVAL;
    }
    reader->info.codec = (AudioCodec)header.codec;
    reader->info.sourceFormat.sampleRate = header.sampleRate;
    reader->info.sourceFormat.channels = (uint16_t)header.channels;
    reader->info.sourceFormat.sampleFormat = (AudioSampleFormat)header.sampleFormat;
    reader->info.frameDurationUs = header.frameDurationUs;
    return 0;
}

int audio_packet_file_reader_read(AudioPacketFileReader *reader, AudioEncodedPacket *packet) {
    AudioPacketRecordHeader recordHeader;

    if (reader == NULL || reader->file == NULL || packet == NULL) {
        return -EINVAL;
    }
    if (fread(&recordHeader, sizeof(recordHeader), 1, reader->file) != 1) {
        return feof(reader->file) ? 0 : -EIO;
    }
    if (recordHeader.size == 0 || recordHeader.size > kMaximumPacketBytes) {
        return -EINVAL;
    }
    if (reader->packetBufferCapacity < recordHeader.size) {
        uint8_t *newBuffer = (uint8_t *)realloc(reader->packetBuffer, recordHeader.size);
        if (newBuffer == NULL) {
            return -ENOMEM;
        }
        reader->packetBuffer = newBuffer;
        reader->packetBufferCapacity = recordHeader.size;
    }
    if (fread(reader->packetBuffer, 1, recordHeader.size, reader->file) != recordHeader.size) {
        return -EIO;
    }

    packet->codec = reader->info.codec;
    packet->data = reader->packetBuffer;
    packet->size = recordHeader.size;
    packet->timestampUs = recordHeader.timestampUs;
    packet->durationUs = reader->info.frameDurationUs;
    packet->sourceFormat = reader->info.sourceFormat;
    return 1;
}

void audio_packet_file_reader_close(AudioPacketFileReader *reader) {
    if (reader == NULL) {
        return;
    }
    if (reader->file != NULL) {
        fclose(reader->file);
    }
    free(reader->packetBuffer);
    memset(reader, 0, sizeof(*reader));
}
