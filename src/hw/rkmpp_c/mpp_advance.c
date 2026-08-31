#include "mpp_advance.h"

#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <linux/dma-buf.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#ifndef RKMPP_C_LOG
#define RKMPP_C_LOG 0
#endif

#if !RKMPP_C_LOG
#define printf(...) do { if (0) fprintf(stdout, __VA_ARGS__); } while (0)
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef MY_DMA_HEAP_H
#define MY_DMA_HEAP_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DMA_HEAP_VALID_FD_FLAGS (O_CLOEXEC | O_ACCMODE)
#define DMA_HEAP_VALID_HEAP_FLAGS (0ULL)

struct dma_heap_allocation_data
{
	__u64 len;
	__u32 fd;
	__u32 fd_flags;
	__u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC                                                   \
	_IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

#endif

/*
 * 这个文件负责“把一帧 MJPEG 解码成一帧 NV12”。
 *
 * 你可以把它分成 4 层来理解：
 *
 * 第 1 层：工具函数
 * - align_to()
 * - mjpeg_parse_size_from_memory()
 * - dump_nv12_frame_from_dmafd()
 *
 * 第 2 层：输入 / 输出 buffer 准备
 * - mjpeg_get_frame_info_from_dmafd()
 * - decoder_prepare_output_dmabuf()
 *
 * 第 3 层：解码器生命周期
 * - rk_mpp_decoder_advance_init()
 * - rk_mpp_decoder_advance_deinit()
 *
 * 第 4 层：真正执行一帧解码
 * - rk_mpp_decoder_advance_do_task()
 *
 * 学这个文件时，最重要的是先搞清 4 个对象：
 * - MppBuffer: 一块实际内存
 * - MppPacket: “压缩码流”的描述对象
 * - MppFrame : “解码输出图像”的描述对象
 * - MppTask  : MPP task 模式下，一次提交给硬件的任务容器
 */
static RK_U32 align_to(RK_U32 value, RK_U32 alignment)
{
	/* 图像硬件通常要求 stride 对齐到 16 / 64 / 128 等边界。 */
	return (value + alignment - 1) & ~(alignment - 1);
}

/*
 * 这里补一套和 RGA 那边同风格的 dma-buf 显式同步。
 *
 * 目的不是说“MPP 一定要求这样才能工作”，而是把链路做成统一模型：
 * - 读输入前显式声明 READ
 * - 写输出前显式声明 WRITE
 * - 用完后再 END
 *
 * 这样我们就能把“MPP 这一层是否存在 buffer 可见性问题”也排查掉，
 * 不必只靠 usleep 去掩盖时序。
 */
static void dmabuf_sync_start(int fd, unsigned long rw)
{
	struct dma_buf_sync sync = {
		.flags = DMA_BUF_SYNC_START | rw,
	};

	if (fd >= 0) {
		(void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	}
}

static void dmabuf_sync_end(int fd, unsigned long rw)
{
	struct dma_buf_sync sync = {
		.flags = DMA_BUF_SYNC_END | rw,
	};

	if (fd >= 0) {
		(void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	}
}

static int is_jpeg_sof_marker(RK_U8 marker)
{
	switch (marker) {
		case 0xC0:
		case 0xC1:
		case 0xC2:
		case 0xC3:
		case 0xC5:
		case 0xC6:
		case 0xC7:
		case 0xC9:
		case 0xCA:
		case 0xCB:
		case 0xCD:
		case 0xCE:
		case 0xCF:
			return 1;
		default:
			return 0;
	}
}

static int mjpeg_parse_size_from_memory(const RK_U8 *data, size_t len,
										RK_U32 *width, RK_U32 *height)
{
	/*
	 * JPEG / MJPEG 是压缩流，宽高并不是通过 stride 直接可知，
	 * 需要从 JPEG 头里的 SOF（Start Of Frame）段解析出来。
	 *
	 * 这个函数只做一件事：
	 * 从“一整帧 JPEG 码流”里提取 width / height。
	 *
	 * 为什么值得学这个函数：
	 * 因为它让你看到，视频链路里“码流层信息”和“像素层信息”是两回事。
	 */
	size_t pos = 0;

	if (data == NULL || len < 4 || width == NULL || height == NULL) {
		return -1;
	}

	if (data[0] != 0xFF || data[1] != 0xD8) {
		return -1;
	}

	pos = 2;
	while (pos + 3 < len) {
		RK_U8 marker;
		size_t seg_len;

		while (pos < len && data[pos] != 0xFF) {
			pos++;
		}
		while (pos < len && data[pos] == 0xFF) {
			pos++;
		}

		if (pos >= len) {
			break;
		}

		marker = data[pos++];
		if (marker == 0xD8 || marker == 0x01) {
			continue;
		}
		if (marker == 0xD9 || marker == 0xDA) {
			break;
		}
		if (pos + 1 >= len) {
			break;
		}

		seg_len = ((size_t)data[pos] << 8) | data[pos + 1];
		pos += 2;
		if (seg_len < 2 || pos + seg_len - 2 > len) {
			break;
		}

		if (is_jpeg_sof_marker(marker)) {
			if (seg_len < 7) {
				return -1;
			}

			*height = ((RK_U32)data[pos + 1] << 8) | data[pos + 2];
			*width = ((RK_U32)data[pos + 3] << 8) | data[pos + 4];
			return 0;
		}

		pos += seg_len - 2;
	}

	return -1;
}

static int alloc_dmabuf_fd_from_drm(size_t size)
{
	/*
	 * 这个函数专门做一件事：
	 * 用 DRM dumb buffer 申请一块内存，然后再导出成 dma-buf fd。
	 *
	 * 这样你就能测试：
	 * “来自 DRM 的 fd，能不能直接给 MPP 输出 frame 用”。
	 *
	 * 这里故意把原来的 dma_heap 方案保留着，不删；
	 * 只是当前测试时，把输出分配切到 DRM 这条路径。
	 */
	int drm_fd;
	int dma_fd = -1;
	struct drm_mode_create_dumb create = {0};
	struct drm_prime_handle prime = {0};

	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("open drm card0");
		return -1;
	}

	/*
	 * 这里只是为了拿到“足够大的可导出内存”，不是真的拿它直接显示。
	 * 之前把 width 直接设成 size，某些驱动会因为 dumb buffer
	 * 宽度太夸张而不稳定。
	 *
	 * 这里改成更像普通二维 buffer 的申请方式：
	 * - 宽度固定成 4096 字节
	 * - 高度按总字节数向上取整
	 *
	 * 这样 pitch / size 还是足够大，但更容易被 DRM 驱动接受。
	 */
	create.width = 4096;
	create.height = (uint32_t)((size + create.width - 1) / create.width);
	create.bpp = 8;

	if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		close(drm_fd);
		return -1;
	}

	prime.handle = create.handle;
	prime.flags = DRM_CLOEXEC | DRM_RDWR;
	if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
		struct drm_mode_destroy_dumb destroy = {0};
		perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
		destroy.handle = create.handle;
		ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		close(drm_fd);
		return -1;
	}

	dma_fd = prime.fd;
	printf("[MPP][DRM] dumb buffer exported: drm_fd=%d handle=%u pitch=%u "
		   "size=%llu dma_fd=%d\n",
		   drm_fd, create.handle, create.pitch, (unsigned long long)create.size,
		   dma_fd);

	/*
	 * 这里保留 dumb handle，不主动 destroy，先以“可用性测试优先”。
	 * 如果后面确认链路没问题，再继续做更规范的句柄回收管理。
	 */
	close(drm_fd);
	return dma_fd;
}

static int alloc_dmabuf_fd(size_t size)
{
	const char *heap_devices[] = {
		"/dev/dma_heap/system-uncached",
		"/dev/dma_heap/system",
		"/dev/dma_heap/cma",
		"/dev/dma_heap/linux,cma",
		"/dev/dma_heap/reserved",
	};

	int heap_fd = -1;
	int dma_fd = -1;
	struct dma_heap_allocation_data alloc = {
		.len = size,
		.fd = 0,
		.fd_flags = O_CLOEXEC | O_RDWR,
		.heap_flags = 0,
	};
	size_t i;

	for (i = 0; i < sizeof(heap_devices) / sizeof(heap_devices[0]); ++i) {
		heap_fd = open(heap_devices[i], O_RDWR | O_CLOEXEC);
		if (heap_fd >= 0) {
			break;
		}
	}

	if (heap_fd < 0) {
		perror("open dma_heap");
		return -1;
	}

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		perror("DMA_HEAP_IOCTL_ALLOC");
		close(heap_fd);
		return -1;
	}

	dma_fd = alloc.fd;
	close(heap_fd);
	return dma_fd;
}

static int decoder_prepare_output_dmabuf(MppDecoderAdvance *ctx, int index,
										 size_t required_size)
{
	/*
	 * 这一层做的是：确保“解码输出目标”这块 dma-buf 足够大。
	 *
	 * 如果之前已经有一块 fd，且容量够，就复用；
	 * 不够就重新申请一块更大的。
	 *
	 * 现在这里不再是“唯一一块输出 buffer”，
	 * 而是“每个 v4l2 index 对应自己的一块输出 buffer”。
	 */
	int new_fd;

	if (index < 0 || index >= MPP_OUTPUT_SLOT_COUNT) {
		fprintf(stderr, "[MPP] invalid output slot index=%d\n", index);
		return -1;
	}

	printf("[MPP][OUT] prepare slot index=%d dec_initialized=%d current_fd=%d "
		   "current_size=%zu required_size=%zu\n",
		   index, ctx->dec_initialized, ctx->dst_fd[index],
		   ctx->dst_size[index], required_size);

	if (ctx->dst_fd[index] >= 0 && ctx->dst_size[index] >= required_size) {
		printf("[MPP][OUT] reuse slot index=%d fd=%d size=%zu\n", index,
			   ctx->dst_fd[index], ctx->dst_size[index]);
		return 0;
	}

	printf("[MPP][OUT] allocate slot index=%d reason=%s%s current_fd=%d "
		   "current_size=%zu required_size=%zu\n",
		   index, ctx->dst_fd[index] < 0 ? "empty" : "",
		   (ctx->dst_fd[index] >= 0 && ctx->dst_size[index] < required_size)
			   ? "size_insufficient"
			   : "",
		   ctx->dst_fd[index], ctx->dst_size[index], required_size);

	/*
	 * 优先走 DRM dumb buffer 导出 dma-buf 的方案。
	 * 这样可以避免继续挤占 /dev/dma_heap/cma，适合现在这个
	 * “摄像头输入 + DRM显示 + MPP输出” 都在吃 CMA 的场景。
	 *
	 * 如果 DRM 路径失败，再退回 dma_heap，方便继续排查。
	 */
	new_fd = alloc_dmabuf_fd_from_drm(required_size);
	if (new_fd < 0) {
		fprintf(stderr,
				"[MPP] drm dumb export failed for index=%d size=%zu, fallback "
				"to dma_heap\n",
				index, required_size);
		new_fd = alloc_dmabuf_fd(required_size);
	}
	if (new_fd < 0) {
		return -1;
	}

	printf("[MPP] prepared output slot index=%d fd=%d size=%zu\n", index,
		   new_fd, required_size);

	if (ctx->dst_fd[index] >= 0) {
		printf("[MPP][OUT] close old slot index=%d old_fd=%d old_size=%zu\n",
			   index, ctx->dst_fd[index], ctx->dst_size[index]);
		close(ctx->dst_fd[index]);
	}

	ctx->dst_fd[index] = new_fd;
	ctx->dst_size[index] = required_size;
	return 0;
	printf("[MPP] 输出槽子准备完成\n");
}

int mjpeg_get_frame_info_from_dmafd(int fd, size_t packet_size,
									FrameInfo *info)
{
	/*
	 * 这一步很关键：
	 *
	 * 输入进来的 fd 里装的是“压缩的 MJPEG 数据”，不是解码后的图像。
	 * 所以我们先 mmap 这块输入 dma-buf，把 JPEG 头读出来，
	 * 再推导出后面解码输出需要的 width / height / stride / buffer_size。
	 */
	void *addr;
	RK_U32 width = 0;
	RK_U32 height = 0;

	if (fd < 0 || packet_size == 0 || info == NULL) {
		return -1;
	}

	dmabuf_sync_start(fd, DMA_BUF_SYNC_READ);

	addr = mmap(NULL, packet_size, PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		perror("mmap mjpeg dmafd");
		dmabuf_sync_end(fd, DMA_BUF_SYNC_READ);
		return -1;
	}

	if (mjpeg_parse_size_from_memory((const RK_U8 *)addr, packet_size, &width,
									 &height) != 0) {
		munmap(addr, packet_size);
		dmabuf_sync_end(fd, DMA_BUF_SYNC_READ);
		return -1;
	}

	munmap(addr, packet_size);
	dmabuf_sync_end(fd, DMA_BUF_SYNC_READ);

	memset(info, 0, sizeof(*info));
	info->width = width;
	info->height = height;
	info->hor_stride = align_to(width, 16);
	info->ver_stride = align_to(height, 16);
	info->packet_size = packet_size;
	/*
	 * MJPEG 外部分配输出帧时，按 SDK 示例至少给到 width * height * 2，
	 * 这样可以覆盖 JPEG 常见的 YUV420 / YUV422 输出需求。
	 */
	info->buffer_size = (size_t)info->hor_stride * info->ver_stride * 2;
	return 0;
}

int dump_nv12_frame_from_dmafd(const char *path, int fd, RK_U32 width,
							   RK_U32 height, RK_U32 hor_stride,
							   RK_U32 ver_stride)
{
	/*
	 * 这个函数把“解码后的 NV12 dma-buf”保存成原始 .nv12 文件。
	 *
	 * 为什么不能直接一把 fwrite 整块内存？
	 * 因为真实 buffer 往往带 stride，对齐后每行可能比可见宽度更长。
	 *
	 * 所以这里必须按行写：
	 * - Y 平面写 height 行，每行只写 width 字节
	 * - UV 平面写 height/2 行，每行也只写 width 字节
	 *
	 * 这样导出的文件才是标准 raw NV12，ffplay 才能正确显示。
	 */
	FILE *fp;
	RK_U8 *base;
	RK_U8 *y_plane;
	RK_U8 *uv_plane;
	size_t map_size;
	RK_U32 row;

	if (path == NULL || fd < 0 || width == 0 || height == 0 ||
		hor_stride < width || ver_stride < height) {
		return -1;
	}

	map_size = (size_t)hor_stride * ver_stride * 2;
	base = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		perror("mmap nv12 output");
		return -1;
	}

	fp = fopen(path, "wb");
	if (fp == NULL) {
		perror("fopen nv12 output");
		munmap(base, map_size);
		return -1;
	}

	y_plane = base;
	uv_plane = base + (size_t)hor_stride * ver_stride;

	for (row = 0; row < height; ++row) {
		if (fwrite(y_plane + (size_t)row * hor_stride, 1, width, fp) != width) {
			perror("write nv12 y plane");
			fclose(fp);
			munmap(base, map_size);
			return -1;
		}
	}

	for (row = 0; row < height / 2; ++row) {
		if (fwrite(uv_plane + (size_t)row * hor_stride, 1, width, fp) !=
			width) {
			perror("write nv12 uv plane");
			fclose(fp);
			munmap(base, map_size);
			return -1;
		}
	}

	if (fclose(fp) != 0) {
		perror("close nv12 output");
		munmap(base, map_size);
		return -1;
	}

	if (munmap(base, map_size) != 0) {
		perror("munmap nv12 output");
		return -1;
	}

	printf("[MPP] dumped NV12 frame to %s (%ux%u)\n", path, width, height);
	return 0;
}

void rk_mpp_decoder_advance_set_frame_callback(
	MppDecoderAdvance *ctx,
	mpp_decoded_frame_callback_t callback,
	void *userdata)
{
	if (ctx == NULL) {
		return;
	}

	/*
	 * 这个回调对应的是“已经完成解码的输出帧”。
	 * 和 cam.c 里的摄像头回调不同，这里抛出去的是 NV12 输出信息。
	 */
	ctx->frame_callback = callback;
	ctx->frame_callback_userdata = userdata;
}

void rk_mpp_decoder_advance_deinit(MppDecoderAdvance *ctx)
{
	/*
	 * 释放顺序的理解重点：
	 * - 先释放我们自己持有的 buffer 引用
	 * - 再关闭输出 dmafd
	 * - 最后销毁 MPP 解码器上下文
	 */
	if (ctx == NULL || !ctx->dec_initialized) {
		return;
	}

	if (ctx->in_buf) {
		mpp_buffer_put(ctx->in_buf);
		ctx->in_buf = NULL;
	}
	if (ctx->out_buf) {
		mpp_buffer_put(ctx->out_buf);
		ctx->out_buf = NULL;
	}
	for (int i = 0; i < MPP_OUTPUT_SLOT_COUNT; ++i) {
		if (ctx->dst_fd[i] >= 0) {
			close(ctx->dst_fd[i]);
			ctx->dst_fd[i] = -1;
		}
		ctx->dst_size[i] = 0;
	}

	if (ctx->dec_ctx) {
		if (ctx->dec_api) {
			ctx->dec_api->reset(ctx->dec_ctx);
		}
		mpp_destroy(ctx->dec_ctx);
		ctx->dec_ctx = NULL;
		ctx->dec_api = NULL;
	}

	ctx->frame_callback = NULL;
	ctx->frame_callback_userdata = NULL;
	ctx->dec_initialized = 0;
	ctx->buf_is_init = 0;
}

int rk_mpp_decoder_advance_do_task(MppDecoderAdvance *ctx, int fd, int dstfd,
								   size_t dst_capacity, int index, int w, int h,
								   int stride, int size)
{
	/*
	 * 这是整条 MPP 解码链路的核心函数。
	 *
	 * 输入：
	 * - fd    : 一帧 MJPEG 压缩数据所在的 dma-buf
	 * - index : 这帧对应的 v4l2 buffer 槽位号
	 * - size  : 当前这帧 JPEG 的真实长度
	 *
	 * 输出：
	 * - dstfd 里得到一帧 NV12
	 *
	 * 你读这个函数时，建议严格按下面 7 个阶段看：
	 *
	 * 1. 校验输入
	 * 2. 从 JPEG 头解析 width / height
	 * 3. 绑定调用方传入的输出 dma-buf
	 * 4. 输入 fd -> MppBuffer -> MppPacket
	 * 5. 输出 fd -> MppBuffer -> MppFrame
	 * 6. task 模式提交给硬件并取回结果
	 * 7. 回收 task / packet / frame / buffer 引用
	 */
	MppPacket packet = NULL;
	MppPacket packet_out = NULL;
	MppFrame frame = NULL;
	MppFrame frame_out = NULL;
	MppTask task = NULL;
	MppBuffer in_buf = NULL;
	MppBuffer out_buf = NULL;
	MppBufferInfo in_buf_info = {0};
	MppBufferInfo out_buf_info = {0};
	FrameInfo info = {0};
	void *packet_data;
	MPP_RET ret = MPP_OK;
	int input_submitted = 0;
	int input_sync_started = 0;
	int output_sync_started = 0;
	int output_fd = dstfd;
	size_t output_size = 0;

	/*
	 * 对 MJPEG 输入来说，stride 没什么意义。
	 * 真正可靠的图像宽高来自 JPEG 头，而不是摄像头回调里传进来的 stride。
	 */
	(void)stride;

	if (ctx == NULL || !ctx->dec_initialized || ctx->dec_api == NULL) {
		fprintf(stderr, "[MPP] decoder is not initialized\n");
		return -1;
	}
	if (fd < 0 || size <= 0) {
		fprintf(stderr, "[MPP] invalid input fd=%d size=%d\n", fd, size);
		return -1;
	}
	if (output_fd < 0) {
		fprintf(stderr, "[MPP] invalid output fd=%d\n", output_fd);
		return -1;
	}
	if (ctx->coding != MPP_VIDEO_CodingMJPEG) {
		fprintf(stderr, "[MPP] advanced task decoder only supports MJPEG, codec=%d\n",
				ctx->coding);
		return -1;
	}

	if (mjpeg_get_frame_info_from_dmafd(fd, (size_t)size, &info) != 0) {

		/*
		 * 理想情况：从 JPEG 头解析到真实宽高。
		 * 兜底情况：如果解析失败，再退回到 V4L2 已知的 w / h。
		 */
		if (w <= 0 || h <= 0) {
			fprintf(stderr, "[MPP] failed to parse mjpeg header and no "
							"fallback size is available\n");
			return -1;
		}

		info.width = (RK_U32)w;
		info.height = (RK_U32)h;
		info.hor_stride = align_to((RK_U32)w, 16);
		info.ver_stride = align_to((RK_U32)h, 16);
		info.packet_size = (size_t)size;
		info.buffer_size = (size_t)info.hor_stride * info.ver_stride * 2;

		printf("[MPP] fallback to v4l2 size w=%u h=%u hs=%u vs=%u "
			   "packet=%zu\n",
			   info.width, info.height, info.hor_stride, info.ver_stride,
			   info.packet_size);
	} else if (info.width != (RK_U32)w || info.height != (RK_U32)h) {

		printf("[MPP] jpeg header size %ux%u differs from v4l2 %dx%d, "
			   "trust jpeg header\n",
			   info.width, info.height, w, h);
	} else {
		printf("[MPP]从码流获取到头部信息成功:size w=%u h=%u hs=%u vs=%u "
			   "packet=%zu\n",
			   info.width, info.height, info.hor_stride, info.ver_stride,
			   info.packet_size);
	}

	output_size = info.buffer_size;
	if (dst_capacity > 0 && dst_capacity < output_size) {
		fprintf(stderr,
				"[MPP] output fd capacity too small: capacity=%zu required=%zu "
				"w=%u h=%u hs=%u vs=%u codec=%d\n",
				dst_capacity, output_size, info.width, info.height,
				info.hor_stride, info.ver_stride, ctx->coding);
		return -1;
	}

	/*
	 * 显式声明：
	 * - 输入码流 fd 交给 MPP 读
	 * - 输出 NV12 fd 交给 MPP 写
	 *
	 * 这样能把 MPP 这段和 RGA 那段的 dma-buf 可见性模型统一起来。
	 */
	dmabuf_sync_start(fd, DMA_BUF_SYNC_READ);
	input_sync_started = 1;

	dmabuf_sync_start(output_fd, DMA_BUF_SYNC_WRITE);
	output_sync_started = 1;

	in_buf_info.type = MPP_BUFFER_TYPE_EXT_DMA;
	in_buf_info.fd = fd;
	in_buf_info.size = info.packet_size;

	/*
	 * 这里把“输入 dmafd”导入为 MppBuffer。
	 * 注意：导入并不等于复制数据，它只是把外部 fd 包装成 MPP 能识别的对象。
	 */
	ret = mpp_buffer_import(&in_buf, &in_buf_info);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] mpp_buffer_import(input) failed: %d\n", ret);
		goto out;
	}

	ret = mpp_packet_init_with_buffer(&packet, in_buf);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] mpp_packet_init_with_buffer failed: %d\n", ret);
		goto out;
	}

	/*
	 * MppPacket 描述的是“这一包压缩码流从哪里开始、长度多少”。
	 * 对 MJPEG 来说，通常一帧 JPEG 就是一包 packet。
	 */
	packet_data = mpp_packet_get_data(packet);
	mpp_packet_set_data(packet, packet_data);
	mpp_packet_set_pos(packet, packet_data);
	mpp_packet_set_size(packet, info.packet_size);
	mpp_packet_set_length(packet, info.packet_size);
	mpp_packet_clr_eos(packet);


	out_buf_info.type = MPP_BUFFER_TYPE_EXT_DMA;
	out_buf_info.fd = output_fd;
	out_buf_info.size = output_size;

	/*
	 * 输出侧同理：把“解码目标 dmafd”导入成 MppBuffer，
	 * 再挂到一个 MppFrame 上。
	 */
	ret = mpp_buffer_import(&out_buf, &out_buf_info);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] mpp_buffer_import(output) failed: %d\n", ret);
		goto out;
	}

	ret = mpp_frame_init(&frame);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] mpp_frame_init failed: %d\n", ret);
		goto out;
	}

	mpp_frame_set_buffer(frame, out_buf);
	mpp_frame_set_width(frame, info.width);
	mpp_frame_set_height(frame, info.height);
	mpp_frame_set_hor_stride(frame, info.hor_stride);
	mpp_frame_set_ver_stride(frame, info.ver_stride);
	mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
	mpp_frame_set_buf_size(frame, output_size);
	printf("packet 和 frame 准备完成了\n");
	/*
	 * 到这里，输入和输出都准备好了：
	 * - packet 代表输入 MJPEG
	 * - frame  代表输出 NV12
	 *
	 * 接下来进入 MPP 的 task 模式。
	 */

	/* 1) 等待输入端口可用。 */
	printf("[MPP][DEBUG] input poll begin\n");
	ret = ctx->dec_api->poll(ctx->dec_ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
	printf("[MPP][DEBUG] input poll end ret=%d\n", ret);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] input poll failed: %d\n", ret);
		goto out;
	}

	/* 2) 取一个输入 task。 */
	printf("[MPP][DEBUG] input dequeue begin\n");
	ret = ctx->dec_api->dequeue(ctx->dec_ctx, MPP_PORT_INPUT, &task);
	printf("[MPP][DEBUG] input dequeue end ret=%d task=%p\n", ret, task);
	if (ret != MPP_OK || task == NULL) {
		fprintf(stderr, "[MPP] input dequeue failed: ret=%d task=%p\n", ret,
				task);
		goto out;
	}

	/* 3) 把输入 packet 和输出 frame 绑定到同一个 task 上。 */
	printf("[MPP][DEBUG] bind packet=%p frame=%p to task=%p\n", packet, frame,
		   task);
	mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, packet);
	mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, frame);

	/* 4) 提交给硬件解码。 */
	printf("[MPP][DEBUG] input enqueue begin task=%p\n", task);
	ret = ctx->dec_api->enqueue(ctx->dec_ctx, MPP_PORT_INPUT, task);
	printf("[MPP][DEBUG] input enqueue end ret=%d\n", ret);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] input enqueue failed: %d\n", ret);
		goto out;
	}
	task = NULL;
	input_submitted = 1;

	/* 5) 等待输出端口有结果。 */
	printf("[MPP][DEBUG] output poll begin\n");
	ret = ctx->dec_api->poll(ctx->dec_ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
	printf("[MPP][DEBUG] output poll end ret=%d\n", ret);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] output poll failed: %d\n", ret);
		goto out;
	}

	/* 6) 取回完成的输出 task。 */
	printf("[MPP][DEBUG] output dequeue begin\n");
	ret = ctx->dec_api->dequeue(ctx->dec_ctx, MPP_PORT_OUTPUT, &task);
	printf("[MPP][DEBUG] output dequeue end ret=%d task=%p\n", ret, task);
	if (ret != MPP_OK || task == NULL) {
		fprintf(stderr, "[MPP] output dequeue failed: ret=%d task=%p\n", ret,
				task);
		goto out;
	}

	/* 7) 从 task 里拿回真正的输出 frame。 */
	printf("[MPP][DEBUG] output get frame begin task=%p\n", task);
	ret = mpp_task_meta_get_frame(task, KEY_OUTPUT_FRAME, &frame_out);
	printf("[MPP][DEBUG] output get frame end ret=%d frame=%p\n", ret,
		   frame_out);
	if (ret != MPP_OK || frame_out == NULL) {
		fprintf(stderr, "[MPP] output get frame failed: ret=%d frame=%p\n", ret,
				frame_out);
		goto out;
	}

	printf("[MPP] decoded frame index=%d w=%u h=%u hs=%u vs=%u fmt=%u buf=%zu "
		   "err=%u discard=%u dst_fd=%d\n",
		   index, mpp_frame_get_width(frame_out),
		   mpp_frame_get_height(frame_out), mpp_frame_get_hor_stride(frame_out),
		   mpp_frame_get_ver_stride(frame_out), mpp_frame_get_fmt(frame_out),
		   mpp_frame_get_buf_size(frame_out), mpp_frame_get_errinfo(frame_out),
		   mpp_frame_get_discard(frame_out), output_fd);

	/*
	 * 到这里解码结果已经由 MPP 写完。
	 * 在把输出 fd 交给后面的 RGA 之前，先结束 WRITE 同步，
	 * 让结果对后续消费者可见。
	 */
	if (output_sync_started) {
		dmabuf_sync_end(output_fd, DMA_BUF_SYNC_WRITE);
		output_sync_started = 0;
	}

	if (mpp_frame_get_errinfo(frame_out) == 0 &&
		mpp_frame_get_discard(frame_out) == 0) {
		if (ctx->frame_callback != NULL) {
			ctx->frame_callback(output_fd, index,
								mpp_frame_get_width(frame_out),
								mpp_frame_get_height(frame_out),
								mpp_frame_get_hor_stride(frame_out),
								mpp_frame_get_ver_stride(frame_out),
								mpp_frame_get_buf_size(frame_out),
								ctx->frame_callback_userdata);
		}
	}

	/* 输出 task 用完后要还回输出端口队列。 */
	ret = ctx->dec_api->enqueue(ctx->dec_ctx, MPP_PORT_OUTPUT, task);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] output enqueue failed: %d\n", ret);
		goto out;
	}
	task = NULL;

	/*
	 * 输入 task 也还没结束。
	 * 还需要把它再 dequeue 回来，取出绑定的 packet，然后释放 packet。
	 */
	ret = ctx->dec_api->dequeue(ctx->dec_ctx, MPP_PORT_INPUT, &task);
	if (ret != MPP_OK || task == NULL) {
		fprintf(stderr, "[MPP] input recycle dequeue failed: ret=%d task=%p\n",
				ret, task);
		goto out;
	}

	ret = mpp_task_meta_get_packet(task, KEY_INPUT_PACKET, &packet_out);
	if (ret == MPP_OK && packet_out != NULL) {
		mpp_packet_deinit(&packet_out);
		packet = NULL;
	}

	ret = ctx->dec_api->enqueue(ctx->dec_ctx, MPP_PORT_INPUT, task);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MPP] input recycle enqueue failed: %d\n", ret);
		goto out;
	}
	task = NULL;

	/*
	 * 输入 packet 的生命周期到这里也结束了，
	 * 可以结束这轮 READ 同步。
	 */
	if (input_sync_started) {
		dmabuf_sync_end(fd, DMA_BUF_SYNC_READ);
		input_sync_started = 0;
	}

	ret = MPP_OK;

out:
	/*
	 * out: 这里统一做“本地引用”的清理。
	 *
	 * 要区分两件事：
	 * - task 的所有权在 MPP 端口队列里，需要 enqueue 回去
	 * - packet / frame / buffer 是当前函数自己持有的引用，需要 deinit / put
	 */
	if (task != NULL) {
		if (input_submitted) {
			ctx->dec_api->enqueue(ctx->dec_ctx, MPP_PORT_OUTPUT, task);
		} else {
			ctx->dec_api->enqueue(ctx->dec_ctx, MPP_PORT_INPUT, task);
		}
	}
	if (packet != NULL) {
		mpp_packet_deinit(&packet);
	}
	if (frame != NULL) {
		mpp_frame_deinit(&frame);
	}
	if (out_buf != NULL) {
		mpp_buffer_put(out_buf);
	}
	if (in_buf != NULL) {
		mpp_buffer_put(in_buf);
	}

	if (output_sync_started) {
		dmabuf_sync_end(output_fd, DMA_BUF_SYNC_WRITE);
	}

	if (input_sync_started) {
		dmabuf_sync_end(fd, DMA_BUF_SYNC_READ);
	}

	return (ret == MPP_OK) ? 0 : -1;
}

int rk_mpp_decoder_advance_init(MppDecoderAdvance *ctx, MppCodingType coding)
{
	MPP_RET ret;
	MppDecCfg dec_cfg = NULL;
	RK_U32 output_fmt = MPP_FMT_YUV420SP;
	
	if (ctx == NULL) {
		return -1;
	}
	if (coding != MPP_VIDEO_CodingMJPEG) {
		fprintf(stderr, "[MppAdvance] advanced decoder only supports MJPEG, codec=%d\n",
				coding);
		return -1;
	}

	memset(ctx, 0, sizeof(*ctx));
    ctx->coding = coding;
	for (int i = 0; i < MPP_OUTPUT_SLOT_COUNT; ++i) {
		ctx->dst_fd[i] = -1;
		ctx->dst_size[i] = 0;
	}
   
	ret = mpp_create(&ctx->dec_ctx, &ctx->dec_api);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MppAdvance] mpp_create(dec) failed: %d\n", ret);
		return -1;
	}

	ret = mpp_init(ctx->dec_ctx, MPP_CTX_DEC, coding);
	if (ret != MPP_OK) {
		fprintf(stderr, "[MppAdvance] mpp_init(dec) failed: %d\n", ret);
		mpp_destroy(ctx->dec_ctx);
		ctx->dec_ctx = NULL;
		ctx->dec_api = NULL;
		return -1;
	}

	ret = mpp_dec_cfg_init(&dec_cfg);
	if (ret == MPP_OK && dec_cfg != NULL) {
		ctx->dec_api->control(ctx->dec_ctx, MPP_DEC_GET_CFG, dec_cfg);
		mpp_dec_cfg_set_u32(dec_cfg, "base:split_parse", 0);
		ctx->dec_api->control(ctx->dec_ctx, MPP_DEC_SET_CFG, dec_cfg);
		ctx->dec_api->control(ctx->dec_ctx, MPP_DEC_SET_OUTPUT_FORMAT,
							  &output_fmt);
		mpp_dec_cfg_deinit(dec_cfg);
	}
	ctx->dec_initialized = 1;
	ctx->buf_is_init = 0;
	printf("[MppAdvance] Decoder initialized (codec=%d)\n", coding);
	return 0;
}
