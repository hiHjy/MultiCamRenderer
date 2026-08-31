# DRM Display 使用说明

这个文档按你现在这套代码的思路写：外部模块分配图像 buffer，并把 `dma_fd` 传给 DRM；DRM 模块内部负责把 `dma_fd` 包装成 `fb_id`，第一次做完整 atomic modeset，后续只切换 `FB_ID`。

## 总体流程

最常见流程是：

```c
DRM_Ctx drm_ctx;
memset(&drm_ctx, 0, sizeof(drm_ctx));

if (drmInit(&drm_ctx) != 0) {
    return -1;
}

drmDumpResources(&drm_ctx);  // 调试阶段建议打开，确认 connector/crtc/plane/mode

DRM_Display_Config cfg;
memset(&cfg, 0, sizeof(cfg));
cfg.fmt = DRM_FORMAT_XRGB8888;
cfg.mode_index = -1;     // -1 表示不强制 mode 下标，默认选 connector 的第 0 个 mode

cfg.src_x = 0;
cfg.src_y = 0;
cfg.src_w = 1920;
cfg.src_h = 1080;

cfg.crtc_x = 0;
cfg.crtc_y = 0;
cfg.crtc_w = 1920;
cfg.crtc_h = 1080;

if (drmDisplaySetupConfig(&drm_ctx, &cfg) != 0) {
    return -1;
}

while (running) {
    DRM_Buf buf;
    memset(&buf, 0, sizeof(buf));
    buf.dma_fd = dma_fd_from_mpp_or_rga;
    buf.w = 1920;
    buf.h = 1080;
    buf.fmt = DRM_FORMAT_XRGB8888;
    buf.pitches[0] = stride;
    buf.offsets[0] = 0;
    buf.modifier = DRM_FORMAT_MOD_INVALID;

    if (drmDisplaySubmit(&drm_ctx, &buf) != 0) {
        if (errno == EAGAIN) {
            drmHandleEvents(&drm_ctx, 10);
            continue;
        }
        break;
    }

    drmHandleEvents(&drm_ctx, 10);
}

drmDeinit(&drm_ctx);
```

## C++ 里怎么调用

头文件已经支持 C++，内部用 `extern "C"` 包住了接口声明，所以 `.cpp`
文件可以直接包含：

```cpp
#include "drm_display.h"

int main()
{
    DRM_Ctx ctx {};

    if (drmInit(&ctx) != 0) {
        return -1;
    }

    DRM_Display_Config cfg {};
    cfg.fmt = DRM_FORMAT_XRGB8888;
    cfg.mode_index = -1;
    cfg.src_w = 1920;
    cfg.src_h = 1080;
    cfg.crtc_w = 1920;
    cfg.crtc_h = 1080;

    drmDisplaySetupConfig(&ctx, &cfg);

    DRM_Buf buf {};
    buf.dma_fd = dma_fd;
    buf.w = 1920;
    buf.h = 1080;
    buf.fmt = DRM_FORMAT_XRGB8888;
    buf.pitches[0] = stride;
    buf.modifier = DRM_FORMAT_MOD_INVALID;

    drmDisplaySubmit(&ctx, &buf);
    drmDeinit(&ctx);
    return 0;
}
```

如果你的 C++ 工程单独编译这个模块，推荐保持：

```text
drm_display.c 用 C 编译器编译
C++ 业务代码用 C++ 编译器编译
最后一起链接 libdrm
```

## 你真正需要关心的 3 个结构体

### DRM_Ctx

`DRM_Ctx` 是整个 DRM 模块的上下文。外部创建一个，然后传给所有接口。

重要字段：

```c
int drm_fd;
DRM_Res res;
DRM_Buffer_Pool pool;
uint32_t selected_connector_id;
uint32_t selected_crtc_id;
uint32_t selected_plane_id;
drmModeModeInfo selected_mode;
int modeset_done;
```

含义：

- `drm_fd`：打开的 `/dev/dri/card0`。
- `res`：`drmInit()` 扫描到的 connector、crtc、encoder、plane、mode。
- `pool`：内部的三缓冲缓存表，保存 `dma_fd -> fb_id` 映射。
- `selected_connector_id`：当前选中的输出口，比如 HDMI/DSI。
- `selected_crtc_id`：当前选中的 CRTC。
- `selected_plane_id`：当前选中的 plane。
- `selected_mode`：当前设置的显示分辨率。
- `modeset_done`：是否已经做过第一次完整 atomic modeset。

外部一般不要直接改这些字段，除非你在调试。

### DRM_Display_Config

`DRM_Display_Config` 用来描述“怎么显示”。

```c
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
```

字段分三类。

第一类：选显示链路。

- `connector_id`：指定输出口。不填就是 0，内部自动选第一个 connected connector。
- `crtc_id`：指定 CRTC。不填就是 0，内部根据 connector/encoder 自动选。
- `plane_id`：指定 plane。不填就是 0，内部根据 `fmt` 自动找支持这个格式的 plane。

如果外部明确知道 plane 可以用，比如你从 dump 里看到 overlay plane `107` 支持 NV12，可以这样写：

```c
cfg.plane_id = 107;
```

如果指定了 plane，代码仍会检查：

- 这个 plane 是否存在。
- 这个 plane 是否支持当前 `crtc_id`。
- 这个 plane 是否支持 `cfg.fmt`。

第二类：选分辨率，也就是 CRTC mode。

优先级是：

1. `use_mode = 1`：直接使用 `cfg.mode`。
2. `mode_index >= 0`：使用 connector 的 `drm_mode_infos[mode_index]`。
3. `mode_w/mode_h` 大于 0：按分辨率匹配，`mode_fps` 大于 0 时也匹配刷新率。
4. 都不填：使用 connector 的第 0 个 mode。

示例：强制 1920x1080@60。

```c
cfg.mode_index = -1;
cfg.mode_w = 1920;
cfg.mode_h = 1080;
cfg.mode_fps = 60;
```

示例：使用 dump 出来的第 0 个 mode。

```c
cfg.mode_index = 0;
```

第三类：设置图像区域和屏幕区域。

`SRC_X/Y/W/H` 表示从 framebuffer 原图里取哪一块。

```text
SRC_X/SRC_Y：从原图哪个点开始取
SRC_W/SRC_H：取多大的区域
```

`CRTC_X/Y/W/H` 表示显示到屏幕哪里、多大。

```text
CRTC_X/CRTC_Y：显示到屏幕哪个位置
CRTC_W/CRTC_H：显示出来多大
```

例子 1：原图 1920x1080，全屏显示，不缩放。

```c
cfg.src_x = 0;
cfg.src_y = 0;
cfg.src_w = 1920;
cfg.src_h = 1080;

cfg.crtc_x = 0;
cfg.crtc_y = 0;
cfg.crtc_w = 1920;
cfg.crtc_h = 1080;
```

例子 2：原图 640x480，放到屏幕左上角，原始大小显示。

```c
cfg.src_x = 0;
cfg.src_y = 0;
cfg.src_w = 640;
cfg.src_h = 480;

cfg.crtc_x = 0;
cfg.crtc_y = 0;
cfg.crtc_w = 640;
cfg.crtc_h = 480;
```

例子 3：原图 640x480，显示时放大到 1280x720。

```c
cfg.src_x = 0;
cfg.src_y = 0;
cfg.src_w = 640;
cfg.src_h = 480;

cfg.crtc_x = 0;
cfg.crtc_y = 0;
cfg.crtc_w = 1280;
cfg.crtc_h = 720;
```

例子 4：只取原图中间一块，然后显示到屏幕指定位置。

```c
cfg.src_x = 320;
cfg.src_y = 180;
cfg.src_w = 640;
cfg.src_h = 360;

cfg.crtc_x = 100;
cfg.crtc_y = 50;
cfg.crtc_w = 640;
cfg.crtc_h = 360;
```

### DRM_Buf

`DRM_Buf` 是每次提交的一帧。

```c
typedef struct {
    int dma_fd;
    uint64_t size;
    int w;
    int h;
    uint32_t fmt;
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier;
} DRM_Buf;
```

必须填：

- `dma_fd`
- `w`
- `h`
- `fmt`
- `pitches[]`
- `offsets[]`

RGB 单平面例子：

```c
DRM_Buf buf;
memset(&buf, 0, sizeof(buf));
buf.dma_fd = dma_fd;
buf.w = 1920;
buf.h = 1080;
buf.fmt = DRM_FORMAT_XRGB8888;
buf.pitches[0] = stride;
buf.offsets[0] = 0;
buf.modifier = DRM_FORMAT_MOD_INVALID;
```

NV12 两平面例子：

```c
DRM_Buf buf;
memset(&buf, 0, sizeof(buf));
buf.dma_fd = dma_fd;
buf.w = 1920;
buf.h = 1080;
buf.fmt = DRM_FORMAT_NV12;
buf.pitches[0] = y_stride;
buf.pitches[1] = uv_stride;
buf.offsets[0] = 0;
buf.offsets[1] = y_stride * 1080;
buf.modifier = DRM_FORMAT_MOD_INVALID;
```

注意：如果外部 buffer 有特殊 layout，比如 AFBC、tile、厂商私有 modifier，需要把 modifier 填对。不然 `drmModeAddFB2()` 可能成功不了，或者显示花屏。

## 首次提交和后续提交的区别

第一次 `drmDisplaySubmit()` 时，内部会调用：

```c
drmAtomicCommitModePlane()
```

这次提交会设置：

```text
Connector.CRTC_ID
CRTC.MODE_ID
CRTC.ACTIVE
Plane.FB_ID
Plane.CRTC_ID
Plane.SRC_X/Y/W/H
Plane.CRTC_X/Y/W/H
```

这叫完整 modeset，必须带：

```c
DRM_MODE_ATOMIC_ALLOW_MODESET
```

后续 `drmDisplaySubmit()` 时，内部只改：

```text
Plane.FB_ID
```

这就是翻页/切帧，开销更小。

## dma_fd 到 fb_id 的缓存

外部传进来的只是 `dma_fd`，DRM 不能直接显示 dma_fd，必须先变成 fb_id：

```text
dma_fd
  -> drmPrimeFDToHandle()
  -> GEM handle
  -> drmModeAddFB2()
  -> fb_id
```

这个过程在 `drmDisplaySubmit()` 内部自动完成。

内部缓存规则：

- 如果同一个 `dma_fd + w/h/fmt/pitches/offsets` 已经包装过，直接复用旧 `fb_id`。
- 如果没包装过，找一个 `FB_FREE` 槽位包装。
- 如果三缓冲都在用，且没有空闲槽位，提交会失败。

## 三缓冲状态

内部最多缓存 `FB_COUNT=3` 个 framebuffer。

状态流转：

```text
FB_FREE
  -> drmDisplaySubmit()
  -> FB_QUEUED
  -> page flip event
  -> FB_FRONT

旧 FB_FRONT
  -> page flip event
  -> FB_FREE
```

如果 `pending_idx >= 0`，表示上一帧已经提交但 page flip 还没完成。

此时再调用 `drmDisplaySubmit()` 会返回：

```c
-1
errno = EAGAIN
```

外部应该先调用：

```c
drmHandleEvents(&ctx, 10);
```

等 page flip 回调回来，再提交下一帧。

## 常见错误

### 1. plane 不支持格式

比如你选择了 primary plane，但它不支持 NV12，会打印类似：

```text
plane 57 does not support fmt NV12(...)
```

解决办法：

- 不指定 `cfg.plane_id`，让内部自动选。
- 或者从 `drmDumpResources()` 里找支持 NV12 的 overlay plane。

### 2. plane 不支持 crtc

一个 plane 不是一定能挂到所有 CRTC 上，要看：

```text
possible_crtc_ids
```

如果强指定了错误组合，会提交失败。

### 3. pitches/offsets 不对

RGB 通常比较简单，NV12 很容易错。

NV12 一般是：

```text
Y plane  offset = 0
UV plane offset = y_stride * height
```

但具体还是以外部分配器返回的真实布局为准。

### 4. 首次提交后屏幕没变化

检查：

- connector 是否 connected。
- mode 是否匹配屏幕。
- plane 是否支持这个 crtc 和 fmt。
- `drmModeAddFB2()` 是否成功。
- `drmAtomicCommit(TEST_ONLY modeset)` 是否失败。

### 5. 后续提交总是 EAGAIN

说明 page flip event 没处理。

主循环里要调用：

```c
drmHandleEvents(&ctx, 0);
```

或者用 poll/epoll 监听 `ctx.drm_fd`，可读时调用 `drmHandleEvent()`。

## 推荐接入顺序

第一步：XRGB8888 dumb buffer 测试。

确认纯色能显示。

第二步：外部 dma_fd RGB 测试。

比如 RGA 输出 XRGB8888，然后传给 `drmDisplaySubmit()`。

第三步：NV12 overlay plane 测试。

从 `drmDumpResources()` 里选支持 NV12 的 overlay plane：

```c
cfg.plane_id = 107;  // 举例，以实际 dump 为准
cfg.fmt = DRM_FORMAT_NV12;
```

第四步：三缓冲和 page flip。

确认状态流转：

```text
FREE -> QUEUED -> FRONT
```

第五步：缩放/裁剪/窗口显示。

调整 `SRC_*` 和 `CRTC_*`。
