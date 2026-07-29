#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <poll.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#define DRM_DEVICE "/dev/dri/card0"
#define WIDTH 800
#define HEIGHT 480
#define FORMAT DRM_FORMAT_XRGB8888

// Spacemit WB属性定义
#define SPACEMIT_WB_PROP_ROTATION "wb_rotation"
#define SPACEMIT_WB_PROP_IN_X "WB_IN_X"
#define SPACEMIT_WB_PROP_IN_Y "WB_IN_Y"
#define SPACEMIT_WB_PROP_IN_W "WB_IN_W"
#define SPACEMIT_WB_PROP_IN_H "WB_IN_H"
#define SPACEMIT_WB_PROP_OUT_X "WB_OUT_X"
#define SPACEMIT_WB_PROP_OUT_Y "WB_OUT_Y"
#define SPACEMIT_WB_PROP_OUT_W "WB_OUT_W"
#define SPACEMIT_WB_PROP_OUT_H "WB_OUT_H"

struct drm_context {
	int fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t encoder_id;

	// WB connector ID
	uint32_t wb_conn_id;
	drmModeConnector *wb_connector;

	// Primary plane ID
	uint32_t plane_id;

	// FB for display
	uint32_t display_fb_id;
	void *display_fb_data;
	struct drm_mode_create_dumb display_dumb;

	// FB for writeback
	uint32_t wb_fb_id;
	void *wb_fb_data;
	struct drm_mode_create_dumb wb_dumb;

	// WB connector properties
	uint32_t wb_rotation_prop_id;
	uint32_t wb_in_x_prop_id;
	uint32_t wb_in_y_prop_id;
	uint32_t wb_in_w_prop_id;
	uint32_t wb_in_h_prop_id;
	uint32_t wb_out_x_prop_id;
	uint32_t wb_out_y_prop_id;
	uint32_t wb_out_w_prop_id;
	uint32_t wb_out_h_prop_id;

	// CRTC properties for writeback
	uint32_t crtc_wb_pos_prop_id;
	uint32_t crtc_offline_mode_prop_id;

	// Display mode
	drmModeModeInfo mode;
	uint32_t mode_blob_id;

	uint32_t width;
	uint32_t height;
	uint32_t format;
};

// 初始化DRM设备
int init_drm(struct drm_context *ctx)
{
	int ret;

	ctx->fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC);
	if (ctx->fd < 0) {
		printf("Failed to open DRM device: %s\n", strerror(errno));
		return -1;
	}

	// 尝试成为DRM master
	ret = drmSetMaster(ctx->fd);
	if (ret) {
		printf("Warning: Failed to become DRM master: %s\n", strerror(errno));
		printf("  This may cause permission issues. Try running as root or closing other DRM clients.\n");
	} else {
		printf("✓ Became DRM master\n");
	}

	// 设置DRM client capabilities
	ret = drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		printf("Warning: Failed to set universal planes cap: %s\n", strerror(errno));
	}

	ret = drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		printf("Warning: Failed to set atomic cap: %s\n", strerror(errno));
	}

	ret = drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
	if (ret) {
		printf("Warning: Failed to set writeback connectors cap: %s\n", strerror(errno));
	} else {
		printf("✓ Writeback connectors capability enabled\n");
	}

	ctx->width = WIDTH;
	ctx->height = HEIGHT;
	ctx->format = FORMAT;

	return 0;
}

// 创建dumb buffer
int create_dumb_buffer(struct drm_context *ctx, struct drm_mode_create_dumb *dumb,
			uint32_t *fb_id, void **fb_data, uint32_t format)
{
	int ret;

	memset(dumb, 0, sizeof(*dumb));
	dumb->width = ctx->width;
	dumb->height = ctx->height;
	
	// 根据格式设置bpp
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
		dumb->bpp = 32;
		break;
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
		dumb->bpp = 16;
		break;
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_BGR888:
		dumb->bpp = 24;
		break;
	default:
		dumb->bpp = 32; // 默认32位
	}
	
	ret = drmIoctl(ctx->fd, DRM_IOCTL_MODE_CREATE_DUMB, dumb);
	if (ret < 0) {
		printf("Failed to create dumb buffer: %s\n", strerror(errno));
		return ret;
	}

	// 创建framebuffer - 修复参数类型
	const uint32_t handles[4] = {dumb->handle, 0, 0, 0};
	const uint32_t pitches[4] = {dumb->pitch, 0, 0, 0};
	const uint32_t offsets[4] = {0, 0, 0, 0};
	
	ret = drmModeAddFB2(ctx->fd, ctx->width, ctx->height, format,
			    handles, pitches, offsets, fb_id, 0);
	if (ret < 0) {
		printf("Failed to add FB: %s\n", strerror(errno));
		goto err_destroy_dumb;
	}

	// 映射buffer
	struct drm_mode_map_dumb map_dumb = {0};
	map_dumb.handle = dumb->handle;
	
	ret = drmIoctl(ctx->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
	if (ret < 0) {
		printf("Failed to map dumb buffer: %s\n", strerror(errno));
		goto err_remove_fb;
	}

	*fb_data = mmap(0, dumb->size, PROT_READ | PROT_WRITE,
			MAP_SHARED, ctx->fd, map_dumb.offset);
	if (*fb_data == MAP_FAILED) {
		printf("mmap failed: %s\n", strerror(errno));
		ret = -1;
		goto err_remove_fb;
	}

	return 0;

err_remove_fb:
	drmModeRmFB(ctx->fd, *fb_id);
err_destroy_dumb:
	memset(&map_dumb, 0, sizeof(map_dumb));
	map_dumb.handle = dumb->handle;
	drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &map_dumb);
	return ret;
}

// 获取对象属性ID的辅助函数
uint32_t get_property_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
	drmModeObjectPropertiesPtr props;
	uint32_t prop_id = 0;

	props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props) {
		return 0;
	}

	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
		if (prop) {
			if (strcmp(prop->name, name) == 0) {
				prop_id = prop->prop_id;
				drmModeFreeProperty(prop);
				break;
			}
			drmModeFreeProperty(prop);
		}
	}

	drmModeFreeObjectProperties(props);
	return prop_id;
}

// 查找写回连接器
int find_writeback_connector(struct drm_context *ctx)
{
	drmModeRes *resources;
	drmModeConnector *connector;
	int found = 0;

	resources = drmModeGetResources(ctx->fd);
	if (!resources) {
		printf("Failed to get DRM resources: %s\n", strerror(errno));
		return -1;
	}

	printf("Searching for writeback connectors...\n");
	printf("Total connectors in resources: %d\n\n", resources->count_connectors);

	// 方法1: 遍历resources中的connector
	for (int i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(ctx->fd, resources->connectors[i]);
		if (connector) {
			printf("=== Connector %d (from resources) ===\n", connector->connector_id);
			printf("  Type: %d", connector->connector_type);
			switch (connector->connector_type) {
				case DRM_MODE_CONNECTOR_DSI: printf(" (DSI)"); break;
				case DRM_MODE_CONNECTOR_WRITEBACK: printf(" (WRITEBACK)"); break;
				case DRM_MODE_CONNECTOR_HDMIA: printf(" (HDMI-A)"); break;
				case DRM_MODE_CONNECTOR_DisplayPort: printf(" (DisplayPort)"); break;
				default: printf(" (Other)"); break;
			}
			printf("\n");
			printf("  Type ID: %d\n", connector->connector_type_id);
			printf("  Connection: %d", connector->connection);
			switch (connector->connection) {
				case DRM_MODE_CONNECTED: printf(" (Connected)"); break;
				case DRM_MODE_DISCONNECTED: printf(" (Disconnected)"); break;
				case DRM_MODE_UNKNOWNCONNECTION: printf(" (Unknown)"); break;
			}
			printf("\n");

			// 保存display connector
			if (connector->connection == DRM_MODE_CONNECTED &&
			    connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK) {
				ctx->conn_id = connector->connector_id;
				printf("  >>> This is the display connector\n");
			}

			drmModeFreeConnector(connector);
			printf("\n");
		}
	}

	// 方法2: 尝试直接访问可能的writeback connector ID
	// 通常writeback connector的ID会比display connector大
	printf("Trying to find writeback connector by probing IDs...\n");
	for (uint32_t conn_id = 256; conn_id < 270; conn_id++) {
		connector = drmModeGetConnector(ctx->fd, conn_id);
		if (connector) {
			// 检查是否有WRITEBACK属性
			drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
				ctx->fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
			if (props) {
				int has_writeback_prop = 0;
				for (unsigned int j = 0; j < props->count_props; j++) {
					drmModePropertyPtr prop = drmModeGetProperty(ctx->fd, props->props[j]);
					if (prop) {
						if (strcmp(prop->name, "WRITEBACK_FB_ID") == 0 ||
						    strcmp(prop->name, "WRITEBACK_OUT_FENCE_PTR") == 0 ||
						    strcmp(prop->name, "WRITEBACK_PIXEL_FORMATS") == 0) {
							has_writeback_prop = 1;
						}
						drmModeFreeProperty(prop);
					}
				}

				if (has_writeback_prop || connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
					printf("\n=== Connector %d (writeback) ===\n", conn_id);
					printf("  Type: %d", connector->connector_type);
					if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
						printf(" (WRITEBACK)");
					}
					printf("\n");
					printf("  Type ID: %d\n", connector->connector_type_id);

					// 列出所有属性
					printf("  Properties (%u):\n", props->count_props);
					for (unsigned int j = 0; j < props->count_props; j++) {
						drmModePropertyPtr prop = drmModeGetProperty(ctx->fd, props->props[j]);
						if (prop) {
							printf("    [%u] %s (ID: %u, value: %lu)\n",
								j, prop->name, prop->prop_id, props->prop_values[j]);
							drmModeFreeProperty(prop);
						}
					}

					printf("  >>> This is the WRITEBACK connector! <<<\n\n");
					ctx->wb_conn_id = conn_id;
					ctx->wb_connector = connector;
					found = 1;
					drmModeFreeObjectProperties(props);
					break;
				}
				drmModeFreeObjectProperties(props);
			}

			if (!found) {
				drmModeFreeConnector(connector);
			}
		}
	}

	if (!found) {
		printf("ERROR: No writeback connector found!\n");
		printf("Tip: Check /sys/class/drm/ for Writeback connectors\n");
		drmModeFreeResources(resources);
		return -1;
	}

	printf("✓ Selected writeback connector: ID=%d\n", ctx->wb_conn_id);
	printf("✓ Selected display connector: ID=%d\n\n", ctx->conn_id);

	// 使用第一个CRTC
	if (resources->count_crtcs > 0) {
		ctx->crtc_id = resources->crtcs[0];
		printf("Using CRTC ID: %u\n", ctx->crtc_id);
	}

	// 获取display connector的模式
	drmModeConnector *display_conn = drmModeGetConnector(ctx->fd, ctx->conn_id);
	if (display_conn && display_conn->count_modes > 0) {
		// 使用第一个模式
		memcpy(&ctx->mode, &display_conn->modes[0], sizeof(drmModeModeInfo));
		printf("Using mode: %s (%dx%d@%.2fHz)\n",
		       ctx->mode.name, ctx->mode.hdisplay, ctx->mode.vdisplay,
		       ctx->mode.clock * 1000.0 / (ctx->mode.htotal * ctx->mode.vtotal));

		// 创建mode blob
		int ret = drmModeCreatePropertyBlob(ctx->fd, &ctx->mode,
		                                     sizeof(ctx->mode), &ctx->mode_blob_id);
		if (ret < 0) {
			printf("Failed to create mode blob: %s\n", strerror(errno));
			drmModeFreeConnector(display_conn);
			drmModeFreeResources(resources);
			return -1;
		}
		printf("✓ Created mode blob: ID=%u\n", ctx->mode_blob_id);
		drmModeFreeConnector(display_conn);
	} else {
		printf("Warning: No modes available on display connector\n");
	}

	drmModeFreeResources(resources);
	return 0;
}

// 获取写回连接器属性
int get_writeback_properties(struct drm_context *ctx)
{
	drmModeObjectPropertiesPtr props;
	drmModePropertyPtr prop;
	int found_count = 0;

	props = drmModeObjectGetProperties(ctx->fd, ctx->wb_conn_id, DRM_MODE_OBJECT_CONNECTOR);
	if (!props) {
		printf("Failed to get connector properties: %s\n", strerror(errno));
		return -1;
	}

	printf("Writeback connector properties:\n");
	for (int i = 0; i < props->count_props; i++) {
		prop = drmModeGetProperty(ctx->fd, props->props[i]);
		if (prop) {
			printf("  [%d] %s (ID: %u)\n", i, prop->name, prop->prop_id);
			
			// 保存重要的属性ID
			if (strcmp(prop->name, SPACEMIT_WB_PROP_ROTATION) == 0) {
				ctx->wb_rotation_prop_id = prop->prop_id;
				found_count++;
			} else if (strcmp(prop->name, SPACEMIT_WB_PROP_IN_X) == 0) {
				ctx->wb_in_x_prop_id = prop->prop_id;
				found_count++;
			} else if (strcmp(prop->name, SPACEMIT_WB_PROP_IN_Y) == 0) {
				ctx->wb_in_y_prop_id = prop->prop_id;
				found_count++;
			} else if (strcmp(prop->name, SPACEMIT_WB_PROP_IN_W) == 0) {
				ctx->wb_in_w_prop_id = prop->prop_id;
				found_count++;
			} else if (strcmp(prop->name, SPACEMIT_WB_PROP_IN_H) == 0) {
				ctx->wb_in_h_prop_id = prop->prop_id;
				found_count++;
			} else if (strcmp(prop->name, SPACEMIT_WB_PROP_OUT_X) == 0) {
				ctx->wb_out_x_prop_id = prop->prop_id;
				found_count++;
			} else if (strcmp(prop->name, SPACEMIT_WB_PROP_OUT_Y) == 0) {
				ctx->wb_out_y_prop_id = prop->prop_id;
				found_count++;
			} else if (strcmp(prop->name, SPACEMIT_WB_PROP_OUT_W) == 0) {
				ctx->wb_out_w_prop_id = prop->prop_id;
				found_count++;
			} else if (strcmp(prop->name, SPACEMIT_WB_PROP_OUT_H) == 0) {
				ctx->wb_out_h_prop_id = prop->prop_id;
				found_count++;
			}
			
			drmModeFreeProperty(prop);
		}
	}

	drmModeFreeObjectProperties(props);

	if (found_count < 9) {
		printf("Warning: Not all Spacemit WB properties found (found %d/9)\n", found_count);
	}

	return 0;
}

// 查找primary plane
int find_primary_plane(struct drm_context *ctx)
{
	drmModePlaneResPtr planes;
	drmModePlanePtr plane;
	drmModeObjectPropertiesPtr props;
	int found = 0;

	planes = drmModeGetPlaneResources(ctx->fd);
	if (!planes) {
		printf("Failed to get plane resources\n");
		return -1;
	}

	printf("\nSearching for primary plane for CRTC %u...\n", ctx->crtc_id);

	for (uint32_t i = 0; i < planes->count_planes; i++) {
		plane = drmModeGetPlane(ctx->fd, planes->planes[i]);
		if (!plane)
			continue;

		// 检查plane是否可以用于这个CRTC
		if (!(plane->possible_crtcs & (1 << 0))) {  // 假设CRTC index是0
			drmModeFreePlane(plane);
			continue;
		}

		// 检查是否是primary plane
		props = drmModeObjectGetProperties(ctx->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
		if (props) {
			for (uint32_t j = 0; j < props->count_props; j++) {
				drmModePropertyPtr prop = drmModeGetProperty(ctx->fd, props->props[j]);
				if (prop) {
					if (strcmp(prop->name, "type") == 0) {
						if (props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) {
							ctx->plane_id = plane->plane_id;
							printf("✓ Found primary plane: ID=%u\n", ctx->plane_id);
							found = 1;
						}
					}
					drmModeFreeProperty(prop);
				}
			}
			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
		if (found)
			break;
	}

	drmModeFreePlaneResources(planes);

	if (!found) {
		printf("Warning: Primary plane not found, using first available plane\n");
		// 如果找不到primary plane，使用第一个plane
		planes = drmModeGetPlaneResources(ctx->fd);
		if (planes && planes->count_planes > 0) {
			ctx->plane_id = planes->planes[0];
			printf("Using plane ID: %u\n", ctx->plane_id);
			found = 1;
		}
		if (planes)
			drmModeFreePlaneResources(planes);
	}

	return found ? 0 : -1;
}

// 获取CRTC的writeback相关属性
int get_crtc_wb_properties(struct drm_context *ctx)
{
	drmModeObjectPropertiesPtr props;
	drmModePropertyPtr prop;
	int found_count = 0;

	props = drmModeObjectGetProperties(ctx->fd, ctx->crtc_id, DRM_MODE_OBJECT_CRTC);
	if (!props) {
		printf("Failed to get CRTC properties: %s\n", strerror(errno));
		return -1;
	}

	printf("\nCRTC properties:\n");
	for (int i = 0; i < props->count_props; i++) {
		prop = drmModeGetProperty(ctx->fd, props->props[i]);
		if (prop) {
			printf("  [%d] %s (ID: %u, value: %lu)\n",
				i, prop->name, prop->prop_id, props->prop_values[i]);

			// 保存WB_POS和offline_mode属性ID
			if (strcmp(prop->name, "WB_POS") == 0) {
				ctx->crtc_wb_pos_prop_id = prop->prop_id;
				found_count++;
				printf("    ✓ Found WB_POS property\n");
			} else if (strcmp(prop->name, "offline_mode") == 0) {
				ctx->crtc_offline_mode_prop_id = prop->prop_id;
				found_count++;
				printf("    ✓ Found offline_mode property\n");
			}

			drmModeFreeProperty(prop);
		}
	}

	drmModeFreeObjectProperties(props);

	if (found_count < 1) {
		printf("Warning: WB_POS property not found\n");
	}

	// offline_mode可能不存在，不强制要求
	if (!ctx->crtc_offline_mode_prop_id) {
		printf("Note: offline_mode property not found (may not be required)\n");
	}

	return 0;
}

// 设置写回属性
int set_writeback_properties(struct drm_context *ctx)
{
	int ret;
	
	printf("Setting writeback properties...\n");
	
	// 设置输入区域（完整帧）
	if (ctx->wb_in_w_prop_id) {
		ret = drmModeObjectSetProperty(ctx->fd, ctx->wb_conn_id,
						DRM_MODE_OBJECT_CONNECTOR,
						ctx->wb_in_w_prop_id, ctx->width);
		if (ret < 0) {
			printf("Failed to set WB_IN_W property: %s\n", strerror(errno));
		}
	}
	
	if (ctx->wb_in_h_prop_id) {
		ret = drmModeObjectSetProperty(ctx->fd, ctx->wb_conn_id,
						DRM_MODE_OBJECT_CONNECTOR,
						ctx->wb_in_h_prop_id, ctx->height);
		if (ret < 0) {
			printf("Failed to set WB_IN_H property: %s\n", strerror(errno));
		}
	}
	
	// 设置输出区域（完整帧）
	if (ctx->wb_out_w_prop_id) {
		ret = drmModeObjectSetProperty(ctx->fd, ctx->wb_conn_id,
						DRM_MODE_OBJECT_CONNECTOR,
						ctx->wb_out_w_prop_id, ctx->width);
		if (ret < 0) {
			printf("Failed to set WB_OUT_W property: %s\n", strerror(errno));
		}
	}
	
	if (ctx->wb_out_h_prop_id) {
		ret = drmModeObjectSetProperty(ctx->fd, ctx->wb_conn_id,
						DRM_MODE_OBJECT_CONNECTOR,
						ctx->wb_out_h_prop_id, ctx->height);
		if (ret < 0) {
			printf("Failed to set WB_OUT_H property: %s\n", strerror(errno));
		}
	}
	
	// 设置旋转（无旋转）
	if (ctx->wb_rotation_prop_id) {
		ret = drmModeObjectSetProperty(ctx->fd, ctx->wb_conn_id,
						DRM_MODE_OBJECT_CONNECTOR,
						ctx->wb_rotation_prop_id, 0); // 0度旋转
		if (ret < 0) {
			printf("Failed to set rotation property: %s\n", strerror(errno));
		}
	}
	
	return 0;
}

// 生成测试图案
void generate_test_pattern(struct drm_context *ctx)
{
	uint32_t *pixels = (uint32_t*)ctx->display_fb_data;
	uint32_t pitch = ctx->display_dumb.pitch / 4;

	printf("Generating test pattern (%dx%d)...\n", ctx->width, ctx->height);

	for (int y = 0; y < ctx->height; y++) {
		for (int x = 0; x < ctx->width; x++) {
			// 生成渐变颜色
			uint8_t r = (x * 255) / ctx->width;
			uint8_t g = (y * 255) / ctx->height;
			uint8_t b = 128 + (x + y) * 127 / (ctx->width + ctx->height);
			
			// 添加一些特征图案
			if ((x / 32 + y / 32) % 2 == 0) {
				// 棋盘格图案
				r = 255 - r;
			}
			
			// 中心十字
			if (abs(x - ctx->width/2) < 2 || abs(y - ctx->height/2) < 2) {
				r = 255; g = 255; b = 255;
			}
			
			// 四个角标记
			if ((x < 20 && y < 20) || 
			    (x < 20 && y >= ctx->height - 20) ||
			    (x >= ctx->width - 20 && y < 20) ||
			    (x >= ctx->width - 20 && y >= ctx->height - 20)) {
				r = 255; g = 0; b = 0;
			}

			// XRGB8888格式: 0xFFRRGGBB
			pixels[y * pitch + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
		}
	}

	printf("Test pattern generation complete\n");
}

// 比较两个framebuffer的内容
int compare_framebuffers(struct drm_context *ctx)
{
	uint32_t *src_pixels = (uint32_t*)ctx->display_fb_data;
	uint32_t *dst_pixels = (uint32_t*)ctx->wb_fb_data;
	uint32_t src_pitch = ctx->display_dumb.pitch / 4;
	uint32_t dst_pitch = ctx->wb_dumb.pitch / 4;
	int errors = 0;
	int identical_pixels = 0;
	
	printf("Comparing framebuffer contents...\n");
	
	// 清空统计
	int black_pixels = 0;
	int white_pixels = 0;
	
	for (int y = 0; y < ctx->height; y++) {
		for (int x = 0; x < ctx->width; x++) {
			uint32_t src_pixel = src_pixels[y * src_pitch + x];
			uint32_t dst_pixel = dst_pixels[y * dst_pitch + x];
			
			// 忽略alpha通道比较
			if ((src_pixel & 0x00FFFFFF) != (dst_pixel & 0x00FFFFFF)) {
				errors++;
				if (errors <= 20) { // 只打印前20个错误
					uint8_t src_r = (src_pixel >> 16) & 0xFF;
					uint8_t src_g = (src_pixel >> 8) & 0xFF;
					uint8_t src_b = src_pixel & 0xFF;
					uint8_t dst_r = (dst_pixel >> 16) & 0xFF;
					uint8_t dst_g = (dst_pixel >> 8) & 0xFF;
					uint8_t dst_b = dst_pixel & 0xFF;
					
					printf("Mismatch @(%4d,%4d): src=(%3d,%3d,%3d), dst=(%3d,%3d,%3d)\n",
						x, y, src_r, src_g, src_b, dst_r, dst_g, dst_b);
				}
			} else {
				identical_pixels++;
			}
			
			// 统计像素值
			if (dst_pixel == 0xFF000000) black_pixels++;
			if (dst_pixel == 0xFFFFFFFF) white_pixels++;
		}
	}

	// 打印统计信息
	printf("\nComparison Statistics:\n");
	printf("  Total pixels: %d\n", ctx->width * ctx->height);
	printf("  Identical pixels: %d\n", identical_pixels);
	printf("  Mismatched pixels: %d\n", errors);
	printf("  Black pixels (0xFF000000) in WB: %d\n", black_pixels);
	printf("  White pixels (0xFFFFFFFF) in WB: %d\n", white_pixels);
	
	if (errors == 0) {
		printf("  ✓ All pixels match!\n");
	} else {
		printf("  ✗ Error rate: %.2f%%\n", 
			(float)errors * 100 / (ctx->width * ctx->height));
	}
	
	return errors;
}

// 先设置显示，不带writeback
int setup_display_only(struct drm_context *ctx)
{
	int ret;
	drmModeAtomicReqPtr req;

	printf("Setting up display without writeback...\n");

	req = drmModeAtomicAlloc();
	if (!req) {
		printf("Failed to allocate atomic request\n");
		return -1;
	}

	// 1. 设置primary plane
	ret = drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "FB_ID"),
					ctx->display_fb_id);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_ID"),
					ctx->crtc_id);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "SRC_X"), 0);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "SRC_Y"), 0);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "SRC_W"),
					ctx->width << 16);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "SRC_H"),
					ctx->height << 16);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_X"), 0);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_Y"), 0);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_W"),
					ctx->width);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_H"),
					ctx->height);
	if (ret < 0) {
		printf("Failed to add plane properties: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added plane properties\n");

	// 2. 设置CRTC MODE_ID
	if (ctx->mode_blob_id) {
		ret = drmModeAtomicAddProperty(req, ctx->crtc_id,
						get_property_id(ctx->fd, ctx->crtc_id,
								DRM_MODE_OBJECT_CRTC, "MODE_ID"),
						ctx->mode_blob_id);
		if (ret < 0) {
			printf("Failed to add CRTC MODE_ID: %s\n", strerror(errno));
			goto err_free_req;
		}
		printf("✓ Added CRTC MODE_ID = %u\n", ctx->mode_blob_id);
	}

	// 3. 设置CRTC ACTIVE
	ret = drmModeAtomicAddProperty(req, ctx->crtc_id,
					get_property_id(ctx->fd, ctx->crtc_id,
							DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
	if (ret < 0) {
		printf("Failed to add CRTC ACTIVE: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added CRTC ACTIVE = 1\n");

	// 4. 设置display connector CRTC_ID
	ret = drmModeAtomicAddProperty(req, ctx->conn_id,
					get_property_id(ctx->fd, ctx->conn_id,
							DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"),
					ctx->crtc_id);
	if (ret < 0) {
		printf("Failed to add connector CRTC_ID: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added display connector CRTC_ID = %u\n", ctx->crtc_id);

	// 提交
	printf("Committing display setup...\n");
	ret = drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret < 0) {
		printf("Failed to commit display setup: %s (errno=%d)\n",
		       strerror(errno), errno);
		goto err_free_req;
	}

	printf("✓ Display setup committed\n");
	drmModeAtomicFree(req);
	return 0;

err_free_req:
	drmModeAtomicFree(req);
	return -1;
}

// 使用原子操作设置写回
int set_writeback_atomic(struct drm_context *ctx)
{
	drmModeAtomicReq *req;
	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	int ret;
	
	req = drmModeAtomicAlloc();
	if (!req) {
		printf("Failed to allocate atomic request\n");
		return -1;
	}
	
	// 设置写回framebuffer
	drmModeAtomicAddProperty(req, ctx->wb_conn_id,
				ctx->wb_in_w_prop_id, ctx->width);
	drmModeAtomicAddProperty(req, ctx->wb_conn_id,
				ctx->wb_in_h_prop_id, ctx->height);
	drmModeAtomicAddProperty(req, ctx->wb_conn_id,
				ctx->wb_out_w_prop_id, ctx->width);
	drmModeAtomicAddProperty(req, ctx->wb_conn_id,
				ctx->wb_out_h_prop_id, ctx->height);
	
	// 提交原子操作
	ret = drmModeAtomicCommit(ctx->fd, req, flags, NULL);
	if (ret < 0) {
		printf("Atomic commit failed: %s\n", strerror(errno));
		drmModeAtomicFree(req);
		return -1;
	}
	
	drmModeAtomicFree(req);
	return 0;
}

// 执行写回操作
int perform_writeback(struct drm_context *ctx, int wb_pos, int offline_mode)
{
	int ret;
	drmModeAtomicReqPtr req;
	int writeback_fence_fd = -1;

	printf("\n=== Performing Writeback Test ===\n");

	// 创建一个atomic request，包含所有必需的属性
	req = drmModeAtomicAlloc();
	if (!req) {
		printf("Failed to allocate atomic request\n");
		return -1;
	}

	printf("\n--- Step 1: Adding all properties to atomic request ---\n");

	// 1. 设置primary plane的framebuffer和位置
	ret = drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "FB_ID"),
					ctx->display_fb_id);
	if (ret < 0) {
		printf("Failed to add plane FB_ID: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added plane FB_ID = %u\n", ctx->display_fb_id);

	ret = drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_ID"),
					ctx->crtc_id);
	if (ret < 0) {
		printf("Failed to add plane CRTC_ID: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added plane CRTC_ID = %u\n", ctx->crtc_id);

	// 设置plane的源区域 (16.16 fixed point)
	ret = drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "SRC_X"), 0);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "SRC_Y"), 0);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "SRC_W"),
					ctx->width << 16);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "SRC_H"),
					ctx->height << 16);
	if (ret < 0) {
		printf("Failed to add plane SRC properties: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added plane SRC properties\n");

	// 设置plane的目标区域
	ret = drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_X"), 0);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_Y"), 0);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_W"),
					ctx->width);
	ret |= drmModeAtomicAddProperty(req, ctx->plane_id,
					get_property_id(ctx->fd, ctx->plane_id,
							DRM_MODE_OBJECT_PLANE, "CRTC_H"),
					ctx->height);
	if (ret < 0) {
		printf("Failed to add plane CRTC properties: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added plane CRTC properties\n");

	// 2. 设置CRTC的MODE_ID (必需，否则显示不会激活)
	if (ctx->mode_blob_id) {
		ret = drmModeAtomicAddProperty(req, ctx->crtc_id,
						get_property_id(ctx->fd, ctx->crtc_id,
								DRM_MODE_OBJECT_CRTC, "MODE_ID"),
						ctx->mode_blob_id);
		if (ret < 0) {
			printf("Failed to add CRTC MODE_ID: %s\n", strerror(errno));
			goto err_free_req;
		}
		printf("✓ Added CRTC MODE_ID = %u\n", ctx->mode_blob_id);
	}

	// 3. 设置CRTC为active
	ret = drmModeAtomicAddProperty(req, ctx->crtc_id,
					get_property_id(ctx->fd, ctx->crtc_id,
							DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
	if (ret < 0) {
		printf("Failed to add CRTC ACTIVE: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added CRTC ACTIVE = 1\n");

	// 4. 设置display connector的CRTC_ID
	ret = drmModeAtomicAddProperty(req, ctx->conn_id,
					get_property_id(ctx->fd, ctx->conn_id,
							DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"),
					ctx->crtc_id);
	if (ret < 0) {
		printf("Failed to add connector CRTC_ID: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added display connector CRTC_ID = %u\n", ctx->crtc_id);

	// 5. 设置CRTC的writeback属性
	if (ctx->crtc_wb_pos_prop_id) {
		ret = drmModeAtomicAddProperty(req, ctx->crtc_id,
						ctx->crtc_wb_pos_prop_id, wb_pos);
		if (ret < 0) {
			printf("Failed to add WB_POS property: %s\n", strerror(errno));
			goto err_free_req;
		}
		printf("✓ Added WB_POS = %d\n", wb_pos);
	}

	if (ctx->crtc_offline_mode_prop_id) {
		ret = drmModeAtomicAddProperty(req, ctx->crtc_id,
						ctx->crtc_offline_mode_prop_id, offline_mode);
		if (ret < 0) {
			printf("Failed to add offline_mode property: %s\n", strerror(errno));
			goto err_free_req;
		}
		printf("✓ Added offline_mode = %d\n", offline_mode);
	}

	// 6. 设置WB connector的输入/输出区域属性
	if (ctx->wb_in_w_prop_id) {
		ret = drmModeAtomicAddProperty(req, ctx->wb_conn_id,
						ctx->wb_in_w_prop_id, ctx->width);
		if (ret < 0) {
			printf("Failed to add WB_IN_W property: %s\n", strerror(errno));
			goto err_free_req;
		}
		printf("✓ Added WB_IN_W = %u\n", ctx->width);
	}

	if (ctx->wb_in_h_prop_id) {
		ret = drmModeAtomicAddProperty(req, ctx->wb_conn_id,
						ctx->wb_in_h_prop_id, ctx->height);
		if (ret < 0) {
			printf("Failed to add WB_IN_H property: %s\n", strerror(errno));
			goto err_free_req;
		}
		printf("✓ Added WB_IN_H = %u\n", ctx->height);
	}

	if (ctx->wb_out_w_prop_id) {
		ret = drmModeAtomicAddProperty(req, ctx->wb_conn_id,
						ctx->wb_out_w_prop_id, ctx->width);
		if (ret < 0) {
			printf("Failed to add WB_OUT_W property: %s\n", strerror(errno));
			goto err_free_req;
		}
		printf("✓ Added WB_OUT_W = %u\n", ctx->width);
	}

	if (ctx->wb_out_h_prop_id) {
		ret = drmModeAtomicAddProperty(req, ctx->wb_conn_id,
						ctx->wb_out_h_prop_id, ctx->height);
		if (ret < 0) {
			printf("Failed to add WB_OUT_H property: %s\n", strerror(errno));
			goto err_free_req;
		}
		printf("✓ Added WB_OUT_H = %u\n", ctx->height);
	}

	// 7. 设置WB connector的CRTC_ID - 将WB connector连接到CRTC
	ret = drmModeAtomicAddProperty(req, ctx->wb_conn_id,
					get_property_id(ctx->fd, ctx->wb_conn_id,
							DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"),
					ctx->crtc_id);
	if (ret < 0) {
		printf("Failed to add WB CRTC_ID property: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added WB connector CRTC_ID = %u\n", ctx->crtc_id);

	// 8. 设置WRITEBACK_FB_ID - 写回目标framebuffer
	ret = drmModeAtomicAddProperty(req, ctx->wb_conn_id,
					get_property_id(ctx->fd, ctx->wb_conn_id,
							DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_FB_ID"),
					ctx->wb_fb_id);
	if (ret < 0) {
		printf("Failed to add WRITEBACK_FB_ID property: %s\n", strerror(errno));
		goto err_free_req;
	}
	printf("✓ Added WRITEBACK_FB_ID = %u (writeback destination)\n", ctx->wb_fb_id);

	// 9. 设置WRITEBACK_OUT_FENCE_PTR - 用于同步
	ret = drmModeAtomicAddProperty(req, ctx->wb_conn_id,
					get_property_id(ctx->fd, ctx->wb_conn_id,
							DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_OUT_FENCE_PTR"),
					(uint64_t)(uintptr_t)&writeback_fence_fd);
	if (ret < 0) {
		printf("Warning: Failed to add WRITEBACK_OUT_FENCE_PTR: %s\n", strerror(errno));
		// 不是致命错误，继续
	} else {
		printf("✓ Added WRITEBACK_OUT_FENCE_PTR\n");
	}

	// 提交atomic request - 一次性提交所有属性
	printf("\n--- Step 2: Committing atomic request ---\n");
	ret = drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret < 0) {
		printf("Failed to commit writeback request: %s (errno=%d)\n",
		       strerror(errno), errno);
		goto err_free_req;
	}

	printf("✓ Writeback committed successfully\n");

	// 等待写回完成
	if (writeback_fence_fd >= 0) {
		printf("\n--- Step 3: Waiting for writeback fence ---\n");
		struct pollfd pfd = {
			.fd = writeback_fence_fd,
			.events = POLLIN
		};
		ret = poll(&pfd, 1, 1000);
		if (ret > 0) {
			printf("✓ Writeback completed (fence signaled)\n");
		} else if (ret == 0) {
			printf("Warning: Fence poll timeout\n");
		} else {
			printf("Warning: Fence poll error: %s\n", strerror(errno));
		}
		close(writeback_fence_fd);
	} else {
		printf("\n--- Step 3: Waiting for writeback to complete ---\n");
		usleep(500000); // 500ms
		printf("✓ Wait completed\n");
	}

	drmModeAtomicFree(req);
	return 0;

err_free_req:
	drmModeAtomicFree(req);
	return -1;
}

// 保存framebuffer到文件（用于调试）
void save_framebuffer_to_file(struct drm_context *ctx, void *data, const char *filename)
{
	FILE *fp = fopen(filename, "wb");
	if (!fp) {
		printf("Failed to open %s for writing\n", filename);
		return;
	}
	
	uint32_t *pixels = (uint32_t*)data;
	uint32_t pitch = ctx->display_dumb.pitch / 4;
	
	// 简单的PPM格式
	fprintf(fp, "P3\n%d %d\n255\n", ctx->width, ctx->height);
	
	for (int y = 0; y < ctx->height; y++) {
		for (int x = 0; x < ctx->width; x++) {
			uint32_t pixel = pixels[y * pitch + x];
			uint8_t r = (pixel >> 16) & 0xFF;
			uint8_t g = (pixel >> 8) & 0xFF;
			uint8_t b = pixel & 0xFF;
			fprintf(fp, "%d %d %d ", r, g, b);
		}
		fprintf(fp, "\n");
	}
	
	fclose(fp);
	printf("Saved framebuffer to %s\n", filename);
}

// 清理资源
void cleanup(struct drm_context *ctx)
{
	printf("\nCleaning up resources...\n");
	
	if (ctx->display_fb_data && ctx->display_fb_data != MAP_FAILED) {
		munmap(ctx->display_fb_data, ctx->display_dumb.size);
	}
	if (ctx->wb_fb_data && ctx->wb_fb_data != MAP_FAILED) {
		munmap(ctx->wb_fb_data, ctx->wb_dumb.size);
	}

	if (ctx->display_fb_id) {
		drmModeRmFB(ctx->fd, ctx->display_fb_id);
	}
	if (ctx->wb_fb_id) {
		drmModeRmFB(ctx->fd, ctx->wb_fb_id);
	}

	struct drm_mode_destroy_dumb destroy_dumb = {0};

	if (ctx->display_dumb.handle) {
		destroy_dumb.handle = ctx->display_dumb.handle;
		drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
	}

	if (ctx->wb_dumb.handle) {
		destroy_dumb.handle = ctx->wb_dumb.handle;
		drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
	}
	
	if (ctx->wb_connector) {
		drmModeFreeConnector(ctx->wb_connector);
	}

	if (ctx->fd >= 0) {
		close(ctx->fd);
	}
	
	printf("Cleanup complete\n");
}

int main(int argc, char *argv[])
{
	struct drm_context ctx = {0};
	int ret;

	// 解析命令行参数
	int save_files = 0;
	int wb_pos = 21; // 默认值21，对应modetest命令中的WB_POS:21
	int offline_mode = 0; // 默认在线模式

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--save") == 0) {
			save_files = 1;
		} else if (strncmp(argv[i], "--wb-pos=", 9) == 0) {
			wb_pos = atoi(argv[i] + 9);
		} else if (strncmp(argv[i], "--offline-mode=", 15) == 0) {
			offline_mode = atoi(argv[i] + 15);
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("Usage: %s [options]\n", argv[0]);
			printf("Options:\n");
			printf("  --save              Save framebuffers to PPM files\n");
			printf("  --wb-pos=<value>    Set WB_POS property (default: 21)\n");
			printf("  --offline-mode=<0|1> Set offline_mode (default: 0)\n");
			printf("  --help, -h          Show this help\n");
			printf("\nWB_POS values (from spacemit_wb.h):\n");
			printf("  0-23: RDMA/PREPQ stages\n");
			printf("  24-26: COMP0-COMP2 (composition)\n");
			printf("  27-29: POST0-POST2 (post-processing)\n");
			return 0;
		}
	}

	printf("========================================\n");
	printf("Spacemit DRM Writeback Consistency Test\n");
	printf("========================================\n");
	printf("Configuration:\n");
	printf("  WB_POS: %d\n", wb_pos);
	printf("  offline_mode: %d\n", offline_mode);
	printf("  Save files: %s\n", save_files ? "yes" : "no");
	printf("========================================\n");

	// 初始化DRM
	ret = init_drm(&ctx);
	if (ret < 0) {
		return -1;
	}
	printf("✓ DRM device opened successfully\n");

	// 查找写回连接器
	ret = find_writeback_connector(&ctx);
	if (ret < 0) {
		cleanup(&ctx);
		return -1;
	}
	printf("✓ Writeback connector found: ID=%u\n", ctx.wb_conn_id);
	
	// 获取写回属性
	ret = get_writeback_properties(&ctx);
	if (ret < 0) {
		printf("Warning: Failed to get writeback properties\n");
	}
	printf("✓ Writeback properties retrieved\n");

	// 获取CRTC的writeback相关属性
	ret = get_crtc_wb_properties(&ctx);
	if (ret < 0) {
		printf("Warning: Failed to get CRTC writeback properties\n");
	}
	printf("✓ CRTC writeback properties retrieved\n");

	// 查找primary plane
	ret = find_primary_plane(&ctx);
	if (ret < 0) {
		printf("Error: Failed to find primary plane\n");
		cleanup(&ctx);
		return -1;
	}
	printf("✓ Primary plane found: ID=%u\n", ctx.plane_id);

	// 创建显示framebuffer
	printf("\nCreating display framebuffer...\n");
	ret = create_dumb_buffer(&ctx, &ctx.display_dumb, &ctx.display_fb_id, 
				&ctx.display_fb_data, ctx.format);
	if (ret < 0) {
		cleanup(&ctx);
		return -1;
	}
	printf("✓ Display framebuffer created: ID=%u\n", ctx.display_fb_id);

	// 创建写回framebuffer
	printf("\nCreating writeback framebuffer...\n");
	ret = create_dumb_buffer(&ctx, &ctx.wb_dumb, &ctx.wb_fb_id, 
				&ctx.wb_fb_data, ctx.format);
	if (ret < 0) {
		cleanup(&ctx);
		return -1;
	}
	printf("✓ Writeback framebuffer created: ID=%u\n", ctx.wb_fb_id);

	// 生成测试图案
	printf("\nGenerating test pattern...\n");
	generate_test_pattern(&ctx);

	// 可选：保存原始图案
	if (save_files) {
		save_framebuffer_to_file(&ctx, ctx.display_fb_data, "display_pattern.ppm");
	}

	// 先设置显示，不带 writeback
	printf("\n=== Step 1: Setting up display first ===\n");
	ret = setup_display_only(&ctx);
	if (ret < 0) {
		printf("✗ Display setup failed\n");
		cleanup(&ctx);
		return -1;
	}
	printf("✓ Display setup completed\n");
	printf(">>> Check if the screen is showing the test pattern now <<<\n");
	printf("Waiting 3 seconds...\n");
	sleep(3);

	// 执行写回
	ret = perform_writeback(&ctx, wb_pos, offline_mode);
	if (ret < 0) {
		printf("✗ Writeback execution failed\n");
		cleanup(&ctx);
		return -1;
	}
	printf("✓ Writeback operation completed\n");

	// 可选：保存写回结果
	if (save_files) {
		save_framebuffer_to_file(&ctx, ctx.wb_fb_data, "writeback_result.ppm");
	}

	// 比较两个framebuffer的内容
	printf("\n=== Verification Results ===\n");
	int errors = compare_framebuffers(&ctx);
	
	printf("\n========================================\n");
	if (errors == 0) {
		printf("✓ TEST PASSED: Display and writeback buffers are identical!\n");
	} else {
		printf("✗ TEST FAILED: Found %d pixel mismatches\n", errors);
		
		// 提供调试建议
		printf("\nDebugging suggestions:\n");
		printf("1. Check if writeback connector is properly configured\n");
		printf("2. Verify that the driver supports the selected format\n");
		printf("3. Try with --save option to save framebuffers for inspection\n");
		printf("4. Check kernel logs for DRM/WB related errors\n");
	}
	printf("========================================\n");

	cleanup(&ctx);
	return (errors == 0) ? 0 : 1;
}
