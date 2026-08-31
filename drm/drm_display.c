#include "drm_display.h"

#include <errno.h>

static const char *connectorTypeToString(uint32_t type)
{
    switch (type) {
    case DRM_MODE_CONNECTOR_Unknown: return "Unknown";
    case DRM_MODE_CONNECTOR_VGA: return "VGA";
    case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite: return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO: return "S-Video";
    case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
    case DRM_MODE_CONNECTOR_Component: return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN: return "9PinDIN";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DisplayPort";
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV: return "TV";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
    case DRM_MODE_CONNECTOR_DSI: return "DSI";
    case DRM_MODE_CONNECTOR_DPI: return "DPI";
    case DRM_MODE_CONNECTOR_WRITEBACK: return "Writeback";
    default: return "Unknown";
    }
}

static const char *encoderTypeToString(uint32_t type)
{
    switch (type) {
    case DRM_MODE_ENCODER_NONE: return "None";
    case DRM_MODE_ENCODER_DAC: return "DAC";
    case DRM_MODE_ENCODER_TMDS: return "TMDS";
    case DRM_MODE_ENCODER_LVDS: return "LVDS";
    case DRM_MODE_ENCODER_TVDAC: return "TVDAC";
    case DRM_MODE_ENCODER_VIRTUAL: return "Virtual";
    case DRM_MODE_ENCODER_DSI: return "DSI";
    case DRM_MODE_ENCODER_DPMST: return "DPMST";
    case DRM_MODE_ENCODER_DPI: return "DPI";
    default: return "Unknown";
    }
}

static const char *planeTypeToString(uint64_t type)
{
    switch (type) {
    case DRM_PLANE_TYPE_OVERLAY: return "Overlay";
    case DRM_PLANE_TYPE_PRIMARY: return "Primary";
    case DRM_PLANE_TYPE_CURSOR: return "Cursor";
    default: return "Unknown";
    }
}

static int planeTypeRank(const char *type)
{
    if (strcmp(type, "Primary") == 0) {
        return 0;
    }
    if (strcmp(type, "Overlay") == 0) {
        return 1;
    }
    if (strcmp(type, "Cursor") == 0) {
        return 2;
    }
    return 3;
}

uint32_t drmGetPropertyId(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) {
        return 0;
    }

    uint32_t prop_id = 0;

    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) {
            continue;
        }

        if (strcmp(prop->name, name) == 0) {
            prop_id = prop->prop_id;
            drmModeFreeProperty(prop);
            break;
        }

        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return prop_id;
}

uint64_t drmGetPropertyEnumValue(int fd, uint32_t obj_id, uint32_t obj_type,
                                 const char *prop_name, const char *enum_name)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) {
        return 0;
    }

    uint64_t value = 0;

    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) {
            continue;
        }

        if (strcmp(prop->name, prop_name) == 0) {
            for (int j = 0; j < prop->count_enums; j++) {
                if (strcmp(prop->enums[j].name, enum_name) == 0) {
                    value = prop->enums[j].value;
                    drmModeFreeProperty(prop);
                    drmModeFreeObjectProperties(props);
                    return value;
                }
            }
        }

        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return value;
}

static int loadConnectorProps(int fd, uint32_t connector_id, DRM_Connector_Props *props)
{
    memset(props, 0, sizeof(*props));
    props->crtc_id = drmGetPropertyId(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

    if (!props->crtc_id) {
        printf("connector %u missing CRTC_ID property\n", connector_id);
        return -1;
    }

    return 0;
}

static int loadCrtcProps(int fd, uint32_t crtc_id, DRM_CRTC_Props *props)
{
    memset(props, 0, sizeof(*props));
    props->mode_id = drmGetPropertyId(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    props->active = drmGetPropertyId(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");

    if (!props->mode_id || !props->active) {
        printf("crtc %u missing MODE_ID/ACTIVE property\n", crtc_id);
        return -1;
    }

    return 0;
}

static int loadPlaneProps(int fd, uint32_t plane_id, DRM_Plane_Props *props)
{
    memset(props, 0, sizeof(*props));
    props->fb_id = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    props->crtc_id = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    props->src_x = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    props->src_y = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    props->src_w = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    props->src_h = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    props->crtc_x = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    props->crtc_y = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    props->crtc_w = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    props->crtc_h = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    props->color_encoding = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING");
    props->color_range = drmGetPropertyId(fd, plane_id, DRM_MODE_OBJECT_PLANE, "COLOR_RANGE");

    if (!props->fb_id || !props->crtc_id ||
        !props->src_x || !props->src_y || !props->src_w || !props->src_h ||
        !props->crtc_x || !props->crtc_y || !props->crtc_w || !props->crtc_h) {
        printf("plane %u missing required atomic plane properties\n", plane_id);
        return -1;
    }

    return 0;
}

int drmAtomicCacheProps(DRM_Ctx *ctx, uint32_t connector_id, uint32_t crtc_id, uint32_t plane_id)
{
    /*
     * 缓存当前显示链路的 atomic property id。
     *
     * atomic API 设置属性时用的是 property id，不是字符串名字。例如设置
     * Plane.FB_ID 时，真正传给 drmModeAtomicAddProperty() 的是 FB_ID
     * 这个属性的 id。
     *
     * property id 在同一个 DRM 设备生命周期里不会每帧变化，所以第一次选定
     * connector/crtc/plane 后查一次即可。后续切帧只需要使用缓存的
     * ctx->atomic.plane.fb_id。
     */
    if (!ctx || ctx->drm_fd < 0) {
        return -1;
    }

    memset(&ctx->atomic, 0, sizeof(ctx->atomic));

    if (loadConnectorProps(ctx->drm_fd, connector_id, &ctx->atomic.connector) != 0 ||
        loadCrtcProps(ctx->drm_fd, crtc_id, &ctx->atomic.crtc) != 0 ||
        loadPlaneProps(ctx->drm_fd, plane_id, &ctx->atomic.plane) != 0) {
        memset(&ctx->atomic, 0, sizeof(ctx->atomic));
        return -1;
    }

    ctx->atomic.connector_id = connector_id;
    ctx->atomic.crtc_id = crtc_id;
    ctx->atomic.plane_id = plane_id;
    ctx->atomic.valid = 1;
    return 0;
}

static int ensureAtomicProps(DRM_Ctx *ctx, uint32_t connector_id, uint32_t crtc_id, uint32_t plane_id)
{
    if (ctx->atomic.valid &&
        ctx->atomic.connector_id == connector_id &&
        ctx->atomic.crtc_id == crtc_id &&
        ctx->atomic.plane_id == plane_id) {
        return 0;
    }

    return drmAtomicCacheProps(ctx, connector_id, crtc_id, plane_id);
}

static int ensurePlaneProps(DRM_Ctx *ctx, uint32_t plane_id)
{
    if (!ctx || ctx->drm_fd < 0) {
        return -1;
    }

    if (ctx->atomic.plane_id == plane_id && ctx->atomic.plane.fb_id) {
        return 0;
    }

    memset(&ctx->atomic.plane, 0, sizeof(ctx->atomic.plane));
    if (loadPlaneProps(ctx->drm_fd, plane_id, &ctx->atomic.plane) != 0) {
        return -1;
    }

    ctx->atomic.plane_id = plane_id;
    return 0;
}

static drmModeAtomicReq *atomicReqBegin(DRM_Ctx *ctx)
{
    if (!ctx) {
        return NULL;
    }

    if (!ctx->atomic_req) {
        ctx->atomic_req = drmModeAtomicAlloc();
    }

    if (ctx->atomic_req) {
        drmModeAtomicSetCursor(ctx->atomic_req, 0);
    }

    return ctx->atomic_req;
}

static void atomicReqReset(DRM_Ctx *ctx)
{
    if (ctx && ctx->atomic_req) {
        drmModeAtomicSetCursor(ctx->atomic_req, 0);
    }
}

static int atomicAddPlane(drmModeAtomicReq *req,
                          uint32_t plane_id,
                          const DRM_Plane_Props *props,
                          uint32_t crtc_id,
                          uint32_t fb_id,
                          int src_x,
                          int src_y,
                          int src_w,
                          int src_h,
                          int crtc_x,
                          int crtc_y,
                          int crtc_w,
                          int crtc_h)
{
    if (src_w <= 0 || src_h <= 0 || crtc_w <= 0 || crtc_h <= 0) {
        printf("invalid plane size src=%dx%d crtc=%dx%d\n", src_w, src_h, crtc_w, crtc_h);
        return -1;
    }

    drmModeAtomicAddProperty(req, plane_id, props->fb_id, fb_id);
    drmModeAtomicAddProperty(req, plane_id, props->crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, props->src_x, (uint64_t)src_x << 16);
    drmModeAtomicAddProperty(req, plane_id, props->src_y, (uint64_t)src_y << 16);
    drmModeAtomicAddProperty(req, plane_id, props->src_w, (uint64_t)src_w << 16);
    drmModeAtomicAddProperty(req, plane_id, props->src_h, (uint64_t)src_h << 16);
    drmModeAtomicAddProperty(req, plane_id, props->crtc_x, crtc_x);
    drmModeAtomicAddProperty(req, plane_id, props->crtc_y, crtc_y);
    drmModeAtomicAddProperty(req, plane_id, props->crtc_w, crtc_w);
    drmModeAtomicAddProperty(req, plane_id, props->crtc_h, crtc_h);

    return 0;
}

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
                             int crtc_h)
{
    /*
     * 完整 atomic modeset。
     *
     * 第一次点亮屏幕或切换分辨率时，需要在同一个 atomic request 中同时设置：
     *
     *   Connector.CRTC_ID = crtc_id
     *   CRTC.MODE_ID      = mode_blob_id
     *   CRTC.ACTIVE       = 1
     *   Plane.FB_ID       = fb_id
     *   Plane.CRTC_ID     = crtc_id
     *   Plane.SRC_*       = 从 framebuffer 中取哪块图像
     *   Plane.CRTC_*      = 显示到屏幕哪里、多大
     *
     * 注意：
     * - mode 不能直接塞进 atomic property，需要先用
     *   drmModeCreatePropertyBlob() 变成 mode_blob_id。
     * - 提交 flags 必须带 DRM_MODE_ATOMIC_ALLOW_MODESET，否则内核会拒绝
     *   MODE_ID/ACTIVE/Connector.CRTC_ID 这类 modeset 操作。
     * - src_x/src_y/src_w/src_h 传入普通像素值，本函数内部会转成 16.16。
     */
    if (!ctx || ctx->drm_fd < 0 || !mode) {
        return -1;
    }

    uint32_t mode_blob_id = 0;
    int ret = -1;

    if (ensureAtomicProps(ctx, connector_id, crtc_id, plane_id) != 0) {
        printf("loadProps error\n");
        return -1;
    }

    ret = drmModeCreatePropertyBlob(ctx->drm_fd, mode, sizeof(*mode), &mode_blob_id);
    if (ret != 0) {
        perror("drmModeCreatePropertyBlob");
        return -1;
    }

    drmModeAtomicReq *req = atomicReqBegin(ctx);
    if (!req) {
        drmModeDestroyPropertyBlob(ctx->drm_fd, mode_blob_id);
        return -1;
    }

    drmModeAtomicAddProperty(req, connector_id, ctx->atomic.connector.crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, crtc_id, ctx->atomic.crtc.mode_id, mode_blob_id);
    drmModeAtomicAddProperty(req, crtc_id, ctx->atomic.crtc.active, 1);

    if (atomicAddPlane(req, plane_id, &ctx->atomic.plane, crtc_id, fb_id,
                       src_x, src_y, src_w, src_h,
                       crtc_x, crtc_y, crtc_w, crtc_h) != 0) {
        atomicReqReset(ctx);
        drmModeDestroyPropertyBlob(ctx->drm_fd, mode_blob_id);
        return -1;
    }

    ret = drmModeAtomicCommit(ctx->drm_fd, req,
                              DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
                              NULL);
    if (ret != 0) {
        perror("drmModeAtomicCommit(TEST_ONLY modeset)");
        atomicReqReset(ctx);
        drmModeDestroyPropertyBlob(ctx->drm_fd, mode_blob_id);
        return -1;
    }

    ret = drmModeAtomicCommit(ctx->drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    if (ret != 0) {
        perror("drmModeAtomicCommit(modeset)");
        atomicReqReset(ctx);
        drmModeDestroyPropertyBlob(ctx->drm_fd, mode_blob_id);
        return -1;
    }

    atomicReqReset(ctx);
    drmModeDestroyPropertyBlob(ctx->drm_fd, mode_blob_id);
    return 0;
}

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
                         int crtc_h)
{
    /*
     * 更新 plane 的完整显示参数。
     *
     * 这个函数适合这些场景：
     * - 换 fb，同时改变显示位置。
     * - 做窗口移动。
     * - 做裁剪。
     * - 做缩放。
     *
     * 如果只是三缓冲切帧，不改变 SRC/CRTC 区域，使用 drmAtomicCommitFb()
     * 更轻。
     */
    if (!ctx || ctx->drm_fd < 0) {
        return -1;
    }

    if (ensurePlaneProps(ctx, plane_id) != 0) {
        return -1;
    }

    drmModeAtomicReq *req = atomicReqBegin(ctx);
    if (!req) {
        return -1;
    }

    if (atomicAddPlane(req, plane_id, &ctx->atomic.plane, crtc_id, fb_id,
                       src_x, src_y, src_w, src_h,
                       crtc_x, crtc_y, crtc_w, crtc_h) != 0) {
        atomicReqReset(ctx);
        return -1;
    }

    int ret = drmModeAtomicCommit(ctx->drm_fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
    if (ret != 0) {
        perror("drmModeAtomicCommit(TEST_ONLY plane)");
        atomicReqReset(ctx);
        return -1;
    }

    ret = drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
    if (ret != 0) {
        perror("drmModeAtomicCommit(plane)");
        atomicReqReset(ctx);
        return -1;
    }

    atomicReqReset(ctx);
    return 0;
}

int drmAtomicCommitFb(DRM_Ctx *ctx, uint32_t plane_id, uint32_t fb_id)
{
    /*
     * 只更新 Plane.FB_ID。
     *
     * 首次 modeset 完成后，connector/crtc/plane/SRC/CRTC 都已经固定。
     * 后续显示下一帧时，只要把 plane 的 FB_ID 切到新的 fb_id 即可。
     *
     * 这个底层函数目前会请求 DRM_MODE_PAGE_FLIP_EVENT，提交成功后需要
     * 外部调用 drmHandleEvents() 消费事件。上层 drmDisplaySubmit() 也用
     * 同样思路维护三缓冲状态。
     */
    if (!ctx || ctx->drm_fd < 0) {
        return -1;
    }

    if (ensurePlaneProps(ctx, plane_id) != 0) {
        return -1;
    }

    drmModeAtomicReq *req = atomicReqBegin(ctx);
    if (!req) {
        return -1;
    }

    drmModeAtomicAddProperty(req, plane_id, ctx->atomic.plane.fb_id, fb_id);

    // int ret = drmModeAtomicCommit(ctx->drm_fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
    // if (ret != 0) {
    //     perror("drmModeAtomicCommit(TEST_ONLY fb)");
    //     atomicReqReset(ctx);
    //     return -1;
    // }

    int ret = drmModeAtomicCommit(ctx->drm_fd, req, DRM_MODE_PAGE_FLIP_EVENT, NULL);
    if (ret != 0) {
        perror("drmModeAtomicCommit(fb)");
        atomicReqReset(ctx);
        return -1;
    }





    atomicReqReset(ctx);
    return 0;
}

static int drmFormatPlaneCount(uint32_t fmt)
{
    switch (fmt) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV24:
    case DRM_FORMAT_NV15:
    case fourcc_code('N', 'V', '2', '0'):
    case fourcc_code('N', 'V', '3', '0'):
        return 2;
    default:
        return 1;
    }
}

static int planeSupportsFormat(const Plane_Info *plane, uint32_t fmt)
{
    for (int i = 0; i < plane->format_count; i++) {
        if (plane->support_formats[i].fmt == fmt) {
            return 1;
        }
    }

    return 0;
}

static int planeSupportsCrtc(const Plane_Info *plane, uint32_t crtc_id)
{
    for (int i = 0; i < plane->possible_crtc_count; i++) {
        if (plane->possible_crtc_ids[i] == crtc_id) {
            return 1;
        }
    }

    return 0;
}

static int chooseConnectedConnector(const DRM_Ctx *ctx)
{
    for (int i = 0; i < ctx->res.connector_count; i++) {
        const Connector_Info *conn = &ctx->res.connector_infos[i];
        if (conn->connected && conn->mode_count > 0) {
            return i;
        }
    }

    return -1;
}

static int findConnectorIndex(const DRM_Ctx *ctx, uint32_t connector_id)
{
    for (int i = 0; i < ctx->res.connector_count; i++) {
        if (ctx->res.connector_infos[i].id == connector_id) {
            return i;
        }
    }

    return -1;
}

static int findPlaneIndex(const DRM_Ctx *ctx, uint32_t plane_id)
{
    for (int i = 0; i < ctx->res.plane_count; i++) {
        if (ctx->res.plane_infos[i].id == plane_id) {
            return i;
        }
    }

    return -1;
}

static const Encoder_Info *findEncoderInfo(const DRM_Ctx *ctx, uint32_t encoder_id)
{
    for (int i = 0; i < ctx->res.encoder_count; i++) {
        if (ctx->res.encoder_infos[i].id == encoder_id) {
            return &ctx->res.encoder_infos[i];
        }
    }

    return NULL;
}

static uint32_t chooseCrtcForConnector(const DRM_Ctx *ctx, const Connector_Info *conn)
{
    if (conn->encoder_id) {
        const Encoder_Info *enc = findEncoderInfo(ctx, conn->encoder_id);
        if (enc && enc->crtc_id) {
            return enc->crtc_id;
        }
        if (enc && enc->possible_crtc_count > 0) {
            return enc->possible_crtc_ids[0];
        }
    }

    for (int i = 0; i < conn->encoder_count; i++) {
        const Encoder_Info *enc = findEncoderInfo(ctx, conn->encoder_ids[i]);
        if (!enc) {
            continue;
        }
        if (enc->crtc_id) {
            return enc->crtc_id;
        }
        if (enc->possible_crtc_count > 0) {
            return enc->possible_crtc_ids[0];
        }
    }

    return ctx->res.crtc_count > 0 ? ctx->res.crtc_infos[0].id : 0;
}

static int choosePlaneForFormat(const DRM_Ctx *ctx, uint32_t crtc_id, uint32_t fmt)
{
    const char *order[] = {"Primary", "Overlay", "Cursor"};

    for (int rank = 0; rank < 3; rank++) {
        for (int i = 0; i < ctx->res.plane_count; i++) {
            const Plane_Info *plane = &ctx->res.plane_infos[i];
            if (strcmp(plane->type, order[rank]) != 0) {
                continue;
            }
            if (planeSupportsCrtc(plane, crtc_id) && planeSupportsFormat(plane, fmt)) {
                return i;
            }
        }
    }

    return -1;
}

static int chooseModeForConfig(const Connector_Info *conn,
                               const DRM_Display_Config *cfg,
                               drmModeModeInfo *mode)
{
    if (cfg->use_mode) {
        *mode = cfg->mode;
        return 0;
    }

    if (cfg->mode_index >= 0) {
        if (cfg->mode_index >= conn->mode_count) {
            return -1;
        }
        *mode = conn->drm_mode_infos[cfg->mode_index].raw;
        return 0;
    }

    if (cfg->mode_w > 0 && cfg->mode_h > 0) {
        for (int i = 0; i < conn->mode_count; i++) {
            const DRM_Mode_Info *candidate = &conn->drm_mode_infos[i];
            if (candidate->w != cfg->mode_w || candidate->h != cfg->mode_h) {
                continue;
            }
            if (cfg->mode_fps > 0 && candidate->fps != cfg->mode_fps) {
                continue;
            }
            *mode = candidate->raw;
            return 0;
        }
        return -1;
    }

    if (conn->mode_count <= 0) {
        return -1;
    }

    *mode = conn->drm_mode_infos[0].raw;
    return 0;
}

static int defaultPitchForFormat(uint32_t fmt, int width, int plane)
{
    switch (fmt) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
        return width * 4;
    case DRM_FORMAT_RGB888:
    case DRM_FORMAT_BGR888:
        return width * 3;
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
    case DRM_FORMAT_YUYV:
    case DRM_FORMAT_UYVY:
        return width * 2;
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV24:
    case DRM_FORMAT_NV15:
    case fourcc_code('N', 'V', '2', '0'):
    case fourcc_code('N', 'V', '3', '0'):
        (void)plane;
        return width;
    default:
        return plane == 0 ? width * 4 : width;
    }
}

static void normalizeBuf(const DRM_Buf *in, DRM_Buf *out)
{
    *out = *in;
    int planes = drmFormatPlaneCount(out->fmt);

    for (int i = 0; i < planes; i++) {
        if (out->pitches[i] == 0) {
            out->pitches[i] = defaultPitchForFormat(out->fmt, out->w, i);
        }
    }
}

static int fbMatchesBuf(const DRM_Fb *fb, const DRM_Buf *buf)
{
    int planes = drmFormatPlaneCount(buf->fmt);

    if (!fb->used || fb->dma_fd != buf->dma_fd ||
        fb->width != buf->w || fb->height != buf->h ||
        fb->format != buf->fmt) {
        return 0;
    }

    for (int i = 0; i < planes; i++) {
        if (fb->pitches[i] != buf->pitches[i] ||
            fb->offsets[i] != buf->offsets[i]) {
            return 0;
        }
    }

    return 1;
}

static int findCachedFb(const DRM_Ctx *ctx, const DRM_Buf *buf)
{
    for (int i = 0; i < FB_COUNT; i++) {
        if (fbMatchesBuf(&ctx->pool.drm_fds[i], buf)) {
            return i;
        }
    }

    return -1;
}

static int findFreeFbSlot(const DRM_Ctx *ctx)
{
    for (int i = 0; i < FB_COUNT; i++) {
        if (!ctx->pool.drm_fds[i].used) {
            return i;
        }
    }

    for (int i = 0; i < FB_COUNT; i++) {
        if (ctx->pool.drm_fds[i].state == FB_FREE) {
            return i;
        }
    }

    return -1;
}

static void drmCloseGemHandle(int fd, uint32_t handle)
{
    if (!handle) {
        return;
    }

    struct drm_gem_close close_arg;
    memset(&close_arg, 0, sizeof(close_arg));
    close_arg.handle = handle;
    drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
}

static void releaseFbSlot(DRM_Ctx *ctx, int index)
{
    if (index < 0 || index >= FB_COUNT) {
        return;
    }

    DRM_Fb *fb = &ctx->pool.drm_fds[index];
    if (!fb->used) {
        return;
    }

    if (fb->fb_id) {
        drmModeRmFB(ctx->drm_fd, fb->fb_id);
    }

    for (int i = 0; i < 4; i++) {
        int duplicate = 0;
        for (int j = 0; j < i; j++) {
            if (fb->handles[j] == fb->handles[i]) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            drmCloseGemHandle(ctx->drm_fd, fb->handles[i]);
        }
    }

    memset(fb, 0, sizeof(*fb));
    fb->state = FB_FREE;
}

static int importBufAsFb(DRM_Ctx *ctx, const DRM_Buf *buf)
{
    /*
     * 把外部 dma_fd 包装成 DRM framebuffer。
     *
     * DRM atomic plane 不能直接显示 dma_fd，它需要 FB_ID。这里做三步：
     *
     * 1. drmPrimeFDToHandle()
     *      把外部 dma-buf fd import 到当前 DRM 设备，得到 GEM handle。
     *
     * 2. drmModeAddFB2()/drmModeAddFB2WithModifiers()
     *      用 handle + format + pitch + offset 创建 framebuffer，得到 fb_id。
     *
     * 3. 写入 ctx->pool.drm_fds[slot]
     *      后面相同 dma_fd 再提交时直接复用 fb_id，不重复 addFB2。
     *
     * 注意：
     * - RGB 通常只有 handles[0]/pitches[0]/offsets[0]。
     * - NV12 这类格式有多个 plane，但在很多分配器里多个 plane 共享同一个
     *   dma_fd/handle，通过 offset 区分 Y 和 UV。
     */
    int slot = findFreeFbSlot(ctx);
    if (slot < 0) {
        printf("no free fb cache slot, front=%d pending=%d\n",
               ctx->pool.front_idx, ctx->pool.pending_idx);
        return -1;
    }

    releaseFbSlot(ctx, slot);

    uint32_t handle = 0;
    if (drmPrimeFDToHandle(ctx->drm_fd, buf->dma_fd, &handle) != 0) {
        perror("drmPrimeFDToHandle");
        return -1;
    }

    DRM_Fb *fb = &ctx->pool.drm_fds[slot];
    int planes = drmFormatPlaneCount(buf->fmt);

    fb->used = 1;
    fb->dma_fd = buf->dma_fd;
    fb->width = buf->w;
    fb->height = buf->h;
    fb->format = buf->fmt;
    fb->state = FB_FREE;

    for (int i = 0; i < planes; i++) {
        fb->handles[i] = handle;
        fb->pitches[i] = buf->pitches[i];
        fb->offsets[i] = buf->offsets[i];
    }

    uint32_t flags = 0;
    int ret;

    if (buf->modifier && buf->modifier != DRM_FORMAT_MOD_INVALID) {
        uint64_t modifiers[4] = {0, 0, 0, 0};
        for (int i = 0; i < planes; i++) {
            modifiers[i] = buf->modifier;
        }
        flags = DRM_MODE_FB_MODIFIERS;
        ret = drmModeAddFB2WithModifiers(ctx->drm_fd, buf->w, buf->h, buf->fmt,
                                         fb->handles, fb->pitches, fb->offsets,
                                         modifiers, &fb->fb_id, flags);
    } else {
        ret = drmModeAddFB2(ctx->drm_fd, buf->w, buf->h, buf->fmt,
                            fb->handles, fb->pitches, fb->offsets,
                            &fb->fb_id, flags);
    }

    if (ret != 0) {
        perror("drmModeAddFB2");
        releaseFbSlot(ctx, slot);
        return -1;
    }

    return slot;
}

static int getOrCreateFb(DRM_Ctx *ctx, const DRM_Buf *buf)
{
    int index = findCachedFb(ctx, buf);
    if (index >= 0) {
        return index;
    }

    return importBufAsFb(ctx, buf);
}

static void pageFlipHandler(int fd, unsigned int frame,
                            unsigned int sec, unsigned int usec, void *data)
{
    /*
     * page flip 完成回调。
     *
     * drmDisplaySubmit() 后续切帧时使用 DRM_MODE_PAGE_FLIP_EVENT |
     * DRM_MODE_ATOMIC_NONBLOCK 提交。提交成功只表示“排队成功”，不表示屏幕
     * 已经切到这张图。
     *
     * 真正切换完成后，内核发事件，drmHandleEvents() 调到这里。
     * 状态在这里完成流转：
     *
     *   pending_idx -> front_idx
     *   old front   -> FB_FREE
     *
     * 外部三缓冲循环要根据这个状态判断哪个 buffer 可以重新写。
     */
    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;

    DRM_Ctx *ctx = data;
    if (!ctx || ctx->pool.pending_idx < 0) {
        return;
    }

    if (ctx->pool.front_idx >= 0 && ctx->pool.front_idx != ctx->pool.pending_idx) {
        ctx->pool.drm_fds[ctx->pool.front_idx].state = FB_FREE;
    }

    ctx->pool.front_idx = ctx->pool.pending_idx;
    ctx->pool.drm_fds[ctx->pool.front_idx].state = FB_FRONT;
    ctx->pool.pending_idx = -1;
}

int drmDisplaySetupConfig(DRM_Ctx *ctx, const DRM_Display_Config *cfg)
{
    /*
     * 上层显示配置入口。
     *
     * 这个函数不提交 framebuffer，它只决定“之后要往哪里显示”：
     *
     * 1. 选择 connector
     *      cfg->connector_id != 0 时使用外部指定的 connector。
     *      否则自动选择第一个 connected 且有 mode 的 connector。
     *
     * 2. 选择 crtc
     *      cfg->crtc_id != 0 时使用外部指定的 crtc。
     *      否则根据 connector 当前 encoder/可用 encoder 自动找 crtc。
     *
     * 3. 选择 mode
     *      支持直接指定 mode、指定 mode_index、指定 w/h/fps、或者默认第 0 个。
     *
     * 4. 选择 plane
     *      cfg->plane_id != 0 时使用外部指定的 plane。
     *      否则按 cfg->fmt 自动找支持这个格式且能挂到 crtc 的 plane。
     *
     * 5. 保存 SRC/CRTC 区域
     *      SRC 是从原图取哪块；CRTC 是显示到屏幕哪里、多大。
     *
     * 6. 缓存 atomic property id
     *      后续 submit 不再按字符串查属性。
     */
    if (!ctx || ctx->drm_fd < 0 || !cfg || !cfg->fmt) {
        return -1;
    }

    int conn_index = cfg->connector_id ? findConnectorIndex(ctx, cfg->connector_id) :
                                         chooseConnectedConnector(ctx);
    if (conn_index < 0) {
        printf("no connector available, requested connector=%u\n", cfg->connector_id);
        return -1;
    }

    const Connector_Info *conn = &ctx->res.connector_infos[conn_index];
    uint32_t crtc_id = cfg->crtc_id ? cfg->crtc_id : chooseCrtcForConnector(ctx, conn);
    if (!crtc_id) {
        printf("no crtc for connector %u\n", conn->id);
        return -1;
    }

    drmModeModeInfo mode;
    if (chooseModeForConfig(conn, cfg, &mode) != 0) {
        printf("no mode matched, connector=%u mode_index=%d mode=%dx%d@%d\n",
               conn->id, cfg->mode_index, cfg->mode_w, cfg->mode_h, cfg->mode_fps);
        return -1;
    }

    int plane_index = cfg->plane_id ? findPlaneIndex(ctx, cfg->plane_id) :
                                      choosePlaneForFormat(ctx, crtc_id, cfg->fmt);
    if (plane_index < 0) {
        char fmt_str[16];
        printf("no plane available, requested plane=%u fmt=%s(%s) crtc=%u\n",
               cfg->plane_id, drmFormatToString(cfg->fmt, fmt_str), drmFormatToName(cfg->fmt), crtc_id);
        return -1;
    }

    const Plane_Info *plane = &ctx->res.plane_infos[plane_index];
    if (!planeSupportsCrtc(plane, crtc_id)) {
        printf("plane %u does not support crtc %u\n", plane->id, crtc_id);
        return -1;
    }

    if (!planeSupportsFormat(plane, cfg->fmt)) {
        char fmt_str[16];
        printf("plane %u does not support fmt %s(%s)\n",
               plane->id, drmFormatToString(cfg->fmt, fmt_str), drmFormatToName(cfg->fmt));
        return -1;
    }

    ctx->selected_connector_id = conn->id;
    ctx->selected_crtc_id = crtc_id;
    ctx->selected_plane_id = plane->id;
    ctx->selected_mode = mode;
    ctx->src_x = cfg->src_x;
    ctx->src_y = cfg->src_y;
    ctx->src_w = cfg->src_w;
    ctx->src_h = cfg->src_h;
    ctx->dst_x = cfg->crtc_x;
    ctx->dst_y = cfg->crtc_y;
    ctx->dst_w = cfg->crtc_w > 0 ? cfg->crtc_w : ctx->selected_mode.hdisplay;
    ctx->dst_h = cfg->crtc_h > 0 ? cfg->crtc_h : ctx->selected_mode.vdisplay;
    ctx->modeset_done = 0;
    ctx->pool.front_idx = -1;
    ctx->pool.pending_idx = -1;

    if (drmAtomicCacheProps(ctx, ctx->selected_connector_id,
                            ctx->selected_crtc_id,
                            ctx->selected_plane_id) != 0) {
        return -1;
    }

    printf("display setup: conn=%u crtc=%u plane=%u fmt=%s src=%d,%d %dx%d dst=%d,%d %dx%d mode=%s\n",
           ctx->selected_connector_id, ctx->selected_crtc_id, ctx->selected_plane_id,
           drmFormatToName(cfg->fmt),
           ctx->src_x, ctx->src_y, ctx->src_w, ctx->src_h,
           ctx->dst_x, ctx->dst_y, ctx->dst_w, ctx->dst_h,
           ctx->selected_mode.name);

    return 0;
}

int drmDisplaySetup(DRM_Ctx *ctx, uint32_t fmt, int x, int y, int w, int h)
{
    /*
     * 简化版 setup。
     *
     * 只指定格式和屏幕显示区域，其他全部自动选择：
     * - connector 自动选 connected。
     * - crtc 自动从 encoder 关系里找。
     * - plane 自动按 fmt 匹配。
     * - mode 默认使用 connector 的第 0 个 mode。
     *
     * x/y/w/h 是 CRTC 区域，也就是“显示到屏幕哪里、多大”。
     */
    DRM_Display_Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fmt = fmt;
    cfg.mode_index = -1;
    cfg.crtc_x = x;
    cfg.crtc_y = y;
    cfg.crtc_w = w;
    cfg.crtc_h = h;

    return drmDisplaySetupConfig(ctx, &cfg);
}

int drmDisplaySubmit(DRM_Ctx *ctx, const DRM_Buf *buf)
{
    /*
     * 提交一帧外部 dma-buf。
     *
     * 这是推荐给外部模块调用的主入口。它把复杂流程包起来：
     *
     * 1. 检查上一帧是否还 pending
     *      如果 pending_idx >= 0，说明 page flip event 还没回来。
     *      此时返回 -1/EAGAIN，外部应先调用 drmHandleEvents()。
     *
     * 2. 规范化 DRM_Buf
     *      如果 pitches[] 没填，内部按格式估算。真实项目里还是建议外部填准。
     *
     * 3. 自动 setup
     *      如果外部没先调用 drmDisplaySetupConfig()，这里会按 buf->fmt 自动配置。
     *
     * 4. dma_fd -> fb_id
     *      缓存里有就复用，没有就 import 并 addFB2。
     *
     * 5. 第一次提交
     *      调 drmAtomicCommitModePlane()，完整设置 connector/crtc/plane。
     *
     * 6. 后续提交
     *      只更新 Plane.FB_ID，并请求 page flip event。
     */
    if (!ctx || ctx->drm_fd < 0 || !buf || buf->dma_fd < 0 || buf->w <= 0 || buf->h <= 0) {
        return -1;
    }

    if (ctx->pool.pending_idx >= 0) {
        errno = EAGAIN;
        return -1;
    }

    DRM_Buf normalized;
    normalizeBuf(buf, &normalized);

    if (!ctx->selected_plane_id) {
        DRM_Display_Config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.fmt = normalized.fmt;
        cfg.mode_index = -1;
        cfg.src_w = normalized.w;
        cfg.src_h = normalized.h;
        cfg.crtc_w = normalized.w;
        cfg.crtc_h = normalized.h;

        if (drmDisplaySetupConfig(ctx, &cfg) != 0) {
            return -1;
        }
    }

    int fb_index = getOrCreateFb(ctx, &normalized);
    if (fb_index < 0) {
        return -1;
    }

    DRM_Fb *fb = &ctx->pool.drm_fds[fb_index];
    int ret;

    if (!ctx->modeset_done) {
        ret = drmAtomicCommitModePlane(ctx,
                                       ctx->selected_connector_id,
                                       ctx->selected_crtc_id,
                                       ctx->selected_plane_id,
                                       fb->fb_id,
                                       &ctx->selected_mode,
                                       ctx->src_x,
                                       ctx->src_y,
                                       ctx->src_w > 0 ? ctx->src_w : normalized.w,
                                       ctx->src_h > 0 ? ctx->src_h : normalized.h,
                                       ctx->dst_x,
                                       ctx->dst_y,
                                       ctx->dst_w,
                                       ctx->dst_h);
        if (ret != 0) {
            return -1;
        }

        ctx->modeset_done = 1;
        ctx->pool.front_idx = fb_index;
        fb->state = FB_FRONT;
        return 0;
    }

    drmModeAtomicReq *req = atomicReqBegin(ctx);
    if (!req) {
        return -1;
    }

    drmModeAtomicAddProperty(req,
                             ctx->selected_plane_id,
                             ctx->atomic.plane.fb_id,
                             fb->fb_id);

    ret = drmModeAtomicCommit(ctx->drm_fd, req,
                              DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK,
                              ctx);
    if (ret != 0) {
        perror("drmModeAtomicCommit(display fb)");
        atomicReqReset(ctx);
        return -1;
    }

    atomicReqReset(ctx);
    ctx->pool.pending_idx = fb_index;
    fb->state = FB_QUEUED;
    return 0;
}

int drmHandleEvents(DRM_Ctx *ctx, int timeout_ms)
{
    /*
     * 处理 DRM 事件。
     *
     * 后续切 FB_ID 时使用了 DRM_MODE_PAGE_FLIP_EVENT，所以必须有人消费
     * drm_fd 上的事件。这个函数内部 poll 等待，然后调用 drmHandleEvent()。
     *
     * timeout_ms:
     * - 0：不阻塞，立即检查。
     * - >0：最多等待 timeout_ms 毫秒。
     * - -1：一直等待，直到有事件。
     */
    if (!ctx || ctx->drm_fd < 0) {
        return -1;
    }

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = ctx->drm_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        return ret;
    }

    drmEventContext ev;
    memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = pageFlipHandler;

    return drmHandleEvent(ctx->drm_fd, &ev);
}

const char *drmFormatToString(uint32_t fmt, char out[16])
{
    out[0] = fmt & 0xff;
    out[1] = (fmt >> 8) & 0xff;
    out[2] = (fmt >> 16) & 0xff;
    out[3] = (fmt >> 24) & 0xff;
    out[4] = '\0';

    for (int i = 0; i < 4; i++) {
        if (out[i] < 32 || out[i] > 126) {
            snprintf(out, 16, "0x%08x", fmt);
            break;
        }
    }

    return out;
}

const char *drmFormatToName(uint32_t fmt)
{
    switch (fmt) {
    case DRM_FORMAT_R8: return "R8";
    case DRM_FORMAT_R10: return "R10";
    case DRM_FORMAT_RGB565: return "RGB565";
    case DRM_FORMAT_BGR565: return "BGR565";
    case DRM_FORMAT_RGB888: return "RGB888";
    case DRM_FORMAT_BGR888: return "BGR888";
    case DRM_FORMAT_XRGB8888: return "XRGB8888";
    case DRM_FORMAT_ARGB8888: return "ARGB8888";
    case DRM_FORMAT_XBGR8888: return "XBGR8888";
    case DRM_FORMAT_ABGR8888: return "ABGR8888";
    case DRM_FORMAT_XRGB2101010: return "XRGB2101010";
    case DRM_FORMAT_ARGB2101010: return "ARGB2101010";
    case DRM_FORMAT_XBGR2101010: return "XBGR2101010";
    case DRM_FORMAT_ABGR2101010: return "ABGR2101010";
    case DRM_FORMAT_YUV420_8BIT: return "YUV420_8BIT";
    case DRM_FORMAT_YUV420_10BIT: return "YUV420_10BIT";
    case DRM_FORMAT_YUYV: return "YUYV 4:2:2 packed";
    case DRM_FORMAT_UYVY: return "UYVY 4:2:2 packed";
    case DRM_FORMAT_Y210: return "Y210 4:2:2 packed 10-bit";
    case DRM_FORMAT_NV12: return "NV12 4:2:0 8-bit";
    case DRM_FORMAT_NV16: return "NV16 4:2:2 8-bit";
    case DRM_FORMAT_NV24: return "NV24 4:4:4 8-bit";
    case DRM_FORMAT_NV15: return "NV15 4:2:0 10-bit packed";
    case fourcc_code('N', 'V', '2', '0'): return "NV20 4:2:2 10-bit packed";
    case fourcc_code('N', 'V', '3', '0'): return "NV30 4:4:4 10-bit packed";
    default: return "Unknown";
    }
}

static int calcRefreshHz(const drmModeModeInfo *mode)
{
    if (!mode || mode->htotal == 0 || mode->vtotal == 0) {
        return 0;
    }

    return (int)(((uint64_t)mode->clock * 1000 + ((uint64_t)mode->htotal * mode->vtotal / 2)) /
                 ((uint64_t)mode->htotal * mode->vtotal));
}

// 输入：possible_crtcs(比特掩码)、res(所有CRTC资源)、out_ids(输出数组，存支持的CRTC ID)、max_ids(数组最大容量)
// 输出：返回这个编码器支持的CRTC数量
static int fillPossibleCrtcs(uint32_t possible_crtcs, const drmModeRes *res,
                             uint32_t out_ids[], int max_ids)
{
    int count = 0; // 记录支持的CRTC个数

    // 遍历显卡所有的CRTC（i是CRTC的索引，从0开始）
    // 两个终止条件：遍历完所有CRTC 或 输出数组存满
    for (int i = 0; i < res->count_crtcs && count < max_ids; i++) {
        // 核心判断：检查掩码的第i位是否为1 → 表示编码器支持第i个CRTC
        // 1u << i：把1左移i位，生成只在第i位为1的掩码
        // & 运算：如果结果非0，说明这一位是1
        if (possible_crtcs & (1u << i)) {
            // 把这个CRTC的真实硬件ID存入输出数组
            out_ids[count++] = res->crtcs[i];
        }
    }

    return count; // 返回支持的CRTC总数量
}

static void fillPlaneType(int fd, Plane_Info *info)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, info->id, DRM_MODE_OBJECT_PLANE);

    strcpy(info->type, "Unknown");
    if (!props) {
        return;
    }

    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) {
            continue;
        }

        if (strcmp(prop->name, "type") == 0) {
            snprintf(info->type, sizeof(info->type), "%s", planeTypeToString(props->prop_values[i]));
            drmModeFreeProperty(prop);
            break;
        }

        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
}

static void scanCrtcs(int fd, drmModeRes *res, DRM_Res *out)
{
    out->crtc_count = res->count_crtcs < DRM_MAX_CRTCS ? res->count_crtcs : DRM_MAX_CRTCS;

    for (int i = 0; i < out->crtc_count; i++) {
        CRTC_Info *info = &out->crtc_infos[i];
        drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[i]);

        memset(info, 0, sizeof(*info));
        info->id = res->crtcs[i];
        info->index = i;

        if (!crtc) {
            continue;
        }

        info->buffer_id = crtc->buffer_id;
        info->x = crtc->x;
        info->y = crtc->y;
        info->w = crtc->width;
        info->h = crtc->height;
        info->mode_valid = crtc->mode_valid;
        if (crtc->mode_valid) {
            info->raw_mode = crtc->mode;
        }
        snprintf(info->mode_name, sizeof(info->mode_name), "%s", crtc->mode_valid ? crtc->mode.name : "");

        drmModeFreeCrtc(crtc);
    }
}

static void scanEncoders(int fd, drmModeRes *res, DRM_Res *out)
{
    out->encoder_count = res->count_encoders < DRM_MAX_ENCODERS ? res->count_encoders : DRM_MAX_ENCODERS;

    for (int i = 0; i < out->encoder_count; i++) {
        Encoder_Info *info = &out->encoder_infos[i];
        drmModeEncoder *enc = drmModeGetEncoder(fd, res->encoders[i]);

        memset(info, 0, sizeof(*info));
        info->id = res->encoders[i];

        if (!enc) {
            strcpy(info->type, "Unknown");
            continue;
        }

        info->crtc_id = enc->crtc_id;

        //possible_crtcs：一个比特位掩码，表示这个编码器可以绑定哪些 CRTC 工作（比如第 0 位 = 1 表示支持第 0 个 CRTC）。
        info->possible_crtcs = enc->possible_crtcs;

        info->possible_clones = enc->possible_clones;



        info->possible_crtc_count = fillPossibleCrtcs(enc->possible_crtcs, res,
                                                      info->possible_crtc_ids,
                                                      DRM_MAX_POSSIBLE_IDS);
        snprintf(info->type, sizeof(info->type), "%s", encoderTypeToString(enc->encoder_type));

        drmModeFreeEncoder(enc);
    }
}

static void scanConnectors(int fd, drmModeRes *res, DRM_Res *out)
{
    out->connector_count = res->count_connectors < DRM_MAX_CONNECTORS ?
                           res->count_connectors : DRM_MAX_CONNECTORS;

    for (int i = 0; i < out->connector_count; i++) {
        Connector_Info *info = &out->connector_infos[i];
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);

        memset(info, 0, sizeof(*info));
        info->id = res->connectors[i];

        if (!conn) {
            strcpy(info->type, "Unknown");
            continue;
        }

        info->encoder_id = conn->encoder_id;
        info->connector_type = conn->connector_type;
        info->connector_type_id = conn->connector_type_id;
        info->mm_width = conn->mmWidth;
        info->mm_height = conn->mmHeight;
        info->connected = conn->connection == DRM_MODE_CONNECTED;
        snprintf(info->type, sizeof(info->type), "%s", connectorTypeToString(conn->connector_type));

        info->encoder_count = conn->count_encoders < DRM_MAX_ENCODERS ?
                              conn->count_encoders : DRM_MAX_ENCODERS;
        for (int j = 0; j < info->encoder_count; j++) {
            info->encoder_ids[j] = conn->encoders[j];
        }

        info->mode_count = conn->count_modes < DRM_MAX_MODES ? conn->count_modes : DRM_MAX_MODES;
        for (int j = 0; j < info->mode_count; j++) {
            DRM_Mode_Info *mode = &info->drm_mode_infos[j];

            memset(mode, 0, sizeof(*mode));
            mode->w = conn->modes[j].hdisplay;
            mode->h = conn->modes[j].vdisplay;
            mode->fps = calcRefreshHz(&conn->modes[j]);
            mode->index = j;
            mode->flags = conn->modes[j].flags;
            mode->type = conn->modes[j].type;
            mode->raw = conn->modes[j];
            snprintf(mode->name, sizeof(mode->name), "%s", conn->modes[j].name);
        }

        drmModeFreeConnector(conn);
    }
}

static void scanPlanes(int fd, drmModeRes *res, DRM_Res *out)
{
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res) {
        perror("drmModeGetPlaneResources");
        return;
    }

    out->plane_count = plane_res->count_planes < DRM_MAX_PLANES ?
                       plane_res->count_planes : DRM_MAX_PLANES;

    for (int i = 0; i < out->plane_count; i++) {
        Plane_Info *info = &out->plane_infos[i];
        drmModePlane *plane = drmModeGetPlane(fd, plane_res->planes[i]);

        memset(info, 0, sizeof(*info));
        info->id = plane_res->planes[i];

        if (!plane) {
            strcpy(info->type, "Unknown");
            continue;
        }

        info->crtc_id = plane->crtc_id;
        info->fb_id = plane->fb_id;
        info->possible_crtcs = plane->possible_crtcs;
        info->possible_crtc_count = fillPossibleCrtcs(plane->possible_crtcs, res,
                                                      info->possible_crtc_ids,
                                                      DRM_MAX_POSSIBLE_IDS);
        info->busy = plane->fb_id != 0;
        info->format_count = plane->count_formats < DRM_MAX_FORMATS ?
                             plane->count_formats : DRM_MAX_FORMATS;

        for (int j = 0; j < info->format_count; j++) {
            info->support_formats[j].fmt = plane->formats[j];
            drmFormatToString(plane->formats[j], info->support_formats[j].fmt_str);
            snprintf(info->support_formats[j].fmt_name,
                     sizeof(info->support_formats[j].fmt_name),
                     "%s", drmFormatToName(plane->formats[j]));
        }

        fillPlaneType(fd, info);
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(plane_res);
}

int drmInit(DRM_Ctx *ctx)
{
    const char *card = "/dev/dri/card0";

    if (!ctx) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->drm_fd = -1;
    ctx->pool.front_idx = -1;
    ctx->pool.pending_idx = -1;

    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(card);
        return -1;
    }

    ctx->drm_fd = fd;
    ctx->atomic_req = drmModeAtomicAlloc();
    if (!ctx->atomic_req) {
        printf("drmModeAtomicAlloc failed\n");
        drmDeinit(ctx);
        return -1;
    }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        perror("drmSetClientCap(UNIVERSAL_PLANES)");
    }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        perror("drmSetClientCap(ATOMIC)");
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        perror("drmModeGetResources");
        drmDeinit(ctx);
        return -1;
    }

    scanCrtcs(fd, res, &ctx->res);
    scanEncoders(fd, res, &ctx->res);
    scanConnectors(fd, res, &ctx->res);
    scanPlanes(fd, res, &ctx->res);

    drmModeFreeResources(res);
    return 0;
}

void drmDeinit(DRM_Ctx *ctx)
{
    if (!ctx) {
        return;
    }

    for (int i = 0; i < FB_COUNT; i++) {
        releaseFbSlot(ctx, i);
    }

    if (ctx->atomic_req) {
        drmModeAtomicFree(ctx->atomic_req);
        ctx->atomic_req = NULL;
    }

    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
    }

    memset(&ctx->res, 0, sizeof(ctx->res));
    memset(&ctx->atomic, 0, sizeof(ctx->atomic));
}

void drmDumpResources(const DRM_Ctx *ctx)
{
    if (!ctx) {
        return;
    }

    printf("\n================ DRM resources ================\n");
    printf("fd=%d connectors=%d encoders=%d crtcs=%d planes=%d\n",
           ctx->drm_fd,
           ctx->res.connector_count,
           ctx->res.encoder_count,
           ctx->res.crtc_count,
           ctx->res.plane_count);

    printf("\n[Connectors]\n");
    for (int i = 0; i < ctx->res.connector_count; i++) {
        const Connector_Info *conn = &ctx->res.connector_infos[i];
        printf("  #%d id=%u type=%s-%u %s encoder=%u modes=%d encoders=%d size=%ummx%umm\n",
               i, conn->id, conn->type, conn->connector_type_id,
               conn->connected ? "connected" : "disconnected",
               conn->encoder_id, conn->mode_count, conn->encoder_count,
               conn->mm_width, conn->mm_height);

        for (int j = 0; j < conn->mode_count; j++) {
            const DRM_Mode_Info *mode = &conn->drm_mode_infos[j];
            printf("      mode[%d] %s %dx%d@%d type=0x%x flags=0x%x\n",
                   j, mode->name, mode->w, mode->h, mode->fps,
                   mode->type, mode->flags);
        }
    }

    printf("\n[Encoders]\n");
    for (int i = 0; i < ctx->res.encoder_count; i++) {
        const Encoder_Info *enc = &ctx->res.encoder_infos[i];
        printf("  #%d id=%u type=%s crtc=%u possible_crtcs=0x%x ids:",
               i, enc->id, enc->type, enc->crtc_id, enc->possible_crtcs);
        for (int j = 0; j < enc->possible_crtc_count; j++) {
            printf(" %u", enc->possible_crtc_ids[j]);
        }
        printf("\n");
    }

    printf("\n[CRTCs]\n");
    for (int i = 0; i < ctx->res.crtc_count; i++) {
        const CRTC_Info *crtc = &ctx->res.crtc_infos[i];
        printf("  #%d id=%u fb=%u pos=%d,%d size=%dx%d mode=%s\n",
               i, crtc->id, crtc->buffer_id, crtc->x, crtc->y,
               crtc->w, crtc->h, crtc->mode_name);
    }

    printf("\n[Planes]\n");
    for (int rank = 0; rank < 4; rank++) {
        const char *group_name[] = {"Primary", "Overlay", "Cursor", "Unknown"};
        int printed_group = 0;

        for (int i = 0; i < ctx->res.plane_count; i++) {
            const Plane_Info *plane = &ctx->res.plane_infos[i];
            if (planeTypeRank(plane->type) != rank) {
                continue;
            }

            if (!printed_group) {
                printf("  <%s>\n", group_name[rank]);
                printed_group = 1;
            }

            printf("    plane[%d]\n", i);
            printf("      id              : %u\n", plane->id);
            printf("      type            : %s\n", plane->type);
            printf("      crtc_id         : %u\n", plane->crtc_id);
            printf("      fb_id           : %u\n", plane->fb_id);
            printf("      busy            : %d\n", plane->busy);
            printf("      possible_crtcs  : 0x%x", plane->possible_crtcs);
            for (int j = 0; j < plane->possible_crtc_count; j++) {
                printf(" %u", plane->possible_crtc_ids[j]);
            }
            printf("\n");
            printf("      format_count    : %d\n", plane->format_count);
            printf("      formats\n");
            for (int j = 0; j < plane->format_count; j++) {
                printf("        [%02d] %-4s  %s\n",
                       j,
                       plane->support_formats[j].fmt_str,
                       plane->support_formats[j].fmt_name);
            }
        }
    }
}
