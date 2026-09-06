#include "mpp_simple.h"
#include <errno.h>
#include <fcntl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "mpp_meta.h"
#include "rk_vdec_cmd.h"
#include "rk_venc_cmd.h"

#ifndef RKMPP_C_LOG
#define RKMPP_C_LOG 0
#endif

#if !RKMPP_C_LOG
#define printf(...) do { if (0) fprintf(stdout, __VA_ARGS__); } while (0)
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#define RKMPP_ALIGN(value, align) (((value) + (align) - 1) & ~((align) - 1))

/*
 * 这是一个“尽量容易看懂”的单线程 MPP 解码示例。
 *
 * 这次重构的重点是把“输入源”和“解码器核心”拆开：
 *
 * 1. rk_mpp_decoder_init()
 *    只负责初始化解码器。
 *
 * 2. rk_mpp_decoder_send_data()
 *    外部传入一块压缩码流 data + len + eos。
 *    这意味着以后可以很自然地换成：
 *    - 文件 fread
 *    - 网络拉流回调
 *    - 环形队列
 *    - socket 接收缓存
 *
 * 3. rk_mpp_decoder_poll_frames()
 *    尽量把当前已经可取出的 frame 全部取出来。
 *
 * 4. rk_mpp_decoder_run_file()
 *    这里只是一个“文件输入包装层”，方便继续用文件做验证。
 */

static int rk_drm_ioctl_retry(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

    return ret;
}

static int rk_drm_create_dmabuf_fd(size_t size)
{
    const char *drm_device = getenv("DRM_DEVICE");
    const char *device_path = (drm_device && drm_device[0]) ? drm_device : "/dev/dri/card0";
    struct drm_mode_create_dumb create_req;
    struct drm_prime_handle prime_req;
    int drm_fd = -1;
    int dma_fd = -1;

    if (size == 0) {
        printf("rk_drm_create_dmabuf_fd invalid size=%zu\n", size);
        return -1;
    }

    drm_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open drm device");
        return -1;
    }

    memset(&create_req, 0, sizeof(create_req));
    create_req.bpp = 8;
    create_req.width = 4096;
    create_req.height = (uint32_t)((size + create_req.width - 1) / create_req.width);

    if (rk_drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        close(drm_fd);
        return -1;
    }

    memset(&prime_req, 0, sizeof(prime_req));
    prime_req.handle = create_req.handle;
    prime_req.flags = DRM_CLOEXEC | DRM_RDWR;
    prime_req.fd = -1;

    if (rk_drm_ioctl_retry(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_req) < 0) {
        struct drm_mode_destroy_dumb destroy_req;

        perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = create_req.handle;
        rk_drm_ioctl_retry(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
        close(drm_fd);
        return -1;
    }

    dma_fd = prime_req.fd;

    close(drm_fd);
    printf("rk_drm_create_dmabuf_fd size=%zu width=%u height=%u pitch=%u alloc_size=%llu fd=%d dev=%s\n",
           size,
           create_req.width,
           create_req.height,
           create_req.pitch,
           (unsigned long long)create_req.size,
           dma_fd,
           device_path);
    return dma_fd;
}

static void rk_mpp_decoder_release_ext_dma_fds(RkMppDecoder *dec)
{
    int i;

    if (!dec)
        return;

    for (i = 0; i < RKMPP_DEC_EXT_BUF_COUNT; ++i) {
        if (dec->ext_dma_fds[i] >= 0) {
            close(dec->ext_dma_fds[i]);
            dec->ext_dma_fds[i] = -1;
        }
    }
}

static void msleep(unsigned int ms)
{
    usleep(ms * 1000);
}

static uint64_t rk_mpp_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *rk_mpp_frame_fmt_name(RK_U32 fmt)
{
    switch (fmt) {
    case MPP_FMT_YUV420SP:
        return "MPP_FMT_YUV420SP / NV12";
    case MPP_FMT_YUV420SP_VU:
        return "MPP_FMT_YUV420SP_VU / NV21";
    case MPP_FMT_YUV420P:
        return "MPP_FMT_YUV420P / I420";
    case MPP_FMT_YUV422SP:
        return "MPP_FMT_YUV422SP / NV16";
    case MPP_FMT_YUV422SP_VU:
        return "MPP_FMT_YUV422SP_VU / NV61";
    case MPP_FMT_YUV422P:
        return "MPP_FMT_YUV422P";
    case MPP_FMT_YUV422_YUYV:
        return "MPP_FMT_YUV422_YUYV / YUY2";
    case MPP_FMT_YUV422_YVYU:
        return "MPP_FMT_YUV422_YVYU";
    case MPP_FMT_YUV422_UYVY:
        return "MPP_FMT_YUV422_UYVY";
    case MPP_FMT_YUV422_VYUY:
        return "MPP_FMT_YUV422_VYUY";
    case MPP_FMT_YUV400:
        return "MPP_FMT_YUV400";
    case MPP_FMT_YUV440SP:
        return "MPP_FMT_YUV440SP";
    case MPP_FMT_YUV411SP:
        return "MPP_FMT_YUV411SP";
    case MPP_FMT_YUV444SP:
        return "MPP_FMT_YUV444SP";
    case MPP_FMT_YUV444P:
        return "MPP_FMT_YUV444P";
    case MPP_FMT_YUV420SP_10BIT:
        return "MPP_FMT_YUV420SP_10BIT";
    case MPP_FMT_YUV422SP_10BIT:
        return "MPP_FMT_YUV422SP_10BIT";
    case MPP_FMT_RGB565:
        return "MPP_FMT_RGB565";
    case MPP_FMT_BGR565:
        return "MPP_FMT_BGR565";
    case MPP_FMT_RGB555:
        return "MPP_FMT_RGB555";
    case MPP_FMT_BGR555:
        return "MPP_FMT_BGR555";
    case MPP_FMT_RGB444:
        return "MPP_FMT_RGB444";
    case MPP_FMT_BGR444:
        return "MPP_FMT_BGR444";
    case MPP_FMT_RGB888:
        return "MPP_FMT_RGB888";
    case MPP_FMT_BGR888:
        return "MPP_FMT_BGR888";
    case MPP_FMT_RGB101010:
        return "MPP_FMT_RGB101010";
    case MPP_FMT_BGR101010:
        return "MPP_FMT_BGR101010";
    case MPP_FMT_ARGB8888:
        return "MPP_FMT_ARGB8888";
    case MPP_FMT_ABGR8888:
        return "MPP_FMT_ABGR8888";
    case MPP_FMT_BGRA8888:
        return "MPP_FMT_BGRA8888";
    case MPP_FMT_RGBA8888:
        return "MPP_FMT_RGBA8888";
    default:
        return "UNKNOWN_MPP_FMT";
    }
}

/*
 * 把解码出的 frame 按 NV12 文件格式写到磁盘。
 */
static void rk_mpp_dump_frame_nv12(MppFrame frame, FILE *fp_out)
{
    MppBuffer buffer = NULL;
    MppFrameFormat fmt;
    RK_U32 width;
    RK_U32 height;
    RK_U32 h_stride;
    RK_U32 v_stride;
    RK_U8 *base = NULL;
    RK_U8 *ptr_y = NULL;
    RK_U8 *ptr_uv = NULL;
    RK_U32 y = 0;

    if (!frame || !fp_out)
        return;

    fmt = mpp_frame_get_fmt(frame);
    if (fmt != MPP_FMT_YUV420SP) {
        printf("warning: current frame fmt=%d, not NV12(MPP_FMT_YUV420SP)\n",
               fmt);
        return;
    }

    buffer = mpp_frame_get_buffer(frame);
    if (!buffer)
        return;

    width = mpp_frame_get_width(frame);
    height = mpp_frame_get_height(frame);
    h_stride = mpp_frame_get_hor_stride(frame);
    v_stride = mpp_frame_get_ver_stride(frame);
    base = (RK_U8 *)mpp_buffer_get_ptr(buffer);
    if (!base)
        return;

    ptr_y = base;
    ptr_uv = base + h_stride * v_stride;

    for (y = 0; y < height; y++)
        fwrite(ptr_y + y * h_stride, 1, width, fp_out);

    for (y = 0; y < height / 2; y++)
        fwrite(ptr_uv + y * h_stride, 1, width, fp_out);
}


/*
 * 初始化解码器核心对象。
 *
 * 这一层不关心输入来自哪里，只关心“我是不是一个准备好的解码器”。
 */
int rk_mpp_decoder_init(RkMppDecoder *dec, MppCodingType type, FILE *f_out)
{
    int i;
    RK_U32 split_parse;

    if (!dec)
        return -1;
    // if (dec->dec_initialized) {
    //     return 0;
    // }
    memset(dec, 0, sizeof(*dec));
    dec->f_out = f_out;
    dec->type = type;
    for (i = 0; i < RKMPP_DEC_EXT_BUF_COUNT; ++i)
        dec->ext_dma_fds[i] = -1;

    if (mpp_create(&dec->ctx, &dec->mpi) != MPP_OK) {
        printf("mpp_create error\n");
        goto fail;
    }

    if (mpp_init(dec->ctx, MPP_CTX_DEC, type) != MPP_OK) {
        printf("mpp_init error\n");
        goto fail;
    }

    if (mpp_packet_init(&dec->packet, NULL, 0) != MPP_OK) {
        printf("mpp_packet_init error\n");
        goto fail;
    }

    if (mpp_dec_cfg_init(&dec->dec_cfg) != MPP_OK) {
        printf("mpp_dec_cfg_init error\n");
        goto fail;
    }

    if (dec->mpi->control(dec->ctx, MPP_DEC_GET_CFG, dec->dec_cfg) != MPP_OK) {
        printf("MPP_DEC_GET_CFG error\n");
        goto fail;
    }

    split_parse = (type == MPP_VIDEO_CodingMJPEG) ? 0 : 1;
    if (mpp_dec_cfg_set_u32(dec->dec_cfg, "base:split_parse", split_parse) != MPP_OK) {
        printf("mpp_dec_cfg_set_u32 error\n");
        goto fail;
    }

    if (dec->mpi->control(dec->ctx, MPP_DEC_SET_CFG, dec->dec_cfg) != MPP_OK) {
        printf("MPP_DEC_SET_CFG error\n");
        goto fail;
    }
    dec->dec_initialized = 1;
    return 0;

fail:
    rk_mpp_decoder_deinit(dec);
    return -1;
}

void rk_mpp_decoder_set_frame_callback(RkMppDecoder *dec,
                                       RkMppFrameCallback callback,
                                       void *userdata)
{
    if (!dec)
        return;

    dec->frame_callback = callback;
    dec->frame_callback_userdata = userdata;
}

/*
 * 处理 info_change。
 *
 * 当 MPP 返回的 frame 带有 info_change 标志时，说明：
 * 1. 解码器已经知道输出宽高/stride/buf_size
 * 2. 但它还在等应用层准备输出缓冲
 */
static int rk_mpp_decoder_handle_info_change(RkMppDecoder *dec, MppFrame frame)
{
    MPP_RET ret;
    RK_U32 width = mpp_frame_get_width(frame);
    RK_U32 height = mpp_frame_get_height(frame);
    RK_U32 h_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 v_stride = mpp_frame_get_ver_stride(frame);
    RK_U32 buf_size = mpp_frame_get_buf_size(frame);
    
    printf("info_change: w=%u h=%u hs=%u vs=%u buf=%u\n",
           width, height, h_stride, v_stride, buf_size);
    


    {
        size_t required_size = (size_t)h_stride * v_stride * 2;
        int i;

        if (required_size < buf_size)
            required_size = buf_size;

        if (!dec->frm_grp) {
        ret = mpp_buffer_group_get_external(&dec->frm_grp, MPP_BUFFER_TYPE_EXT_DMA);
        if (ret) {
            printf("mpp_buffer_group_get_external failed ret=%d\n", ret);
            return -1;
        }
        } else {
            ret = mpp_buffer_group_clear(dec->frm_grp);
            if (ret) {
                printf("mpp_buffer_group_clear failed ret=%d\n", ret);
                return -1;
            }
        }

        rk_mpp_decoder_release_ext_dma_fds(dec);

        for (i = 0; i < RKMPP_DEC_EXT_BUF_COUNT; ++i) {
            MppBufferInfo info;

            dec->ext_dma_fds[i] = rk_drm_create_dmabuf_fd(required_size);
            if (dec->ext_dma_fds[i] < 0) {
                printf("rk_drm_create_dmabuf_fd failed index=%d size=%zu\n",
                       i, required_size);
                rk_mpp_decoder_release_ext_dma_fds(dec);
                return -1;
            }

            memset(&info, 0, sizeof(info));
            info.type = MPP_BUFFER_TYPE_EXT_DMA;
            info.fd = dec->ext_dma_fds[i];
            info.size = required_size;
            info.index = i;

            ret = mpp_buffer_commit(dec->frm_grp, &info);
            if (ret) {
                printf("mpp_buffer_commit failed index=%d ret=%d fd=%d size=%zu\n",
                       i, ret, info.fd, info.size);
                rk_mpp_decoder_release_ext_dma_fds(dec);
                return -1;
            }
        }

        ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_EXT_BUF_GROUP, dec->frm_grp);
        if (ret) {
            printf("MPP_DEC_SET_EXT_BUF_GROUP failed ret=%d\n", ret);
            rk_mpp_decoder_release_ext_dma_fds(dec);
            return -1;
        }
    }

    // if (!dec->frm_grp) {
    //     ret = mpp_buffer_group_get_internal(&dec->frm_grp, MPP_BUFFER_TYPE_ION);
    //     if (ret) {
    //         printf("mpp_buffer_group_get_internal failed ret=%d\n", ret);
    //         return -1;
    //     }

    //     ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_EXT_BUF_GROUP, dec->frm_grp);
    //     if (ret) {
    //         printf("MPP_DEC_SET_EXT_BUF_GROUP failed ret=%d\n", ret);
    //         return -1;
    //     }
    // } else {
    //     ret = mpp_buffer_group_clear(dec->frm_grp);
    //     if (ret) {
    //         printf("mpp_buffer_group_clear failed ret=%d\n", ret);
    //         return -1;
    //     }
    // }

    // ret = mpp_buffer_group_limit_config(dec->frm_grp, buf_size, );
    // if (ret) {
    //     printf("mpp_buffer_group_limit_config failed ret=%d\n", ret);
    //     return -1;
    // }

    ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
    if (ret) {
        printf("MPP_DEC_SET_INFO_CHANGE_READY failed ret=%d\n", ret);
        return -1;
    }

    return 0;
}

/*
 * 处理一帧输出。
 *
 * 如果是 info_change，就走缓冲准备流程；
 * 如果是正常图像帧，就统计并按 NV12 写文件。
 */
static int rk_mpp_decoder_handle_frame(RkMppDecoder *dec, MppFrame frame)
{
   
    RK_U32 fmt = mpp_frame_get_fmt(frame);
    RK_U32 width = mpp_frame_get_width(frame);
    RK_U32 height = mpp_frame_get_height(frame);
    RK_U32 h_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 v_stride = mpp_frame_get_ver_stride(frame);
    MppBuffer buf = mpp_frame_get_buffer(frame);
    RK_U32 size = mpp_frame_get_buf_size(frame);
    int fd = buf ? mpp_buffer_get_fd(buf) : -1;
    printf("[rkmpp decoder]mpp decoded frame:%p fmt:%u(%s) %ux%u stride=%ux%u buf=%p fd=%d size=%u pts=%lld\n",
       frame, fmt, rk_mpp_frame_fmt_name(fmt),
       width, height, h_stride, v_stride,
       buf, fd, size, mpp_frame_get_pts(frame));
    printf(" [rkmpp decoder]errinfo=%u discard=%d info_change=%d eos=%d\n",
       mpp_frame_get_errinfo(frame),
       mpp_frame_get_discard(frame),
       mpp_frame_get_info_change(frame),
       mpp_frame_get_eos(frame));

    if (mpp_frame_get_info_change(frame))
        return rk_mpp_decoder_handle_info_change(dec, frame);

    //printf("成功读取到一帧数据 %d\n", ++dec->frame_count);
    // if (dec->f_out && !mpp_frame_get_errinfo(frame))
    //     rk_mpp_dump_frame_nv12(frame, dec->f_out);
    printf("errinfo:%d, discard:%d\n", mpp_frame_get_errinfo(frame), mpp_frame_get_discard(frame));
    if (dec->frame_callback && !mpp_frame_get_discard(frame)) {
        MppBuffer buffer = mpp_frame_get_buffer(frame);
        const uint8_t *data = NULL;
        size_t size = 0;
        uint64_t now_ms = rk_mpp_now_ms();

        if (buffer) {
            data = (const uint8_t *)mpp_buffer_get_ptr(buffer);
            size = mpp_frame_get_buf_size(frame);
        }

        dec->frame_count++;
        if (now_ms == 0 || dec->last_output_log_ms == 0 ||
            now_ms - dec->last_output_log_ms >= 10000ULL) {
            MppDecQueryCfg query;

            memset(&query, 0, sizeof(query));
            query.query_flag = MPP_DEC_QUERY_FPS | MPP_DEC_QUERY_BPS |
                               MPP_DEC_QUERY_DEC_IN_PKT |
                               MPP_DEC_QUERY_DEC_OUT_FRM |
                               MPP_DEC_QUERY_DEC_WORK;

            if (dec->mpi->control(dec->ctx, MPP_DEC_QUERY, &query) != MPP_OK) {
                fprintf(stdout,
                        "[RKMPP Decoder] frame=%d codec=%d %ux%u stride=%ux%u fmt=%u(%s) fd=%d size=%zu pts=%lld\n",
                        dec->frame_count, dec->type, width, height, h_stride,
                        v_stride, fmt, rk_mpp_frame_fmt_name(fmt), fd, size,
                        (long long)mpp_frame_get_pts(frame));
            } else {
                fprintf(stdout,
                        "[RKMPP Decoder] frame=%d codec=%d %ux%u stride=%ux%u fmt=%u(%s) fd=%d size=%zu pts=%lld rt_fps=%u rt_bps=%u in_pkt=%u out_frm=%u hw_run=%u\n",
                        dec->frame_count, dec->type, width, height, h_stride,
                        v_stride, fmt, rk_mpp_frame_fmt_name(fmt), fd, size,
                        (long long)mpp_frame_get_pts(frame), query.rt_fps,
                        query.rt_bps, query.dec_in_pkt_cnt,
                        query.dec_out_frm_cnt, query.dec_hw_run_cnt);
            }
            dec->last_output_log_ms = now_ms;
        }

        printf("2\n");
        dec->frame_callback(data, size, fd, width, height, h_stride, v_stride, fmt,
                            mpp_frame_get_pts(frame),
                            dec->frame_callback_userdata);
    }

    return 0;
}

/*
 * 尽量把当前已经能取出来的 frame 全部取出来。
 *
 * 返回值约定：
 * 0  : 当前轮正常结束，可以继续送更多输入
 * 1  : 收到真正的 eos frame，整个解码完成
 * <0 : 出错
 */
static int rk_mpp_decoder_poll_frames(RkMppDecoder *dec)
{
    while (1) {
        MPP_RET ret;
        MppFrame frame = NULL;

        ret = dec->mpi->decode_get_frame(dec->ctx, &frame);
        //printf("调用 decode_get_frame ret=%d\n", ret);
        if (ret == MPP_ERR_TIMEOUT) {
            dec->timeout_count++;
            if (dec->eos_sent)
                dec->eos_wait_count++;
            msleep(1);
            return 0;
        }

        if (ret != MPP_OK) {
            printf("mpi->decode_get_frame error ret=%d\n", ret);
            return -1;
        }

        dec->timeout_count = 0;
        dec->eos_wait_count = 0;

        if (!frame) {
            //printf("空\n");
            return 0;
        } 
        
        printf("[RKMPP Decoder] get frame\n");
        

        if (rk_mpp_decoder_handle_frame(dec, frame)) {
            mpp_frame_deinit(&frame);
            return -1;
        }

        if (mpp_frame_get_eos(frame)) {
            printf("got eos frame, decode finished\n");
            mpp_frame_deinit(&frame);
            return 1;
        }

        mpp_frame_deinit(&frame);
    }

}

/*
 * 对外暴露的“喂一块压缩码流”的函数。
 *
 * 这一步就是从“文件 demo”过渡到“可接其他输入源”的关键。
 * 以后无论数据来自哪里，只要最后能给你：
 *   data / len / eos
 * 基本都可以接到这里。
 */
int rk_mpp_decoder_send_data_with_pts(RkMppDecoder *dec,
                                      const uint8_t *data,
                                      size_t len,
                                      int eos,
                                      RK_S64 pts_us)
{
    if (!dec) {
        printf("decoder is null\n");
        return -1;
    }

    if (len > 0 && !data) {
        printf("input data is null while len=%zu\n", len);
        return -1;
    }

    if (len > dec->internal_buf_capacity) {
        uint8_t *new_buf = (uint8_t *)realloc(dec->internal_buf, len);
        if (!new_buf) {
            fprintf(stderr, "resize decoder input buffer failed len=%zu\n", len);
            return -1;
        }
        dec->internal_buf = new_buf;
        dec->internal_buf_capacity = len;
    }

    if (len > 0)
        memcpy(dec->internal_buf, data, len);
    int pkt_done = 0;

    mpp_packet_set_data(dec->packet, dec->internal_buf);
    mpp_packet_set_pos(dec->packet, dec->internal_buf);
    mpp_packet_set_size(dec->packet, len);
    mpp_packet_set_length(dec->packet, len);
    mpp_packet_set_pts(dec->packet, pts_us);
    mpp_packet_set_dts(dec->packet, pts_us);
    mpp_packet_clr_eos(dec->packet);

    if (eos)
        mpp_packet_set_eos(dec->packet);
    //printf("pkt_done=%d\n", pkt_done);
    while (!pkt_done) {
        MPP_RET ret = dec->mpi->decode_put_packet(dec->ctx, dec->packet);
        if (ret == MPP_OK) {
            pkt_done = 1;
            if (eos)
                dec->eos_sent = 1;
            printf("packet中的数据送往解码器成功 len=%zu eos=%d pts_us=%lld\n",
                   len, eos, (long long)pts_us);
        } else {
            if (ret == MPP_ERR_BUFFER_FULL) { // MPP_ERR_BUFFER_FULL
                
                printf("解码器内部缓冲满了，无法接受新数据了\n");
                
                ret = rk_mpp_decoder_poll_frames(dec);
                if (ret < 0) {
                    printf("解码器处理已解码帧时出错 ret=%d\n", ret);
                    return -1;
                }

            } else {
                printf("packet送往解码器失败 ret=%d\n", ret);
            }
            
            msleep(1);
        }
    }

    if (eos) {
        int i;

        for (i = 0; i < 2000; ++i) {
            int ret = rk_mpp_decoder_poll_frames(dec);
            if (ret != 0)
                return ret;
            msleep(1);
        }
    }

    return rk_mpp_decoder_poll_frames(dec);
}

int rk_mpp_decoder_send_data(RkMppDecoder *dec,
                             const uint8_t *data,
                             size_t len,
                             int eos)
{
    return rk_mpp_decoder_send_data_with_pts(dec, data, len, eos, 0);
}

/*
 * 文件输入版本只是一个“外层包装”。
 *
 * 这个函数的职责非常单纯：
 * 1. fread 一块码流
 * 2. 调用 rk_mpp_decoder_send_data()
 * 3. 文件结束后进入 drain
 */
void rk_mpp_decoder_deinit(RkMppDecoder *dec)
{
    if (!dec)
        return;

    if (dec->packet)
        mpp_packet_deinit(&dec->packet);
    if (dec->dec_cfg)
        mpp_dec_cfg_deinit(dec->dec_cfg);
    if (dec->ctx) {
        if (dec->mpi)
            dec->mpi->reset(dec->ctx);
        mpp_destroy(dec->ctx);
    }
    if (dec->frm_grp)
        mpp_buffer_group_put(dec->frm_grp);
    rk_mpp_decoder_release_ext_dma_fds(dec);
    free(dec->internal_buf);
    dec->internal_buf = NULL;
    dec->internal_buf_capacity = 0;
}

static void rk_mpp_encoder_calc_buffer_size(RkMppEncoder *enc)
{
    switch (enc->fmt) {
    case MPP_FMT_YUV420SP:
    case MPP_FMT_YUV420SP_VU:
    case MPP_FMT_YUV420P:
        enc->frame_size = (size_t)enc->h_stride * enc->v_stride * 3 / 2;
        break;
    case MPP_FMT_YUV422_YUYV:
    case MPP_FMT_YUV422_YVYU:
    case MPP_FMT_YUV422_UYVY:
    case MPP_FMT_YUV422_VYUY:
        enc->frame_size = (size_t)enc->h_stride * enc->v_stride * 2;
        break;
    case MPP_FMT_RGB888:
    case MPP_FMT_BGR888:
        enc->frame_size = (size_t)enc->h_stride * enc->v_stride * 3;
        break;
    case MPP_FMT_RGBA8888:
    case MPP_FMT_BGRA8888:
    case MPP_FMT_ARGB8888:
    case MPP_FMT_ABGR8888:
        enc->frame_size = (size_t)enc->h_stride * enc->v_stride * 4;
        break;
    default:
        enc->frame_size = (size_t)enc->h_stride * enc->v_stride * 3 / 2;
        break;
    }
    enc->packet_size = enc->frame_size;
}

static int rk_mpp_encoder_emit_packet(RkMppEncoder *enc,
                                      MppPacket packet,
                                      int is_header)
{
    const uint8_t *data = NULL;
    size_t size = 0;
    int eos = 0;
    int is_intra = 0;

    if (!enc || !packet)
        return -1;

    data = (const uint8_t *)mpp_packet_get_pos(packet);
    size = mpp_packet_get_length(packet);
    eos = mpp_packet_get_eos(packet);
    if (mpp_packet_has_meta(packet)) {
        MppMeta meta = mpp_packet_get_meta(packet);
        (void)mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &is_intra);
    }

    if (size > 0 && enc->f_out)
        fwrite(data, 1, size, enc->f_out);

    if (is_header) {
        printf("encoded header size=%zu\n", size);
    } else {
        enc->pkt_eos = eos;
        enc->frame_count++;
        printf("encoded frame %d, packet size=%zu eos=%d\n",
               enc->frame_count, size, eos);
    }

    {
        uint64_t now_ms = rk_mpp_now_ms();

        if (now_ms == 0 || enc->last_packet_log_ms == 0 ||
            now_ms - enc->last_packet_log_ms >= 10000ULL) {
            MppEncQueryCfg query;

            memset(&query, 0, sizeof(query));
            query.query_flag = MPP_ENC_QUERY_FPS | MPP_ENC_QUERY_BPS |
                               MPP_ENC_QUERY_ENC_IN_FRM |
                               MPP_ENC_QUERY_ENC_OUT_PKT |
                               MPP_ENC_QUERY_ENC_WORK;

            if (enc->mpi && enc->ctx &&
                enc->mpi->control(enc->ctx, MPP_ENC_QUERY, &query) == MPP_OK) {
                fprintf(stdout,
                        "[RKMPP Encoder] frame=%d codec=%d input=%ux%u stride=%ux%u fmt=%d packet=%zu header=%d intra=%d eos=%d rt_fps=%u rt_bps=%u in_frm=%u out_pkt=%u hw_run=%u\n",
                        enc->frame_count, enc->type, enc->width, enc->height,
                        enc->h_stride, enc->v_stride, enc->fmt, size,
                        is_header, is_intra, eos, query.rt_fps, query.rt_bps,
                        query.enc_in_frm_cnt, query.enc_out_pkt_cnt,
                        query.enc_hw_run_cnt);
            } else {
                fprintf(stdout,
                        "[RKMPP Encoder] frame=%d codec=%d input=%ux%u stride=%ux%u fmt=%d packet=%zu header=%d intra=%d eos=%d\n",
                        enc->frame_count, enc->type, enc->width, enc->height,
                        enc->h_stride, enc->v_stride, enc->fmt, size,
                        is_header, is_intra, eos);
            }
            enc->last_packet_log_ms = now_ms;
        }
    }

    if (size > 0 && enc->packet_callback)
        enc->packet_callback(data, size, is_header, is_intra, eos,
                             enc->packet_callback_userdata);

    return 0;
}

static int rk_mpp_encoder_prepare(RkMppEncoder *enc)
{
    MPP_RET ret = MPP_OK;
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;

    ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret) {
        printf("mpp_create encoder failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_init(enc->ctx, MPP_CTX_ENC, enc->type);
    if (ret) {
        printf("mpp_init encoder failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_enc_cfg_init(&enc->enc_cfg);
    if (ret) {
        printf("mpp_enc_cfg_init failed ret=%d\n", ret);
        return -1;
    }

    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_CFG, enc->enc_cfg);
    if (ret) {
        printf("MPP_ENC_GET_CFG failed ret=%d\n", ret);
        return -1;
    }

    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:width", enc->width);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:height", enc->height);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:hor_stride", enc->h_stride);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:ver_stride", enc->v_stride);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "prep:format", enc->fmt);

    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_in_num", enc->fps);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_out_num", enc->fps);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:gop", enc->gop);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:bps_target", enc->bps);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:bps_max", enc->bps * 17 / 16);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:bps_min", enc->bps * 15 / 16);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_init", -1);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_max", 48);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_min", 10);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_max_i", 48);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_min_i", 10);
    mpp_enc_cfg_set_s32(enc->enc_cfg, "rc:qp_ip", 2);

    mpp_enc_cfg_set_s32(enc->enc_cfg, "codec:type", enc->type);
    if (enc->type == MPP_VIDEO_CodingAVC) {
        mpp_enc_cfg_set_s32(enc->enc_cfg, "h264:profile", 100);
        mpp_enc_cfg_set_s32(enc->enc_cfg, "h264:level", 40);
        mpp_enc_cfg_set_s32(enc->enc_cfg, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(enc->enc_cfg, "h264:trans8x8", 1);
    }

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->enc_cfg);
    if (ret) {
        printf("MPP_ENC_SET_CFG failed ret=%d\n", ret);
        return -1;
    }

    if (enc->type == MPP_VIDEO_CodingAVC || enc->type == MPP_VIDEO_CodingHEVC) {
        ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_HEADER_MODE,
                                &header_mode);
        if (ret) {
            printf("MPP_ENC_SET_HEADER_MODE failed ret=%d\n", ret);
            return -1;
        }
    }

    ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_DRM);
    if (ret) {
        printf("mpp_buffer_group_get_internal failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_buffer_get(enc->buf_grp, &enc->frm_buf, enc->frame_size);
    if (ret) {
        printf("mpp_buffer_get frm_buf failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_buffer_get(enc->buf_grp, &enc->pkt_buf, enc->packet_size);
    if (ret) {
        printf("mpp_buffer_get pkt_buf failed ret=%d\n", ret);
        return -1;
    }

    return 0;
}

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
                        FILE *f_out)
{
    if (!enc || !width || !height) {
        printf("rk_mpp_encoder_init invalid argument\n");
        return -1;
    }

    memset(enc, 0, sizeof(*enc));

    enc->type = type;
    enc->width = width;
    enc->height = height;
    enc->h_stride = h_stride ? h_stride : RKMPP_ALIGN(width, 16);
    enc->v_stride = v_stride ? v_stride : RKMPP_ALIGN(height, 16);
    enc->fmt = fmt;
    enc->fps = fps > 0 ? fps : 30;
    enc->bps = bps > 0 ? bps : (RK_S32)(width * height * enc->fps / 8);
    enc->gop = gop > 0 ? gop : enc->fps;
    enc->f_out = f_out;

    rk_mpp_encoder_calc_buffer_size(enc);
    if (rk_mpp_encoder_prepare(enc)) {
        rk_mpp_encoder_deinit(enc);
        return -1;
    }

    return 0;
}

void rk_mpp_encoder_set_packet_callback(RkMppEncoder *enc,
                                        RkMppPacketCallback callback,
                                        void *userdata)
{
    if (!enc)
        return;

    enc->packet_callback = callback;
    enc->packet_callback_userdata = userdata;
}

int rk_mpp_encoder_write_header(RkMppEncoder *enc)
{
    MPP_RET ret = MPP_OK;

    if (!enc || !enc->ctx || !enc->mpi || !enc->pkt_buf) {
        printf("rk_mpp_encoder_write_header invalid encoder state\n");
        return -1;
    }

    ret = mpp_packet_init_with_buffer(&enc->packet, enc->pkt_buf);
    if (ret) {
        printf("mpp_packet_init_with_buffer failed ret=%d\n", ret);
        return -1;
    }

    mpp_packet_set_length(enc->packet, 0);

    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_HDR_SYNC, enc->packet);
    if (ret) {
        printf("MPP_ENC_GET_HDR_SYNC failed ret=%d\n", ret);
        mpp_packet_deinit(&enc->packet);
        return -1;
    }

    rk_mpp_encoder_emit_packet(enc, enc->packet, 1);
    mpp_packet_deinit(&enc->packet);
    return 0;
}

int rk_mpp_encoder_request_idr(RkMppEncoder *enc)
{
    MPP_RET ret = MPP_OK;

    if (!enc || !enc->ctx || !enc->mpi) {
        printf("rk_mpp_encoder_request_idr invalid encoder state\n");
        return -1;
    }

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_IDR_FRAME, NULL);
    if (ret) {
        printf("MPP_ENC_SET_IDR_FRAME failed ret=%d\n", ret);
        return -1;
    }

    return 0;
}

int rk_mpp_encoder_send_frame(RkMppEncoder *enc, int fd, int eos)
{
    MPP_RET ret = MPP_OK;
    MppMeta meta = NULL;
    MppBuffer frm_buf = NULL;
    MppBufferInfo info = {0};

    if (!enc || !enc->ctx || !enc->mpi) {
        printf("invalid encoder state\n");
        return -1;
    }

    if (fd < 0) {
        printf("rk_mpp_encoder_send_frame invalid fd=%d\n", fd);
        return -1;
    }

    info.type = MPP_BUFFER_TYPE_EXT_DMA;
    info.fd = fd;
    info.size = enc->frame_size;

    ret = mpp_buffer_import(&frm_buf, &info);
    if (ret || !frm_buf) {
        printf("mpp_buffer_import failed ret=%d\n", ret);
        return -1;
    }

    ret = mpp_frame_init(&enc->frame);
    if (ret) {
        printf("mpp_frame_init failed ret=%d\n", ret);
        mpp_buffer_put(frm_buf);
        return -1;
    }

    mpp_frame_set_width(enc->frame, enc->width);
    mpp_frame_set_height(enc->frame, enc->height);
    mpp_frame_set_hor_stride(enc->frame, enc->h_stride);
    mpp_frame_set_ver_stride(enc->frame, enc->v_stride);
    mpp_frame_set_fmt(enc->frame, enc->fmt);
    mpp_frame_set_eos(enc->frame, eos ? 1 : 0);
    mpp_frame_set_buffer(enc->frame, frm_buf);

    meta = mpp_frame_get_meta(enc->frame);

    ret = mpp_packet_init_with_buffer(&enc->packet, enc->pkt_buf);
    if (ret) {
        printf("mpp_packet_init_with_buffer failed ret=%d\n", ret);
        mpp_frame_deinit(&enc->frame);
        mpp_buffer_put(frm_buf);
        return -1;
    }

    mpp_packet_set_length(enc->packet, 0);
    mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, enc->packet);

    ret = enc->mpi->encode_put_frame(enc->ctx, enc->frame);
    if (ret) {
        printf("encode_put_frame failed ret=%d\n", ret);
        mpp_packet_deinit(&enc->packet);
        mpp_frame_deinit(&enc->frame);
        mpp_buffer_put(frm_buf);
        return -1;
    }

    mpp_frame_deinit(&enc->frame);

    ret = enc->mpi->encode_get_packet(enc->ctx, &enc->packet);
    if (ret) {
        printf("encode_get_packet failed ret=%d\n", ret);
        mpp_packet_deinit(&enc->packet);
        mpp_buffer_put(frm_buf);
        return -1;
    }

    if (enc->packet) {
        rk_mpp_encoder_emit_packet(enc, enc->packet, 0);
        mpp_packet_deinit(&enc->packet);
    }

    mpp_buffer_put(frm_buf);

    if (eos)
        enc->eos_sent = 1;

    return 0;
}

void rk_mpp_encoder_deinit(RkMppEncoder *enc)
{
    if (!enc)
        return;

    if (enc->packet)
        mpp_packet_deinit(&enc->packet);
    if (enc->frame)
        mpp_frame_deinit(&enc->frame);
    if (enc->enc_cfg)
        mpp_enc_cfg_deinit(enc->enc_cfg);
    if (enc->frm_buf)
        mpp_buffer_put(enc->frm_buf);
    if (enc->pkt_buf)
        mpp_buffer_put(enc->pkt_buf);
    if (enc->buf_grp)
        mpp_buffer_group_put(enc->buf_grp);
    if (enc->ctx) {
        if (enc->mpi)
            enc->mpi->reset(enc->ctx);
        mpp_destroy(enc->ctx);
    }

    memset(enc, 0, sizeof(*enc));
}
