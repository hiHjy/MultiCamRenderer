#ifndef __MPP_SIMPLE_H__
#define __MPP_SIMPLE_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_vdec_cfg.h"
#include "rk_venc_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RKMPP_DEC_EXT_BUF_COUNT 32

typedef void (*RkMppFrameCallback)(const uint8_t *data,
                                   size_t size,
                                   int fd,
                                   RK_U32 width,
                                   RK_U32 height,
                                   RK_U32 h_stride,
                                   RK_U32 v_stride,
                                   RK_U32 fmt,
                                   RK_S64 pts_us,
                                   void *userdata);

typedef struct RkMppDecoder_t {
    MppCtx ctx;
    MppApi *mpi;
    MppPacket packet;
    MppDecCfg dec_cfg;
    MppBufferGroup frm_grp;
    FILE *f_out;
    int frame_count;
    int timeout_count;
    int eos_wait_count;
    int eos_sent;
    MppCodingType type;
    RkMppFrameCallback frame_callback;
    void *frame_callback_userdata;
    int dec_initialized;
    int ext_dma_fds[RKMPP_DEC_EXT_BUF_COUNT];
    uint8_t *internal_buf;
    size_t internal_buf_capacity;
    uint64_t last_output_log_ms;
} RkMppDecoder;

int rk_mpp_decoder_init(RkMppDecoder *dec, MppCodingType type, FILE *f_out);
void rk_mpp_decoder_set_frame_callback(RkMppDecoder *dec,
                                       RkMppFrameCallback callback,
                                       void *userdata);
int rk_mpp_decoder_send_data_with_pts(RkMppDecoder *dec,
                                      const uint8_t *data,
                                      size_t len,
                                      int eos,
                                      RK_S64 pts_us);
int rk_mpp_decoder_send_data(RkMppDecoder *dec,
                             const uint8_t *data,
                             size_t len,
                             int eos);
void rk_mpp_decoder_deinit(RkMppDecoder *dec);

typedef void (*RkMppPacketCallback)(const uint8_t *data,
                                    size_t size,
                                    int is_header,
                                    int is_intra,
                                    int eos,
                                    void *userdata);

typedef struct RkMppEncoder_t {
    MppCtx ctx;
    MppApi *mpi;
    MppEncCfg enc_cfg;
    MppFrame frame;
    MppPacket packet;

    MppBufferGroup buf_grp;
    MppBuffer frm_buf;
    MppBuffer pkt_buf;

    FILE *f_out;
    int frame_count;
    int eos_sent;
    int pkt_eos;

    RkMppPacketCallback packet_callback;
    void *packet_callback_userdata;

    RK_U32 width;
    RK_U32 height;
    RK_U32 h_stride;
    RK_U32 v_stride;
    MppFrameFormat fmt;
    MppCodingType type;
    RK_S32 fps;
    RK_S32 bps;
    RK_S32 gop;
    size_t frame_size;
    size_t packet_size;
    uint64_t last_packet_log_ms;
} RkMppEncoder;

int rk_mpp_encoder_init(RkMppEncoder *enc,
                        MppCodingType type,
                        RK_U32 width,
                        RK_U32 height,
                        RK_U32 h_stride,
                        RK_U32 v_stride,
                        MppFrameFormat fmt,
                        RK_S32 fps,
                        RK_S32 bps,
                        RK_S32 gop,
                        FILE *f_out);
void rk_mpp_encoder_set_packet_callback(RkMppEncoder *enc,
                                        RkMppPacketCallback callback,
                                        void *userdata);
int rk_mpp_encoder_write_header(RkMppEncoder *enc);
int rk_mpp_encoder_request_idr(RkMppEncoder *enc);
int rk_mpp_encoder_send_frame(RkMppEncoder *enc, int fd, int eos);
void rk_mpp_encoder_deinit(RkMppEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif  // __MPP_SIMPLE_H__
