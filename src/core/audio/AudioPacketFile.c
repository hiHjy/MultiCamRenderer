#include "AudioPacketFile.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {
    kAudioPacketFileVersion = 2,
    kMaximumPacketBytes = 1024 * 1024,
};

typedef struct AudioPacketFileHeaderV1 {
    char magic[8];
    uint32_t version;
    uint32_t codec;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t sampleFormat;
    uint32_t frameDurationUs;
} AudioPacketFileHeaderV1;

typedef struct AudioPacketFileHeaderV2 {
    char magic[8];
    uint32_t version;
    uint32_t codec;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t sampleFormat;
    uint32_t frameSamples;
    uint32_t frameDurationUs;
} AudioPacketFileHeaderV2;

typedef struct AudioPacketRecordHeader {
    uint64_t timestampUs;
    uint32_t size;
} AudioPacketRecordHeader;

static const char kAudioPacketFileMagicV1[8] = {'M', 'C', 'R', 'O', 'P', 'U', 'S', '1'};
static const char kAudioPacketFileMagicV2[8] = {'M', 'C', 'R', 'A', 'U', 'D', '0', '2'};

static int is_supported_codec(AudioCodec codec) {
    return codec == AUDIO_CODEC_OPUS || codec == AUDIO_CODEC_AAC;
}

int audio_packet_file_writer_open(AudioPacketFileWriter *writer,
                                  const char *path,
                                  const AudioPacketFileInfo *info) {
    AudioPacketFileHeaderV2 header;

    if (writer == NULL || path == NULL || info == NULL || !is_supported_codec(info->codec) ||
        info->sourceFormat.sampleRate == 0 || info->sourceFormat.channels == 0 ||
        info->sourceFormat.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE
        || (info->frameSamples == 0 && info->frameDurationUs == 0)) {
        return -EINVAL;
    }
    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, "wb");
    if (writer->file == NULL) {
        return -errno;
    }

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, kAudioPacketFileMagicV2, sizeof(header.magic));
    header.version = kAudioPacketFileVersion;
    header.codec = (uint32_t)info->codec;
    header.sampleRate = info->sourceFormat.sampleRate;
    header.channels = info->sourceFormat.channels;
    header.sampleFormat = (uint32_t)info->sourceFormat.sampleFormat;
    header.frameSamples = info->frameSamples;
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
    char magic[8];
    AudioPacketFileHeaderV1 headerV1;
    AudioPacketFileHeaderV2 headerV2;

    if (reader == NULL || path == NULL) {
        return -EINVAL;
    }
    memset(reader, 0, sizeof(*reader));
    reader->file = fopen(path, "rb");
    if (reader->file == NULL) {
        return -errno;
    }
    if (fread(magic, sizeof(magic), 1, reader->file) != 1) {
        audio_packet_file_reader_close(reader);
        return -EINVAL;
    }
    rewind(reader->file);
    if (memcmp(magic, kAudioPacketFileMagicV1, sizeof(magic)) == 0) {
        if (fread(&headerV1, sizeof(headerV1), 1, reader->file) != 1
            || headerV1.version != 1 || headerV1.codec != AUDIO_CODEC_OPUS) {
            audio_packet_file_reader_close(reader);
            return -EINVAL;
        }
        reader->info.codec = (AudioCodec)headerV1.codec;
        reader->info.sourceFormat.sampleRate = headerV1.sampleRate;
        reader->info.sourceFormat.channels = (uint16_t)headerV1.channels;
        reader->info.sourceFormat.sampleFormat = (AudioSampleFormat)headerV1.sampleFormat;
        reader->info.frameDurationUs = headerV1.frameDurationUs;
        reader->info.frameSamples = (uint32_t)((uint64_t)headerV1.frameDurationUs
                                                * headerV1.sampleRate / 1000000ULL);
    } else if (memcmp(magic, kAudioPacketFileMagicV2, sizeof(magic)) == 0) {
        if (fread(&headerV2, sizeof(headerV2), 1, reader->file) != 1
            || headerV2.version != kAudioPacketFileVersion || !is_supported_codec((AudioCodec)headerV2.codec)) {
            audio_packet_file_reader_close(reader);
            return -EINVAL;
        }
        reader->info.codec = (AudioCodec)headerV2.codec;
        reader->info.sourceFormat.sampleRate = headerV2.sampleRate;
        reader->info.sourceFormat.channels = (uint16_t)headerV2.channels;
        reader->info.sourceFormat.sampleFormat = (AudioSampleFormat)headerV2.sampleFormat;
        reader->info.frameSamples = headerV2.frameSamples;
        reader->info.frameDurationUs = headerV2.frameDurationUs;
    } else {
        audio_packet_file_reader_close(reader);
        return -EINVAL;
    }
    if (reader->info.sourceFormat.sampleRate == 0 || reader->info.sourceFormat.channels == 0
        || reader->info.sourceFormat.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE
        || (reader->info.frameSamples == 0 && reader->info.frameDurationUs == 0)) {
        audio_packet_file_reader_close(reader);
        return -EINVAL;
    }
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
    packet->frameSamples = reader->info.frameSamples;
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
