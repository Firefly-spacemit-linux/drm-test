/*
 * DRM Atomic Modesetting with Multiple RDMA Channels
 * 使用4个plane分别显示4个RDMA通道 - 单次显示版本
 * 仅支持ARGB8888格式
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <math.h>
#include <drm/drm_fourcc.h>
#include <stdarg.h>

#define MAX_RDMA_CHANNELS 4
static int DEBUG_LEVEL = 1;  // 0=无, 1=基本, 2=详细, 3=调试

/* 每个RDMA通道的配置 */
typedef struct {
    int crtc_x;      // 在CRTC上的X位置
    int crtc_y;      // 在CRTC上的Y位置
    int crtc_w;      // 在CRTC上的宽度
    int crtc_h;      // 在CRTC上的高度
    uint32_t rdma_id; // RDMA通道ID
    uint8_t base_r;  // 基础红色分量
    uint8_t base_g;  // 基础绿色分量
    uint8_t base_b;  // 基础蓝色分量
    uint32_t plane_id; // 使用的plane ID
} rdma_channel_config;

/* 4个RDMA通道的配置 - 使用plane 55, 67, 79, 91 */
static rdma_channel_config rdma_configs[MAX_RDMA_CHANNELS] = {
    {0,    0,    480, 256, 0, 255, 0,   0, 31},   // 左上角 - 红色 - plane 31
    {480,  0,    480, 256, 1, 0,   255, 0, 43},   // 右上角 - 绿色 - plane 43
    {0,    256,  480, 256, 2, 0,   0,   255, 55}, // 左下角 - 蓝色 - plane 55
    {480,  256,  480, 256, 3, 255, 255, 0, 67}    // 右下角 - 黄色 - plane 67
};

static const uint32_t FORMAT = DRM_FORMAT_ARGB8888;  // 固定使用ARGB8888格式
static int channel_width = 480, channel_height = 256;  // 每个通道的大小

struct drm_object {
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
    uint32_t id;
};

struct modeset_buf {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint8_t *map;
    uint32_t fb;
};

/* 每个输出结构对应一个plane */
struct output {
    struct output *next;
    struct modeset_buf buf;  // 只需要一个buffer
    struct drm_object plane;
    uint32_t crtc_id;
    
    /* 通道配置索引 */
    int channel_idx;
    
    /* 颜色 */
    uint8_t r, g, b;
};

static struct output *output_list = NULL;

/* 调试打印函数 */
static void debug_print(int level, const char *format, ...)
{
    if (level <= DEBUG_LEVEL) {
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
    }
}

void print_usage(const char *program_name)
{
    fprintf(stderr, "Usage: %s [options]\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d <device>     Specify the video device (default: /dev/dri/card1)\n");
    fprintf(stderr, "  -w <width>      Set the image width for each channel (default: 480)\n");
    fprintf(stderr, "  -h <height>     Set the image height for each channel (default: 256)\n");
    fprintf(stderr, "  -c <crtc>       Set CRTC ID to use (default: find automatically)\n");
    fprintf(stderr, "  -t <seconds>    Run for specified seconds (default: 5)\n");
    fprintf(stderr, "  -v              Verbose output\n");
    fprintf(stderr, "  -vv             Very verbose output\n");
    fprintf(stderr, "\nRDMA Channels Configuration (4 channels using 4 planes):\n");
    fprintf(stderr, "  Channel 0: Top-left     (0, 0)     - Red, RDMA ID 0, Plane 31\n");
    fprintf(stderr, "  Channel 1: Top-right    (480, 0)   - Green, RDMA ID 1, Plane 43\n");
    fprintf(stderr, "  Channel 2: Bottom-left  (0, 256)   - Blue, RDMA ID 2, Plane 55\n");
    fprintf(stderr, "  Channel 3: Bottom-right (480, 256) - Yellow, RDMA ID 3, Plane 67\n");
}

static int modeset_open(int *out, const char *node)
{
    int fd, ret;
    uint64_t cap;
    
    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ret = -errno;
        fprintf(stderr, "cannot open '%s': %m\n", node);
        return ret;
    }
    
    debug_print(1, "Opened DRM device: %s (fd=%d)\n", node, fd);
    
    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret) {
        fprintf(stderr, "failed to set universal planes cap, %d\n", ret);
        close(fd);
        return ret;
    }
    
    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret) {
        fprintf(stderr, "failed to set atomic cap, %d", ret);
        close(fd);
        return ret;
    }
    
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap) {
        fprintf(stderr, "drm device '%s' does not support dumb buffers\n", node);
        close(fd);
        return -EOPNOTSUPP;
    }
    
    *out = fd;
    return 0;
}

static void modeset_get_object_properties(int fd, struct drm_object *obj,
                                          uint32_t type)
{
    obj->props = drmModeObjectGetProperties(fd, obj->id, type);
    if (!obj->props) {
        fprintf(stderr, "cannot get object %d properties: %s\n",
                obj->id, strerror(errno));
        return;
    }
    
    obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
    for (unsigned int i = 0; i < obj->props->count_props; i++)
        obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
}

static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj,
                                   const char *name, uint64_t value)
{
    int i;
    uint32_t prop_id = 0;
    
    for (i = 0; i < obj->props->count_props; i++) {
        if (obj->props_info[i] && !strcmp(obj->props_info[i]->name, name)) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }
    
    if (prop_id == 0) {
        debug_print(2, "no object property: %s\n", name);
        return -EINVAL;
    }
    
    return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

/* 检查plane是否可用并可以被指定的CRTC使用 */
static int check_plane_availability(int fd, uint32_t plane_id, uint32_t crtc_id, int crtc_index)
{
    drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
    if (!plane) {
        fprintf(stderr, "Failed to get plane %u\n", plane_id);
        return -1;
    }
    
    debug_print(3, "Checking plane %u:\n", plane_id);
    debug_print(3, "  fb_id=%u, crtc_id=%u\n", plane->fb_id, plane->crtc_id);
    debug_print(3, "  possible_crtcs=0x%x, crtc_index=%d\n", plane->possible_crtcs, crtc_index);
    
    /* 检查plane是否可以被指定的CRTC使用 */
    if (!(plane->possible_crtcs & (1 << crtc_index))) {
        fprintf(stderr, "Plane %u cannot be used with CRTC %u (possible_crtcs=0x%x, crtc_index=%d)\n", 
                plane_id, crtc_id, plane->possible_crtcs, crtc_index);
        drmModeFreePlane(plane);
        return -1;
    }
    
    drmModeFreePlane(plane);
    return 0;
}

static void modeset_drm_object_fini(struct drm_object *obj)
{
    if (!obj->props) return;
    
    for (int i = 0; i < obj->props->count_props; i++) {
        if (obj->props_info[i])
            drmModeFreeProperty(obj->props_info[i]);
    }
    free(obj->props_info);
    drmModeFreeObjectProperties(obj->props);
}

static int modeset_setup_objects(int fd, struct output *out)
{
    struct drm_object *plane = &out->plane;
    
    modeset_get_object_properties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!plane->props)
        return -ENOMEM;
    
    /* 打印plane的属性用于调试 */
    debug_print(2, "Plane %u properties:\n", plane->id);
    for (int i = 0; i < plane->props->count_props; i++) {
        if (plane->props_info[i]) {
            debug_print(2, "  [%d] %s = %lu\n", i, 
                       plane->props_info[i]->name,
                       plane->props->prop_values[i]);
        }
    }
    
    return 0;
}

static void modeset_destroy_objects(int fd, struct output *out)
{
    modeset_drm_object_fini(&out->plane);
}

static int modeset_create_fb(int fd, struct modeset_buf *buf, int channel_idx)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_destroy_dumb dreq;
    struct drm_mode_map_dumb mreq;
    int ret;
    
    /* 创建dumb buffer - ARGB8888格式，32bpp */
    memset(&creq, 0, sizeof(creq));
    creq.width = buf->width;
    creq.height = buf->height;
    creq.bpp = 32;
    
    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
        fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
        return -errno;
    }
    
    buf->stride = creq.pitch;
    buf->size = creq.size;
    buf->handle = creq.handle;
    
    /* 验证 stride 是否合理 */
    uint32_t expected_stride = buf->width * 4;  // 每像素4字节
    if (buf->stride != expected_stride) {
        debug_print(1, "WARNING: stride (%u) != width * 4 (%u)\n", 
                   buf->stride, expected_stride);
        debug_print(1, "  width=%u, height=%u, stride=%u, size=%u\n",
                   buf->width, buf->height, buf->stride, buf->size);
    }
    
    /* 创建framebuffer */
    uint32_t handles[4] = {buf->handle, 0, 0, 0};
    uint32_t strides[4] = {buf->stride, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    
    ret = drmModeAddFB2(fd, buf->width, buf->height, FORMAT,
                       handles, strides, offsets, &buf->fb, 0);
    if (ret) {
        fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
        ret = -errno;
        goto err_destroy;
    }
    
    /* map buffer */
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = buf->handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }
    
    buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, mreq.offset);
    if (buf->map == MAP_FAILED) {
        fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }
    
    /* 根据通道索引填充颜色 */
    rdma_channel_config *config = &rdma_configs[channel_idx];
    
    debug_print(2, "Filling buffer for channel %d: %dx%d, stride=%u, color=(%u,%u,%u)\n",
               channel_idx, buf->width, buf->height, buf->stride,
               config->base_r, config->base_g, config->base_b);
    
    /* 填充颜色 */
    for (unsigned int j = 0; j < buf->height; ++j) {
        uint32_t *row = (uint32_t *)(buf->map + buf->stride * j);
        for (unsigned int k = 0; k < buf->width; ++k) {
            row[k] = (0xFF << 24) |          // Alpha通道完全不透明
                    (config->base_r << 16) | // Red通道
                    (config->base_g << 8) |  // Green通道
                    config->base_b;          // Blue通道
        }
    }
    
    fprintf(stderr, "Channel %d: Created framebuffer %u: %dx%d, stride=%u, color=(R=%u,G=%u,B=%u)\n",
            channel_idx, buf->fb, buf->width, buf->height, buf->stride,
            config->base_r, config->base_g, config->base_b);
    
    return 0;
    
err_fb:
    drmModeRmFB(fd, buf->fb);
err_destroy:
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = buf->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    return ret;
}

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
    struct drm_mode_destroy_dumb dreq;
    
    if (buf->map && buf->map != MAP_FAILED)
        munmap(buf->map, buf->size);
    
    if (buf->fb) {
        drmModeRmFB(fd, buf->fb);
    }
    
    if (buf->handle) {
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = buf->handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
}

static int modeset_setup_framebuffer(int fd, struct output *out)
{
    out->buf.width = channel_width;
    out->buf.height = channel_height;
    
    /* 传递通道索引以填充正确的初始颜色 */
    return modeset_create_fb(fd, &out->buf, out->channel_idx);
}

static void output_destroy(int fd, struct output *out)
{
    modeset_destroy_objects(fd, out);
    modeset_destroy_fb(fd, &out->buf);
    free(out);
}

static struct output *output_create(int fd, drmModeRes *res, 
                                    uint32_t crtc_id, int crtc_index,
                                    int channel_idx)
{
    struct output *out;
    int ret;
    uint32_t plane_id;
    
    out = malloc(sizeof(*out));
    if (!out)
        return NULL;
    
    memset(out, 0, sizeof(*out));
    out->crtc_id = crtc_id;
    out->channel_idx = channel_idx;
    
    /* 使用配置中指定的plane ID */
    plane_id = rdma_configs[channel_idx].plane_id;
    
    debug_print(1, "Creating channel %d with plane %u\n", channel_idx, plane_id);
    
    /* 检查plane是否可用 */
    ret = check_plane_availability(fd, plane_id, crtc_id, crtc_index);
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Plane %u is not available\n", channel_idx, plane_id);
        free(out);
        return NULL;
    }
    
    out->plane.id = plane_id;
    
    /* 获取plane属性 */
    ret = modeset_setup_objects(fd, out);
    if (ret) {
        fprintf(stderr, "Channel %d: cannot get plane properties\n", channel_idx);
        free(out);
        return NULL;
    }
    
    /* 创建framebuffer - 会自动填充初始颜色 */
    ret = modeset_setup_framebuffer(fd, out);
    if (ret) {
        fprintf(stderr, "Channel %d: cannot create framebuffer\n", channel_idx);
        modeset_destroy_objects(fd, out);
        free(out);
        return NULL;
    }
    
    /* 设置颜色变量 */
    out->r = rdma_configs[channel_idx].base_r;
    out->g = rdma_configs[channel_idx].base_g;
    out->b = rdma_configs[channel_idx].base_b;
    
    fprintf(stderr, "Channel %d: Created output with plane %u, CRTC %u, color=(R=%u,G=%u,B=%u)\n", 
            channel_idx, out->plane.id, out->crtc_id, out->r, out->g, out->b);
    
    return out;
}

/* 查找可用的CRTC */
static int find_available_crtc(int fd, drmModeRes *res, uint32_t *crtc_id, int *crtc_index)
{
    drmModeConnector *conn;
    drmModeEncoder *enc;
    int i;
    
    for (i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;
        
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            debug_print(2, "Found connected connector %u\n", conn->connector_id);
            
            if (conn->encoder_id) {
                enc = drmModeGetEncoder(fd, conn->encoder_id);
                if (enc && enc->crtc_id) {
                    *crtc_id = enc->crtc_id;
                    
                    /* 查找CRTC索引 */
                    for (int j = 0; j < res->count_crtcs; j++) {
                        if (res->crtcs[j] == *crtc_id) {
                            *crtc_index = j;
                            break;
                        }
                    }
                    
                    drmModeFreeEncoder(enc);
                    drmModeFreeConnector(conn);
                    
                    fprintf(stderr, "Found connected CRTC %u via connector %u\n",
                            *crtc_id, conn->connector_id);
                    return 0;
                }
                if (enc)
                    drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(conn);
    }
    
    /* 如果没有找到连接的connector，使用第一个可用的CRTC */
    if (res->count_crtcs > 0) {
        *crtc_id = res->crtcs[0];
        *crtc_index = 0;
        fprintf(stderr, "Using first available CRTC %u\n", *crtc_id);
        return 0;
    }
    
    return -ENOENT;
}

static int modeset_prepare(int fd, uint32_t crtc_id)
{
    drmModeRes *res;
    int crtc_index = 0;
    int i;
    
    res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n", errno);
        return -errno;
    }
    
    debug_print(2, "DRM resources:\n");
    debug_print(2, "  Connectors: %d\n", res->count_connectors);
    debug_print(2, "  Encoders: %d\n", res->count_encoders);
    debug_print(2, "  CRTCs: %d\n", res->count_crtcs);
    debug_print(2, "  FBs: %d\n", res->count_fbs);
    
    /* 如果没有指定CRTC，自动查找一个 */
    if (crtc_id == 0) {
        if (find_available_crtc(fd, res, &crtc_id, &crtc_index) < 0) {
            fprintf(stderr, "cannot find available CRTC\n");
            drmModeFreeResources(res);
            return -ENOENT;
        }
    } else {
        /* 查找指定CRTC的索引 */
        for (i = 0; i < res->count_crtcs; i++) {
            if (res->crtcs[i] == crtc_id) {
                crtc_index = i;
                break;
            }
        }
    }
    
    fprintf(stderr, "Using CRTC %u (index %d)\n", crtc_id, crtc_index);
    
    /* 创建4个输出，每个对应一个plane */
    for (i = 0; i < MAX_RDMA_CHANNELS; i++) {
        struct output *out = output_create(fd, res, crtc_id, crtc_index, i);
        if (!out) {
            fprintf(stderr, "Failed to create channel %d\n", i);
            continue;
        }
        
        out->next = output_list;
        output_list = out;
        
        fprintf(stderr, "Created channel %d:\n", i);
        fprintf(stderr, "  Plane: %u\n", out->plane.id);
        fprintf(stderr, "  Position: (%d, %d)\n", 
                rdma_configs[i].crtc_x, rdma_configs[i].crtc_y);
        fprintf(stderr, "  Size: %dx%d\n", 
                rdma_configs[i].crtc_w, rdma_configs[i].crtc_h);
        fprintf(stderr, "  RDMA ID: %u\n", rdma_configs[i].rdma_id);
        fprintf(stderr, "  Initial Color: R=%u, G=%u, B=%u\n", 
                out->r, out->g, out->b);
    }
    
    drmModeFreeResources(res);
    
    if (!output_list) {
        fprintf(stderr, "couldn't create any outputs\n");
        return -1;
    }
    
    fprintf(stderr, "\nTotal created %d channel(s)\n", MAX_RDMA_CHANNELS);
    
    return 0;
}

/* 设置atomic commit的属性 */
static int output_atomic_prepare_commit(int fd, struct output *out,
                                        drmModeAtomicReq *req)
{
    struct drm_object *plane = &out->plane;
    struct modeset_buf *buf = &out->buf;
    rdma_channel_config *config = &rdma_configs[out->channel_idx];
    
    debug_print(2, "Channel %d atomic prepare:\n", out->channel_idx);
    debug_print(2, "  Plane: %u\n", plane->id);
    debug_print(2, "  FB: %u (%dx%d, stride=%u)\n", 
               buf->fb, buf->width, buf->height, buf->stride);
    debug_print(2, "  Position: (%d, %d)\n", config->crtc_x, config->crtc_y);
    debug_print(2, "  Size: %dx%d\n", config->crtc_w, config->crtc_h);
    debug_print(2, "  RDMA ID: %u\n", config->rdma_id);
    
    fprintf(stderr, "Channel %d: Setting plane=%u, position=(%d,%d), size=%dx%d, RDMA_ID=%d\n",
            out->channel_idx, plane->id, config->crtc_x, config->crtc_y,
            config->crtc_w, config->crtc_h, config->rdma_id);
    
    int ret = 0;
    
    /* 设置plane属性 */
    ret = set_drm_object_property(req, plane, "FB_ID", buf->fb);
    debug_print(3, "  Set FB_ID=%u: %s\n", buf->fb, ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set FB_ID\n", out->channel_idx);
        return -1;
    }
    
    ret = set_drm_object_property(req, plane, "CRTC_ID", out->crtc_id);
    debug_print(3, "  Set CRTC_ID=%u: %s\n", out->crtc_id, ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set CRTC_ID\n", out->channel_idx);
        return -1;
    }
    
    /* 设置源矩形 (16.16定点数) */
    ret = set_drm_object_property(req, plane, "SRC_X", 0);
    debug_print(3, "  Set SRC_X=0: %s\n", ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set SRC_X\n", out->channel_idx);
        return -1;
    }
    
    ret = set_drm_object_property(req, plane, "SRC_Y", 0);
    debug_print(3, "  Set SRC_Y=0: %s\n", ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set SRC_Y\n", out->channel_idx);
        return -1;
    }
    
    uint64_t src_w = channel_width << 16;
    uint64_t src_h = channel_height << 16;
    ret = set_drm_object_property(req, plane, "SRC_W", src_w);
    debug_print(3, "  Set SRC_W=%lu: %s\n", src_w, ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set SRC_W\n", out->channel_idx);
        return -1;
    }
    
    ret = set_drm_object_property(req, plane, "SRC_H", src_h);
    debug_print(3, "  Set SRC_H=%lu: %s\n", src_h, ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set SRC_H\n", out->channel_idx);
        return -1;
    }
    
    /* 设置目标矩形 (屏幕位置和大小) */
    ret = set_drm_object_property(req, plane, "CRTC_X", config->crtc_x * 2);
    debug_print(3, "  Set CRTC_X=%d: %s\n", config->crtc_x, ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set CRTC_X\n", out->channel_idx);
        return -1;
    }
    
    ret = set_drm_object_property(req, plane, "CRTC_Y", config->crtc_y * 2);
    debug_print(3, "  Set CRTC_Y=%d: %s\n", config->crtc_y, ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set CRTC_Y\n", out->channel_idx);
        return -1;
    }
    
    ret = set_drm_object_property(req, plane, "CRTC_W", config->crtc_w * 2);
    debug_print(3, "  Set CRTC_W=%d: %s\n", config->crtc_w, ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set CRTC_W\n", out->channel_idx);
        return -1;
    }
    
    ret = set_drm_object_property(req, plane, "CRTC_H", config->crtc_h * 2);
    debug_print(3, "  Set CRTC_H=%d: %s\n", config->crtc_h, ret < 0 ? "FAILED" : "OK");
    if (ret < 0) {
        fprintf(stderr, "Channel %d: Failed to set CRTC_H\n", out->channel_idx);
        return -1;
    }
    
    /* 设置RDMA通道ID */
    ret = set_drm_object_property(req, plane, "RDMA_ID", config->rdma_id);
    if (ret < 0) {
        debug_print(2, "Channel %d: RDMA_ID property not found, continuing anyway\n", 
                   out->channel_idx);
        /* 有些驱动可能没有RDMA_ID属性，继续执行 */
    } else {
        debug_print(3, "  Set RDMA_ID=%u: OK\n", config->rdma_id);
    }
    
    return 0;
}

/* 执行单次modeset - 同时设置4个plane */
static int perform_modeset(int fd)
{
    drmModeAtomicReq *req;
    struct output *iter;
    int ret, flags;
    int planes_configured = 0;
    
    req = drmModeAtomicAlloc();
    
    fprintf(stderr, "\nSetting up all channels...\n");
    
    /* 为所有输出准备commit */
    for (iter = output_list; iter; iter = iter->next) {
        ret = output_atomic_prepare_commit(fd, iter, req);
        if (ret < 0) {
            fprintf(stderr, "Channel %d: prepare failed\n", iter->channel_idx);
            drmModeAtomicFree(req);
            return ret;
        }
        
        planes_configured++;
        fprintf(stderr, "Channel %d: Prepared commit with color (R=%u,G=%u,B=%u)\n",
                iter->channel_idx, iter->r, iter->g, iter->b);
    }
    
    if (planes_configured == 0) {
        fprintf(stderr, "No planes configured\n");
        drmModeAtomicFree(req);
        return -EINVAL;
    }
    
    /* 执行modeset - 一次提交所有plane */
    flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    
    fprintf(stderr, "Committing %d planes with atomic modeset...\n", planes_configured);
    
    ret = drmModeAtomicCommit(fd, req, flags, NULL);
    drmModeAtomicFree(req);
    
    if (ret < 0) {
        fprintf(stderr, "Atomic commit failed: %s\n", strerror(errno));
        return ret;
    }
    
    fprintf(stderr, "Atomic commit successful!\n");
    
    /* 正确打印plane和channel的对应关系 */
    fprintf(stderr, "\n%d planes are now active:\n", planes_configured);
    for (iter = output_list; iter; iter = iter->next) {
        const char *color_name = "";
        const char *position = "";
        
        switch (iter->channel_idx) {
            case 0:
                color_name = "RED";
                position = "Top-left";
                break;
            case 1:
                color_name = "GREEN";
                position = "Top-right";
                break;
            case 2:
                color_name = "BLUE";
                position = "Bottom-left";
                break;
            case 3:
                color_name = "YELLOW";
                position = "Bottom-right";
                break;
        }
        
        fprintf(stderr, "Plane %u (Channel %d): %s - %s (RDMA %u)\n", 
                iter->plane.id, iter->channel_idx, position, color_name,
                rdma_configs[iter->channel_idx].rdma_id);
    }
    
    return 0;
}

/* 主函数 - 单次显示 */
static int modeset_show(int fd, int run_seconds)
{
    int ret;
    
    /* 执行modeset */
    ret = perform_modeset(fd);
    if (ret < 0) {
        fprintf(stderr, "Modeset failed\n");
        return ret;
    }
    
    fprintf(stderr, "\nDisplaying for %d seconds...\n", run_seconds);
    fprintf(stderr, "Press Enter to exit early.\n");
    
    /* 简单等待 */
    if (run_seconds > 0) {
        time_t start = time(NULL);
        while (time(NULL) < start + run_seconds) {
            /* 检查是否有键盘输入 */
            fd_set fds;
            struct timeval tv = {0, 100000}; // 100ms
            
            FD_ZERO(&fds);
            FD_SET(0, &fds);
            
            if (select(1, &fds, NULL, NULL, &tv) > 0) {
                if (FD_ISSET(0, &fds)) {
                    fprintf(stderr, "Exit due to user input\n");
                    break;
                }
            }
        }
    }
    return 0;
}

/* 清理资源 */
static void modeset_cleanup(int fd)
{
    struct output *iter;
    
    fprintf(stderr, "\nCleaning up...\n");
    
    while (output_list) {
        iter = output_list;
        output_list = iter->next;
        output_destroy(fd, iter);
    }
}

int main(int argc, char **argv)
{
    int ret = 0, fd;
    const char *card = "/dev/dri/card1";
    uint32_t crtc_id = 0;
    int run_seconds = 5;
    int c;
    int verbose_count = 0;
    
    while ((c = getopt(argc, argv, "d:w:h:c:t:v")) != -1) {
        switch (c) {
        case 'd':
            card = optarg;
            break;
        case 'w':
            channel_width = atoi(optarg);
            if (channel_width <= 0 || channel_width > 1920) {
                fprintf(stderr, "Invalid width: %d\n", channel_width);
                return EXIT_FAILURE;
            }
            break;
        case 'h':
            channel_height = atoi(optarg);
            if (channel_height <= 0 || channel_height > 1080) {
                fprintf(stderr, "Invalid height: %d\n", channel_height);
                return EXIT_FAILURE;
            }
            break;
        case 'c':
            crtc_id = atoi(optarg);
            break;
        case 't':
            run_seconds = atoi(optarg);
            if (run_seconds < 0) {
                fprintf(stderr, "Invalid time: %d seconds\n", run_seconds);
                return EXIT_FAILURE;
            }
            break;
        case 'v':
            verbose_count++;
            DEBUG_LEVEL = verbose_count + 1; // -v: level 2, -vv: level 3
            break;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    
    fprintf(stderr, "=== DRM 4-Plane RDMA Test (Single Display) ===\n");
    fprintf(stderr, "Device: %s\n", card);
    fprintf(stderr, "Format: ARGB8888\n");
    fprintf(stderr, "Run time: %d seconds\n", run_seconds);
    if (DEBUG_LEVEL > 1) {
        fprintf(stderr, "Debug level: %d\n", DEBUG_LEVEL);
    }
    
    /* 只有在命令行指定了宽度或高度时，才更新配置中的位置和大小 */
    bool size_changed = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "-h") == 0) {
            size_changed = true;
            break;
        }
    }
    
    if (size_changed) {
        /* 更新配置中的位置和大小 */
        for (int i = 0; i < MAX_RDMA_CHANNELS; i++) {
            rdma_configs[i].crtc_w = channel_width;
            rdma_configs[i].crtc_h = channel_height;
            
            /* 重新计算位置，使4个通道平铺显示 */
            if (i == 0) {
                rdma_configs[i].crtc_x = 0;
                rdma_configs[i].crtc_y = 0;
            } else if (i == 1) {
                rdma_configs[i].crtc_x = channel_width;
                rdma_configs[i].crtc_y = 0;
            } else if (i == 2) {
                rdma_configs[i].crtc_x = 0;
                rdma_configs[i].crtc_y = channel_height;
            } else if (i == 3) {
                rdma_configs[i].crtc_x = channel_width;
                rdma_configs[i].crtc_y = channel_height;
            }
        }
        fprintf(stderr, "Channel size: %dx%d (manual)\n", channel_width, channel_height);
    } else {
        /* 使用配置文件中预设的大小 */
        channel_width = rdma_configs[0].crtc_w;
        channel_height = rdma_configs[0].crtc_h;
        fprintf(stderr, "Channel size: %dx%d (default)\n", channel_width, channel_height);
    }
    
    fprintf(stderr, "\nChannel layout:\n");
    for (int i = 0; i < MAX_RDMA_CHANNELS; i++) {
        fprintf(stderr, "  Channel %d: plane %u, position (%d, %d), size %dx%d, RDMA ID %d\n",
                i, rdma_configs[i].plane_id, 
                rdma_configs[i].crtc_x, rdma_configs[i].crtc_y,
                rdma_configs[i].crtc_w, rdma_configs[i].crtc_h,
                rdma_configs[i].rdma_id);
    }
    
    /* 打开DRM设备 */
    ret = modeset_open(&fd, card);
    if (ret)
        goto out_return;
    
    /* 准备所有输出 */
    ret = modeset_prepare(fd, crtc_id);
    if (ret)
        goto out_close;
    
    /* 执行单次显示 */
    ret = modeset_show(fd, run_seconds);
    
    /* 清理资源 */
    modeset_cleanup(fd);
    
out_close:
    close(fd);
out_return:
    if (ret) {
        fprintf(stderr, "\nTest failed with error %d\n", ret);
        return EXIT_FAILURE;
    } else {
        fprintf(stderr, "\n4 RDMA Channel Scaling Test completed successfully!\n");
        return EXIT_SUCCESS;
    }
}