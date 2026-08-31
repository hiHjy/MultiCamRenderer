#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>

#define DRM_MAX_PLANES          24
#define DRM_MAX_FORMATS         64
#define DRM_MAX_CRTCS           8
#define DRM_MAX_ENCODERS        8
#define DRM_MAX_CONNECTORS      8
#define DRM_MAX_MODES           32
#define DRM_MAX_POSSIBLE_IDS    8
#define FB_COUNT                3

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 外部传入的一帧图像。
 *
 * 这个结构体描述的是“已经由外部模块分配好的 dma-buf”，比如 MPP、
 * RGA、camera、其他 buffer pool 给出来的 fd。DRM 内部会把 dma_fd
 * import 成 GEM handle，再用 drmModeAddFB2() 包装成 fb_id。
 *
 * 使用规则：
 * - dma_fd/w/h/fmt 必填。
 * - pitches[] 建议外部明确填写；如果为 0，内部会按常见格式粗略推导。
 * - offsets[] 多数单平面 RGB 为 0；NV12 等多平面格式要按真实布局填写。
 * - modifier 不使用时填 DRM_FORMAT_MOD_INVALID 或 0。
 */
typedef struct {
    int dma_fd;              /* 外部传入的 dma-buf fd，不是 /dev/dri/cardX 的 fd */
    uint64_t size;           /* buffer 总大小，可选；当前主要用于记录/调试 */
    int w;                   /* 源图宽度，单位像素 */
    int h;                   /* 源图高度，单位像素 */
    uint32_t fmt;            /* DRM_FORMAT_*，例如 DRM_FORMAT_XRGB8888/NV12 */
    uint32_t pitches[4];     /* 每个 plane 的 stride，单位字节 */
    uint32_t offsets[4];     /* 每个 plane 在 dma-buf 中的偏移，单位字节 */
    uint64_t modifier;       /* DRM format modifier；不用 modifier 时填 invalid/0 */
} DRM_Buf;

/**
 * 内部 framebuffer 状态。
 *
 * 三缓冲状态机使用：
 * - FB_FREE: 外部可以复用/写入这一帧对应的 buffer。
 * - FB_QUEUED: 已提交给 DRM，等待 page flip event。
 * - FB_FRONT: 当前正在被屏幕扫描显示。
 */
typedef enum {
    FB_FREE = 0,
    FB_QUEUED,
    FB_FRONT
} Fb_State;

/**
 * 内部缓存的一项 framebuffer。
 *
 * 外部每次 submit 传入 dma_fd 后，内部会先查这个表：
 * - 找到相同 dma_fd + 尺寸 + 格式 + pitch/offset：直接复用 fb_id。
 * - 找不到：drmPrimeFDToHandle() + drmModeAddFB2() 新建 fb_id 并缓存。
 *
 * 这个结构体由 DRM 模块内部维护，外部一般只读 state 即可。
 */
typedef struct {
    int used;                /* 这一项缓存是否有效 */
    int dma_fd;              /* 被包装的外部 dma-buf fd */
    uint32_t handles[4];     /* dma-buf import 后得到的 GEM handle */
    uint32_t fb_id;          /* drmModeAddFB2() 生成的 framebuffer id */
    int width;               /* framebuffer 宽度 */
    int height;              /* framebuffer 高度 */
    uint32_t pitches[4];     /* addFB2 使用的 pitches */
    uint32_t offsets[4];     /* addFB2 使用的 offsets */
    uint32_t format;         /* DRM_FORMAT_* */
    Fb_State state;          /* 三缓冲状态 */
} DRM_Fb;

/**
 * 简单三缓冲池。
 *
 * front_idx:
 *   当前正在显示的 fb 索引。
 *
 * pending_idx:
 *   已提交 DRM_MODE_PAGE_FLIP_EVENT，但 page flip 回调还没回来的 fb 索引。
 *   pending_idx >= 0 时，drmDisplaySubmit() 会返回 -1/EAGAIN，避免连续提交把
 *   状态机冲乱。外部应调用 drmHandleEvents() 消费事件。
 */
typedef struct {
    DRM_Fb drm_fds[FB_COUNT];
    int front_idx;
    int pending_idx;
} DRM_Buffer_Pool;

/**
 * plane 支持的一种像素格式。
 */
typedef struct {
    uint32_t fmt;            /* DRM_FORMAT_* 数值 */
    char fmt_str[16];        /* fourcc 短码，例如 XR24/NV12 */
    char fmt_name[64];       /* 可读名称，例如 XRGB8888/NV12 4:2:0 8-bit */
} Plane_Support_Format;

/**
 * DRM plane 资源信息。
 *
 * plane 是真正挂 framebuffer 的图层。Primary 常用于主画面，Overlay
 * 常用于视频层/叠加层，Cursor 用于鼠标光标。
 */
typedef struct {
    uint32_t id;             /* plane object id */
    uint32_t crtc_id;        /* 当前绑定的 crtc id，0 表示未绑定 */
    uint32_t fb_id;          /* 当前绑定的 fb id，0 表示未绑定 */
    uint32_t possible_crtcs; /* 内核返回的 CRTC bitmask */
    uint32_t possible_crtc_ids[DRM_MAX_POSSIBLE_IDS]; /* bitmask 展开后的真实 crtc id */
    int possible_crtc_count;
    char type[32];           /* Primary/Overlay/Cursor/Unknown */
    Plane_Support_Format support_formats[DRM_MAX_FORMATS];
    int format_count;
    int busy;                /* fb_id != 0 时认为正在被占用 */
} Plane_Info;

/**
 * DRM CRTC 资源信息。
 *
 * CRTC 可以理解为扫描输出控制器。设置分辨率时，atomic 实际设置的是
 * CRTC.MODE_ID 和 CRTC.ACTIVE。
 */
typedef struct {
    uint32_t id;             /* crtc object id */
    uint32_t index;          /* 在 drmModeRes->crtcs[] 中的下标 */
    uint32_t buffer_id;      /* 当前 CRTC 上的 fb id */
    int x;
    int y;
    int w;
    int h;
    int mode_valid;          /* raw_mode 是否有效 */
    char mode_name[32];      /* 当前 mode 名称 */
    drmModeModeInfo raw_mode;/* 当前 CRTC mode 原始结构 */
} CRTC_Info;

/**
 * DRM encoder 资源信息。
 *
 * encoder 用于把 CRTC 的像素流转换成 connector 对应的输出信号。
 * 实际选路时，主要用它找可用 CRTC。
 */
typedef struct {
    uint32_t id;             /* encoder object id */
    uint32_t crtc_id;        /* 当前绑定的 crtc id */
    uint32_t possible_crtcs; /* 可绑定 CRTC bitmask */
    uint32_t possible_clones;
    uint32_t possible_crtc_ids[DRM_MAX_POSSIBLE_IDS];
    int possible_crtc_count;
    char type[32];           /* TMDS/DSI/DPI/... */
} Encoder_Info;

/**
 * connector 支持的一种显示模式。
 */
typedef struct {
    int w;                   /* 分辨率宽 */
    int h;                   /* 分辨率高 */
    int fps;                 /* 估算刷新率 */
    int index;               /* 在 connector mode 数组中的下标 */
    uint32_t flags;
    uint32_t type;
    char name[32];           /* mode 名称，例如 1920x1080 */
    drmModeModeInfo raw;     /* atomic modeset 要创建 blob 的原始 mode */
} DRM_Mode_Info;

/**
 * DRM connector 资源信息。
 *
 * connector 是物理/逻辑显示输出端，比如 HDMI、DSI、eDP。
 * 只有 connected=1 且 mode_count>0 的 connector 才适合直接显示。
 */
typedef struct {
    uint32_t id;             /* connector object id */
    uint32_t encoder_id;     /* 当前推荐/正在使用的 encoder */
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t mm_width;
    uint32_t mm_height;
    int connected;
    int mode_count;
    int encoder_count;
    uint32_t encoder_ids[DRM_MAX_ENCODERS];
    DRM_Mode_Info drm_mode_infos[DRM_MAX_MODES];
    char type[32];           /* HDMI-A/DSI/eDP/... */
} Connector_Info;

/**
 * drmInit() 后保存的全量 DRM 资源快照。
 */
typedef struct {
    Plane_Info plane_infos[DRM_MAX_PLANES];
    CRTC_Info crtc_infos[DRM_MAX_CRTCS];
    Encoder_Info encoder_infos[DRM_MAX_ENCODERS];
    Connector_Info connector_infos[DRM_MAX_CONNECTORS];
    int plane_count;
    int crtc_count;
    int encoder_count;
    int connector_count;
} DRM_Res;

/**
 * atomic plane 属性 id 缓存。
 *
 * 注意这里保存的是 property id，不是 property value。
 */
typedef struct {
    uint32_t fb_id;          /* Plane.FB_ID */
    uint32_t crtc_id;        /* Plane.CRTC_ID */
    uint32_t src_x;          /* Plane.SRC_X，16.16 fixed point */
    uint32_t src_y;
    uint32_t src_w;
    uint32_t src_h;
    int32_t crtc_x;          /* Plane.CRTC_X，屏幕坐标 */
    int32_t crtc_y;
    uint32_t crtc_w;         /* Plane.CRTC_W，屏幕显示宽 */
    uint32_t crtc_h;         /* Plane.CRTC_H，屏幕显示高 */
    uint32_t color_encoding; /* 可选，YUV plane 常用 */
    uint32_t color_range;    /* 可选，YUV plane 常用 */
} DRM_Plane_Props;

/**
 * atomic connector 属性 id 缓存。
 */
typedef struct {
    uint32_t crtc_id;        /* Connector.CRTC_ID */
} DRM_Connector_Props;

/**
 * atomic CRTC 属性 id 缓存。
 */
typedef struct {
    uint32_t mode_id;        /* CRTC.MODE_ID */
    uint32_t active;         /* CRTC.ACTIVE */
} DRM_CRTC_Props;

/**
 * 当前显示链路的 atomic 属性缓存。
 *
 * 一旦 connector/crtc/plane 选定，property id 不需要每帧查询。
 * drmAtomicCacheProps() 会填充这里。
 */
typedef struct {
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    DRM_Connector_Props connector;
    DRM_CRTC_Props crtc;
    DRM_Plane_Props plane;
    int valid;
} DRM_Atomic_Props;

/**
 * 显示配置。
 *
 * 这是 drmDisplaySetupConfig() 的输入。字段填 0 通常表示自动选择或默认值。
 *
 * connector_id/crtc_id/plane_id:
 *   外部知道对象 id 时可以指定；为 0 时内部自动选择。
 *
 * mode 选择优先级：
 *   1. use_mode=1 时直接使用 mode。
 *   2. mode_index>=0 时使用 connector modes[mode_index]。
 *   3. mode_w/mode_h 填写时按分辨率和可选 fps 匹配。
 *   4. 都不填时使用 connector 的第 0 个 mode。
 *
 * SRC_*:
 *   从 framebuffer 原图中取哪一块，单位像素。SRC_W/H 为 0 时，
 *   drmDisplaySubmit() 会默认使用当前 DRM_Buf 的 w/h。
 *
 * CRTC_*:
 *   显示到屏幕哪个位置、多大。CRTC_W/H 为 0 时默认使用当前 mode 分辨率。
 *   当 SRC_W/H 和 CRTC_W/H 不一致时，硬件会尝试缩放。
 */
typedef struct {
    uint32_t fmt;

    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;

    int mode_index;
    int mode_w;
    int mode_h;
    int mode_fps;
    drmModeModeInfo mode;
    int use_mode;

    int src_x;
    int src_y;
    int src_w;
    int src_h;

    int crtc_x;
    int crtc_y;
    int crtc_w;
    int crtc_h;
} DRM_Display_Config;

/**
 * DRM 上下文。
 *
 * 外部创建一个 DRM_Ctx，然后按如下顺序使用：
 *   drmInit(&ctx);
 *   drmDisplaySetupConfig(&ctx, &cfg); // 可选，不调则 submit 时自动 setup
 *   drmDisplaySubmit(&ctx, &buf);
 *   drmHandleEvents(&ctx, timeout_ms); // 有异步 page flip 时需要调用
 *   drmDeinit(&ctx);
 */
typedef struct {
    int drm_fd;                  /* /dev/dri/card0 fd */
    DRM_Res res;                 /* 资源枚举结果 */
    DRM_Atomic_Props atomic;     /* 当前显示链路的 property id 缓存 */
    drmModeAtomicReq *atomic_req;/* 复用的 atomic request */
    DRM_Buffer_Pool pool;        /* dma_fd -> fb_id 缓存和三缓冲状态 */
    uint32_t selected_connector_id;
    uint32_t selected_crtc_id;
    uint32_t selected_plane_id;
    drmModeModeInfo selected_mode;
    int src_x;
    int src_y;
    int src_w;
    int src_h;
    int dst_x;
    int dst_y;
    int dst_w;
    int dst_h;
    int modeset_done;            /* 第一次全量 atomic modeset 是否已完成 */
} DRM_Ctx;

/**
 * 打开 DRM 设备并枚举资源。
 *
 * 成功后 ctx->res 中保存 connector/crtc/encoder/plane/mode 信息，
 * 同时打开 UNIVERSAL_PLANES 和 ATOMIC client capability。
 *
 * 返回：0 成功，-1 失败。
 */
int drmInit(DRM_Ctx *ctx);

/**
 * 释放 DRM_Ctx 内部资源。
 *
 * 会释放缓存的 fb_id/GEM handle、atomic request，并关闭 drm_fd。
 */
void drmDeinit(DRM_Ctx *ctx);

/**
 * 打印当前枚举到的 DRM 资源，主要用于学习和调试。
 */
void drmDumpResources(const DRM_Ctx *ctx);

/**
 * 把 DRM_FORMAT_* 转成 fourcc 短字符串。
 *
 * 示例：DRM_FORMAT_XRGB8888 -> "XR24"。
 * out 至少建议 16 字节。
 */
const char *drmFormatToString(uint32_t fmt, char out[16]);

/**
 * 把 DRM_FORMAT_* 转成更容易读的格式名。
 *
 * 示例：DRM_FORMAT_XRGB8888 -> "XRGB8888"。
 */
const char *drmFormatToName(uint32_t fmt);

/**
 * 查询某个 DRM object 的 property id。
 *
 * obj_type 示例：
 * - DRM_MODE_OBJECT_PLANE
 * - DRM_MODE_OBJECT_CRTC
 * - DRM_MODE_OBJECT_CONNECTOR
 *
 * 返回：property id；0 表示没找到。
 */
uint32_t drmGetPropertyId(int fd, uint32_t obj_id, uint32_t obj_type, const char *name);

/**
 * 查询枚举属性中某个 enum name 对应的 value。
 *
 * 主要用于 COLOR_ENCODING/COLOR_RANGE 这类 enum 属性。
 */
uint64_t drmGetPropertyEnumValue(int fd, uint32_t obj_id, uint32_t obj_type,
                                 const char *prop_name, const char *enum_name);

/**
 * 缓存当前 connector/crtc/plane 这条显示链路需要的 atomic property id。
 *
 * 一般不需要外部手动调用；drmDisplaySetupConfig() 会自动调用。
 */
int drmAtomicCacheProps(DRM_Ctx *ctx, uint32_t connector_id, uint32_t crtc_id, uint32_t plane_id);

/**
 * 底层 atomic 全量提交：设置 connector + crtc + plane。
 *
 * 这是真正的 modeset：
 * - Connector.CRTC_ID = crtc_id
 * - CRTC.MODE_ID = mode blob
 * - CRTC.ACTIVE = 1
 * - Plane.FB_ID、Plane.CRTC_ID、Plane.SRC 系列、Plane.CRTC 系列
 *
 * 一般建议用上层 drmDisplaySetupConfig() + drmDisplaySubmit()。
 */
int drmAtomicCommitModePlane(DRM_Ctx *ctx,
                             uint32_t connector_id,
                             uint32_t crtc_id,
                             uint32_t plane_id,
                             uint32_t fb_id,
                             const drmModeModeInfo *mode,
                             int src_x,
                             int src_y,
                             int src_w,
                             int src_h,
                             int crtc_x,
                             int crtc_y,
                             int crtc_w,
                             int crtc_h);

/**
 * 底层 atomic plane 提交：更新 FB_ID + SRC_* + CRTC_*。
 *
 * 用于移动、裁剪、缩放或切换 fb。只切帧时更推荐 drmAtomicCommitFb()
 * 或上层 drmDisplaySubmit()。
 */
int drmAtomicCommitPlane(DRM_Ctx *ctx,
                         uint32_t crtc_id,
                         uint32_t plane_id,
                         uint32_t fb_id,
                         int src_x,
                         int src_y,
                         int src_w,
                         int src_h,
                         int crtc_x,
                         int crtc_y,
                         int crtc_w,
                         int crtc_h);

/**
 * 底层 atomic 只更新 plane 的 FB_ID。
 *
 * 首次全量提交之后，双/三缓冲切帧通常只需要更新 FB_ID。
 */
int drmAtomicCommitFb(DRM_Ctx *ctx, uint32_t plane_id, uint32_t fb_id);

/**
 * 简化显示配置接口。
 *
 * 等价于：
 * - 自动选择 connector/crtc/plane
 * - 只指定格式和屏幕显示区域 CRTC_X/Y/W/H
 *
 * 参数 x/y/w/h 是显示到屏幕的位置和大小，不是原图裁剪区域。
 */
int drmDisplaySetup(DRM_Ctx *ctx, uint32_t fmt, int x, int y, int w, int h);

/**
 * 完整显示配置接口。
 *
 * 外部可以指定 connector_id/crtc_id/plane_id/mode/SRC/CRTC。
 * 不指定的部分内部会自动选择。
 *
 * 常见用法：
 *   DRM_Display_Config cfg;
 *   memset(&cfg, 0, sizeof(cfg));
 *   cfg.fmt = DRM_FORMAT_XRGB8888;
 *   cfg.mode_index = -1;
 *   cfg.src_x = 0;
 *   cfg.src_y = 0;
 *   cfg.src_w = 640;
 *   cfg.src_h = 480;
 *   cfg.crtc_x = 100;
 *   cfg.crtc_y = 50;
 *   cfg.crtc_w = 1280;
 *   cfg.crtc_h = 720;
 *   drmDisplaySetupConfig(&ctx, &cfg);
 */
int drmDisplaySetupConfig(DRM_Ctx *ctx, const DRM_Display_Config *cfg);

/**
 * 提交一帧外部 dma-buf 显示。
 *
 * 内部行为：
 * - 如果 dma_fd 还没有 fb_id，自动 import 并 drmModeAddFB2()。
 * - 第一次 submit 做全量 atomic modeset。
 * - 后续 submit 只更新 FB_ID，并请求 page flip event。
 *
 * 如果上一帧 page flip 还没完成，会返回 -1 且 errno=EAGAIN。
 */
int drmDisplaySubmit(DRM_Ctx *ctx, const DRM_Buf *buf);

/**
 * 处理 DRM 事件。
 *
 * 使用 DRM_MODE_PAGE_FLIP_EVENT 后，外部需要周期性调用这个函数。
 * page flip 回调会把 pending buffer 标记为 FRONT，把旧 FRONT 标记为 FREE。
 *
 * 返回：poll/drmHandleEvent 的返回值；0 表示超时。
 */
int drmHandleEvents(DRM_Ctx *ctx, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
