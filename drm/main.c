
#include "drm_display.h"
#include <sys/mman.h>

int main(int argc, char const *argv[])
{
    (void)argc;
    (void)argv;

    DRM_Ctx drm_ctx;

    if (drmInit(&drm_ctx) != 0) {
        return 1;
    }

    drmDumpResources(&drm_ctx);

    int width = 1920;
    int height = 1080;

    struct drm_mode_create_dumb create = {
        .width = width,
        .height = height,
        .bpp = 32,               // XRGB8888
        .flags = 0
    };
    int ret = drmIoctl(drm_ctx.drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    if (ret < 0) {
        perror("drmIoctl CREATE_DUMB");
        close(drm_ctx.drm_fd);
        return -1;
    }

    printf("DUMB buffer created:\n");
    printf("  handle: %u\n", create.handle);
    printf("  pitch: %u\n", create.pitch);
    printf("  size: %llu\n", (unsigned long long)create.size);



    struct drm_mode_map_dumb map = {
        .handle = create.handle,
    };
    ret = drmIoctl(drm_ctx.drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    if (ret < 0) {
        perror("drmIoctl MAP_DUMB");
        return -1;
    }

    uint32_t *p = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       drm_ctx.drm_fd, map.offset);
    if (p == MAP_FAILED) {
        perror("mmap dumb");
        return -1;
    }

    for (uint32_t y = 0; y < create.height; y++) {
        uint32_t *line = (uint32_t *)((uint8_t *)p + y * create.pitch);
        for (uint32_t x = 0; x < create.width; x++) {
            line[x] = 0x00ff4040;
        }
    }

    int dma_fd = -1;
    ret = drmPrimeHandleToFD(drm_ctx.drm_fd, create.handle, DRM_CLOEXEC | DRM_RDWR, &dma_fd);
    if (ret != 0) {
        perror("drmPrimeHandleToFD");
        return -1;
    }

    DRM_Buf buf = {
        .dma_fd = dma_fd,
        .size = create.size,
        .w = create.width,
        .h = create.height,
        .fmt = DRM_FORMAT_XRGB8888,
        .pitches = { create.pitch, 0, 0, 0 },
        .offsets = { 0, 0, 0, 0 },
        .modifier = DRM_FORMAT_MOD_INVALID,
    };

    DRM_Display_Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fmt = buf.fmt;
    cfg.mode_index = -1;
    cfg.src_x = 0;
    cfg.src_y = 0;
    cfg.src_w = create.width;
    cfg.src_h = create.height;
    cfg.crtc_x = 0;
    cfg.crtc_y = 0;
    cfg.crtc_w = create.width;
    cfg.crtc_h = create.height;

    ret = drmDisplaySetupConfig(&drm_ctx, &cfg);
    if (ret != 0) {
        printf("drmDisplaySetup failed\n");
        return -1;
    }

    ret = drmDisplaySubmit(&drm_ctx, &buf);
    if (ret != 0) {
        printf("drmDisplaySubmit failed\n");
        return -1;
    }

    sleep(5);
    close(dma_fd);
    munmap(p, create.size);
    struct drm_mode_destroy_dumb destroy = {
        .handle = create.handle,
    };
    drmIoctl(drm_ctx.drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    drmDeinit(&drm_ctx);
    return 0;
}
