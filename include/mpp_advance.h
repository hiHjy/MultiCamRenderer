#ifndef MPP_ADVANCE_H
#define MPP_ADVANCE_H

#include <stdio.h>
#include <string.h>

#include <rk_mpi.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#include <rk_vdec_cfg.h>

#define MPP_OUTPUT_SLOT_COUNT 4

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mpp_decoded_frame_callback_t)(int fd,
                                             RK_U32 width,
                                             RK_U32 height,
                                             RK_U32 hor_stride,
                                             RK_U32 ver_stride,
                                             size_t size,
                                             void *userdata);

typedef struct RkMppInputPacket {
    int fd;
    size_t capacity;
    size_t packet_size;
} RkMppInputPacket;

typedef struct RkMppOutputFrame {
    int fd;
    size_t capacity;
    int width;
    int height;
    int stride;
    int height_stride;
} RkMppOutputFrame;

typedef struct MppDecoderAdvance {
    MppCtx dec_ctx;
    MppApi *dec_api;
   
    MppBuffer in_buf;
    MppBuffer out_buf;
    MppBufferInfo in_buf_info;
    MppBufferInfo out_buf_info;
    MppCodingType coding;
    int dec_initialized;
    int buf_is_init;
    int dst_fd[MPP_OUTPUT_SLOT_COUNT];
    size_t dst_size[MPP_OUTPUT_SLOT_COUNT];
    mpp_decoded_frame_callback_t frame_callback;
    void *frame_callback_userdata;
} MppDecoderAdvance;

int dump_nv12_frame_from_dmafd(const char *path,
                               int fd,
                               RK_U32 width,
                               RK_U32 height,
                               RK_U32 hor_stride,
                               RK_U32 ver_stride);
void rk_mpp_decoder_advance_set_frame_callback(
    MppDecoderAdvance *ctx,
    mpp_decoded_frame_callback_t callback,
    void *userdata);
int rk_mpp_decoder_advance_init(MppDecoderAdvance *ctx, MppCodingType coding);
void rk_mpp_decoder_advance_deinit(MppDecoderAdvance *ctx);
int rk_mpp_decoder_advance_do_task(MppDecoderAdvance *ctx,
                                   const RkMppInputPacket *input,
                                   const RkMppOutputFrame *output);

#ifdef __cplusplus
}
#endif
#endif
