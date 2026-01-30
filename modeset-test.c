/*
 * DRM Double-Buffered VSync'ed Atomic Modesetting Howto
 * This example extends modeset-vsync.c, introducing planes and the
 * atomic API.
 *
 * Planes can be used to blend or overlay images on top of a CRTC
 * framebuffer during the scanout process. Not all hardware provide
 * planes and the number of planes available is also limited. If there's
 * not enough planes available or the hardware does not provide them,
 * users should fallback to composition via GPU or CPU to blend or
 * overlay the planes. Notice that this render process will result
 * in delay, what justifies the usage of planes by modern hardware
 * that needs to be fast.
 *
 * There are three types of planes: primary, cursor and overlay. For
 * compatibility with legacy userspace, the default behavior is to expose
 * only overlay planes to userspace (we're going to see in the code that
 * we have to ask to receive all types of planes). A good example of plane
 * usage is this: imagine a static desktop screen and the user is moving
 * the cursor around. Only the cursor is moving. Instead of calculating the
 * complete scene for each time the user moves its cursor, we can update only
 * the cursor plane and it will be automatically overlayed by the hardware on
 * top of the primary plane. There's no need of software composition in this
 * case.
 *
 * But there was synchronisation problems related to multiple planes
 * usage. The KMS API was not atomic, so you'd have to update the primary
 * plane and then the overlay planes with distinct IOCTL's. This could lead
 * to tearing and also some trouble related to blocking, so the atomic
 * API was proposed to fix these problems.
 *
 * With the introduction of the KMS atomic API, all the planes could get
 * updated in a single IOCTL, using drmModeAtomicCommit(). This can be
 * either asynchronous or fully blocking.
 *
 * This example assumes that you are familiar with modeset-vsync. Only
 * the differences between both files are highlighted here.
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

/*
 * A new struct is introduced: drm_object. It stores properties of certain
 * objects (connectors, CRTC and planes) that are used in atomic modeset setup
 * and also in atomic page-flips (all planes updated in a single IOCTL).
 */
struct format_info {
    uint32_t drm_format;
    const char *format_name;
    uint32_t format_id;
    uint32_t bpp;
};
#define DATA_DIR "/usr/share/drm"
static int width = 1920, height = 1080;
static int CRTC_W = 1920, SRC_W = 1920, CRTC_H = 1080, SRC_H = 1080;
static int rdma_id = 1;
static char DATA[256] = {0};
static bool photo = false;
static bool set_gamma = false;
static float scaling_factor = 1.0;
static int p_id = -1;

static const struct format_info format_table[] = {
    { DRM_FORMAT_ABGR2101010, "ABGR2101010", 1, 32 },
    { DRM_FORMAT_ARGB2101010, "ARGB2101010", 0, 32 },
    { DRM_FORMAT_RGBA1010102, "RGBA1010102", 2, 32 },
    { DRM_FORMAT_BGRA1010102, "BGRA1010102", 3, 32 },
    { DRM_FORMAT_ARGB8888,    "ARGB8888",    4, 32 },
    { DRM_FORMAT_ABGR8888,    "ABGR8888",    5, 32 },
    { DRM_FORMAT_RGBA8888,    "RGBA8888",    6, 32 },
    { DRM_FORMAT_BGRA8888,    "BGRA8888",    7, 32 },
    { DRM_FORMAT_XRGB8888,    "XRGB8888",    8, 32 },
    { DRM_FORMAT_XBGR8888,    "XBGR8888",    9, 32 },
    { DRM_FORMAT_RGBX8888,    "RGBX8888",   10, 32 },
    { DRM_FORMAT_BGRX8888,    "BGRX8888",   11, 32 },
    { DRM_FORMAT_RGB888,      "RGB888",     12, 24 },
    { DRM_FORMAT_BGR888,      "BGR888",     13, 24 },
    { DRM_FORMAT_RGBA5551,    "RGBA5551",   14, 16 },
    { DRM_FORMAT_BGRA5551,    "BGRA5551",   15, 16 },
    { DRM_FORMAT_ABGR1555,    "ABGR1555",   16, 16 },
    { DRM_FORMAT_ARGB1555,    "ARGB1555",   17, 16 },
    { DRM_FORMAT_RGBX5551,    "RGBX5551",   18, 16 },
    { DRM_FORMAT_BGRX5551,    "BGRX5551",   19, 16 },
    { DRM_FORMAT_XBGR1555,    "XBGR1555",   20, 16 },
    { DRM_FORMAT_XRGB1555,    "XRGB1555",   21, 16 },
    { DRM_FORMAT_RGB565,      "RGB565",     22, 16 },
    { DRM_FORMAT_BGR565,      "BGR565",     23, 16 },
    { DRM_FORMAT_ARGB16161616F, "ARGB16161616F", 24, 64 },
    { DRM_FORMAT_ABGR16161616F, "ABGR16161616F", 25, 64 },
    { DRM_FORMAT_YUYV,        "YUYV",       34, 16 },
    { DRM_FORMAT_YVYU,        "YVYU",       34, 16 },
    { DRM_FORMAT_UYVY,        "UYVY",       35, 16 },
    { DRM_FORMAT_VYUY,        "VYUY",       35, 16 },
    { DRM_FORMAT_YUV420,      "YUV420",     37, 12 },
    { DRM_FORMAT_NV12,        "NV12",       37, 12 },
    { DRM_FORMAT_NV21,        "NV21",       37, 12 },
    { DRM_FORMAT_YVU420,      "YVU420",     38, 12 },
    { DRM_FORMAT_YUV420,      "YUV420_P3",  38, 12 },
    { DRM_FORMAT_YVU420,      "YVU420_P3",  38, 12 },
    { DRM_FORMAT_P010,        "P010",       40, 24 },
    { DRM_FORMAT_XYUV8888,    "XYUV8888",   32, 32 },
    { DRM_FORMAT_Y410,        "Y410",       33, 32 },
};


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

struct modeset_output {
	struct modeset_output *next;

	unsigned int front_buf;
	struct modeset_buf bufs[2];

	struct drm_object connector;
	struct drm_object crtc;
	struct drm_object plane;

	drmModeModeInfo mode;
	uint32_t mode_blob_id;
	uint32_t crtc_index;

	bool pflip_pending;
	bool cleanup;
	uint32_t format;
	uint8_t r, g, b;
	bool r_up, g_up, b_up;
};
static struct modeset_output *output_list = NULL;

void print_usage(const char *program_name)
{
    fprintf(stderr, "Usage: %s [options]\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d <device>     Specify the video device\n");
    fprintf(stderr, "  -f <format>     Set the video format (e.g., ARGB8888, NV12, etc.)\n");
    fprintf(stderr, "  -w <width>      Set the image width\n");
    fprintf(stderr, "  -h <height>     Set the image height\n");
    fprintf(stderr, "  -c <width>      Set CRTC width\n");
    fprintf(stderr, "  -C <height>     Set CRTC height\n");
    fprintf(stderr, "  -s <width>      Set source width\n");
    fprintf(stderr, "  -S <height>     Set source height\n");
    fprintf(stderr, "  -t photo        Enable photo mode\n");
    fprintf(stderr, "  -r <id>         Set RDMA id\n");
    fprintf(stderr, "  -p <id>         Set plane id\n");
//     fprintf(stderr, "  -g              Set gamma correction\n");
}

static int parse_format(const char *fmt_str)
{
	for (size_t i = 0; i < sizeof(format_table) / sizeof(format_table[0]); i++) {
		if (strcasecmp(fmt_str, format_table[i].format_name) == 0) {
			return format_table[i].drm_format;
		}
	}

	return -1;
}

/*
 * modeset_open() changes just a little bit. We now have to set that we're going
 * to use the KMS atomic API and check if the device is capable of handling it.
 */

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

	/* Set that we want to receive all the types of planes in the list. This
	 * have to be done since, for legacy reasons, the default behavior is to
	 * expose only the overlay planes to the users. The atomic API only
	 * works if this is set.
	 */
	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		fprintf(stderr, "failed to set universal planes cap, %d\n", ret);
		return ret;
	}

	/* Here we set that we're going to use the KMS atomic API. It's supposed
	 * to set the DRM_CLIENT_CAP_UNIVERSAL_PLANES automatically, but it's a
	 * safe behavior to set it explicitly as we did in the previous
	 * commands. This is also good for learning purposes.
	 */
	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		fprintf(stderr, "failed to set atomic cap, %d", ret);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	if (drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) < 0 || !cap) {
		fprintf(stderr, "drm device '%s' does not support atomic KMS\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out = fd;
	return 0;
}

/*
 * get_property_value() is a new function. Given a device, the properties of
 * an object and a name, search for the value of property 'name'. If we can't
 * find it, return -1.
 */

static int64_t get_property_value(int fd, drmModeObjectPropertiesPtr props,
				  const char *name)
{
	drmModePropertyPtr prop;
	uint64_t value;
	bool found;
	int j;

	found = false;
	for (j = 0; j < props->count_props && !found; j++) {
		prop = drmModeGetProperty(fd, props->props[j]);
		if (!strcmp(prop->name, name)) {
			value = props->prop_values[j];
			found = true;
		}
		drmModeFreeProperty(prop);
	}

	if (!found)
		return -1;
	return value;
}

/*
 * get_drm_object_properties() is a new helpfer function that retrieves
 * the properties of a certain CRTC, plane or connector object.
 */

static void modeset_get_object_properties(int fd, struct drm_object *obj,
					  uint32_t type)
{
	const char *type_str;
	unsigned int i;

	obj->props = drmModeObjectGetProperties(fd, obj->id, type);
	if (!obj->props) {
		switch(type) {
			case DRM_MODE_OBJECT_CONNECTOR:
				type_str = "connector";
				break;
			case DRM_MODE_OBJECT_PLANE:
				type_str = "plane";
				break;
			case DRM_MODE_OBJECT_CRTC:
				type_str = "CRTC";
				break;
			default:
				type_str = "unknown type";
				break;
		}
		fprintf(stderr, "cannot get %s %d properties: %s\n",
			type_str, obj->id, strerror(errno));
		return;
	}

	obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
	for (i = 0; i < obj->props->count_props; i++)
		obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
}

/*
 * set_drm_object_property() is a new function. It sets a property value to a
 * CRTC, plane or connector object.
 */

static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj,
				   const char *name, uint64_t value)
{
	int i;
	uint32_t prop_id = 0;

	for (i = 0; i < obj->props->count_props; i++) {
		if (!strcmp(obj->props_info[i]->name, name)) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id == 0) {
		fprintf(stderr, "no object property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

/*
 * modeset_find_crtc() changes a little bit. Now we also have to save the CRTC
 * index, and not only its id.
 */

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_output *out)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	uint32_t crtc;
	struct modeset_output *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			for (iter = output_list; iter; iter = iter->next) {
				if (iter->crtc.id == crtc) {
					crtc = 0;
					break;
				}
			}

			if (crtc > 0) {
				drmModeFreeEncoder(enc);
				out->crtc.id = crtc;
				/* find the CRTC's index */
				for (i = 0; i < res->count_crtcs; ++i) {
					if (res->crtcs[i] == crtc) {
						out->crtc_index = i;
						break;
					}
				}
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC.
	 */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other output already uses this CRTC */
			crtc = res->crtcs[j];
			for (iter = output_list; iter; iter = iter->next) {
				if (iter->crtc.id == crtc) {
					crtc = 0;
					break;
				}
			}

			/* We have found a CRTC, so save it and return. Note
			 * that we have to save its index as well. The CRTC
			 * index (not its ID) will be used when searching for a
			 * suitable plane.
			 */
			if (crtc > 0) {
				fprintf(stdout, "crtc %u found for encoder %u, will need full modeset\n",
					crtc, conn->encoders[i]);;
				drmModeFreeEncoder(enc);
				out->crtc.id = crtc;
				out->crtc_index = j;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable crtc for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

/*
 * modeset_find_plane() is a new function. Given a certain combination
 * of connector+CRTC, it looks for a primary plane for it.
 */

static int modeset_find_plane(int fd, struct modeset_output *out)
{
	drmModePlaneResPtr plane_res;
	bool found_primary = false;
	int i, ret = -EINVAL;

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
				strerror(errno));
		return -ENOENT;
	}

	/* iterates through all planes of a certain device */
	for (i = 0; (i < plane_res->count_planes) && !found_primary; i++) {
		int plane_id = plane_res->planes[i];

		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
		if (!plane) {
			fprintf(stderr, "drmModeGetPlane(%u) failed: %s\n", plane_id,
					strerror(errno));
			continue;
		}

		/* check if the plane can be used by our CRTC */
		if (plane->possible_crtcs & (1 << out->crtc_index)) {
			drmModeObjectPropertiesPtr props =
				drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

			/* Get the "type" property to check if this is a primary
			 * plane. Type property is special, as its enum value is
			 * defined in UAPI headers. For the properties that are
			 * not defined in the UAPI headers, we would have to
			 * give kernel the property name and it would return the
			 * corresponding enum value. We could also do this for
			 * the "type" property, but it would make this simple
			 * example more complex. The reason why defining enum
			 * values for kernel properties in UAPI headers is
			 * deprecated is that string names are easier to both
			 * (userspace and kernel) make unique and keep
			 * consistent between drivers and kernel versions. But
			 * in order to not break userspace, some properties were
			 * left in the UAPI headers as well.
			 */
			if(p_id != -1 && plane_id == p_id){
				found_primary = true;
				out->plane.id = plane_id;
				fprintf(stderr, "Use Plane_id(%d) \n", plane_id);
				ret = 0;
			}
			else if (get_property_value(fd, props, "type") == DRM_PLANE_TYPE_PRIMARY && p_id == -1) {
				found_primary = true;
				out->plane.id = plane_id;
				fprintf(stderr, "Use DRM_PLANE_TYPE_PRIMARY Plane_id(%d) \n", plane_id);
				ret = 0;
			}

			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);

	if (found_primary)
		fprintf(stdout, "found primary plane, id: %d\n", out->plane.id);
	else
		fprintf(stdout, "couldn't find a primary plane\n");
	return ret;
}

/*
 * modeset_drm_object_fini() is a new helper function that destroys CRTCs,
 * connectors and planes
 */

static void modeset_drm_object_fini(struct drm_object *obj)
{
	for (int i = 0; i < obj->props->count_props; i++)
		drmModeFreeProperty(obj->props_info[i]);
	free(obj->props_info);
	drmModeFreeObjectProperties(obj->props);
}

/*
 * modeset_setup_objects() is a new function. It helps us to retrieve
 * connector, CRTC and plane objects properties from the device. These
 * properties will help us during the atomic modesetting commit, so we save
 * them in our struct modeset_output object.
 */

static int modeset_setup_objects(int fd, struct modeset_output *out)
{
	struct drm_object *connector = &out->connector;
	struct drm_object *crtc = &out->crtc;
	struct drm_object *plane = &out->plane;

	/* retrieve connector properties from the device */
	modeset_get_object_properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
	if (!connector->props)
		goto out_conn;

	/* retrieve CRTC properties from the device */
	modeset_get_object_properties(fd, crtc, DRM_MODE_OBJECT_CRTC);
	if (!crtc->props)
		goto out_crtc;

	/* retrieve plane properties from the device */
	modeset_get_object_properties(fd, plane, DRM_MODE_OBJECT_PLANE);
	if (!plane->props)
		goto out_plane;

	return 0;

out_plane:
	modeset_drm_object_fini(crtc);
out_crtc:
	modeset_drm_object_fini(connector);
out_conn:
	return -ENOMEM;
}

/*
 * modeset_destroy_objects() is a new function. It destroys what we allocate
 * in modeset_setup_objects().
 */

static void modeset_destroy_objects(int fd, struct modeset_output *out)
{
	modeset_drm_object_fini(&out->connector);
	modeset_drm_object_fini(&out->crtc);
	modeset_drm_object_fini(&out->plane);
}

/*
 * modeset_create_fb() stays the same.
 */

static int modeset_create_fb(int fd, struct modeset_buf *buf, uint32_t requested_format)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	const struct format_info *fmti = NULL;
	int ret;
	// uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

	#if 0
	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create dumb buffer (%d): %m\n",
			errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	handles[0] = buf->handle;
	pitches[0] = buf->stride;
	ret = drmModeAddFB2(fd, buf->width, buf->height, DRM_FORMAT_XRGB8888,
			    handles, pitches, offsets, &buf->fb, 0);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_destroy;
	}
	#else
	for (size_t i = 0; i < sizeof(format_table) / sizeof(format_table[0]); ++i) {
		if (format_table[i].drm_format == requested_format) {
			fmti = &format_table[i];
			break;
		}
	}

	if (!fmti) {
		fprintf(stderr, "Unsupported format: 0x%08x\n", requested_format);
		return -EINVAL;
	}

	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = fmti->bpp;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create dumb buffer (%d): %m\n",
			errno);
		return -errno;
	}
	uint32_t handles[4] = {0};
	uint32_t strides[4] = {0};
	uint32_t offsets[4] = {0};
	// buf->stride = creq.pitch;
	buf->stride = buf->width;
	buf->size = creq.size;
	buf->handle = creq.handle;
	fprintf(stderr, "buf->size = creq.size (%llu)\n", (unsigned long long)creq.size);

	switch (fmti->drm_format) {
		case DRM_FORMAT_ARGB8888:
		case DRM_FORMAT_ABGR8888:
		case DRM_FORMAT_RGBA8888:
		case DRM_FORMAT_BGRA8888:
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_XBGR8888:
		case DRM_FORMAT_RGBX8888:
		case DRM_FORMAT_BGRX8888:
		case DRM_FORMAT_ARGB2101010:
		case DRM_FORMAT_ABGR2101010:
		case DRM_FORMAT_RGBA1010102:
		case DRM_FORMAT_BGRA1010102:
		case DRM_FORMAT_RGB888:
		case DRM_FORMAT_BGR888:
		case DRM_FORMAT_RGB565:
		case DRM_FORMAT_BGR565:
		case DRM_FORMAT_RGBA5551:
		case DRM_FORMAT_BGRA5551:
		case DRM_FORMAT_ABGR1555:
		case DRM_FORMAT_ARGB1555:
		case DRM_FORMAT_RGBX5551:
		case DRM_FORMAT_BGRX5551:
		case DRM_FORMAT_XBGR1555:
		case DRM_FORMAT_XRGB1555:
		case DRM_FORMAT_ARGB16161616F:
		case DRM_FORMAT_ABGR16161616F:
		case DRM_FORMAT_YUYV:
		case DRM_FORMAT_YVYU:
		case DRM_FORMAT_UYVY:
		case DRM_FORMAT_VYUY:
		case DRM_FORMAT_XYUV8888:
		case DRM_FORMAT_Y410:
			buf->stride = creq.pitch;
			handles[0] = buf->handle;
			strides[0] = buf->stride;
			break;
		case DRM_FORMAT_YUV420:
		case DRM_FORMAT_YUV420_8BIT:
			offsets[0] = 0;
			handles[0] = buf->handle;
			strides[0] = buf->stride;
			strides[1] = strides[0]/2;
			offsets[1] = strides[0] * buf->height;
			handles[1] = buf->handle;
			strides[2] = strides[1];
			offsets[2] = offsets[1] + strides[1] * buf->height / 2;
			handles[2] = buf->handle;
			break;

		case DRM_FORMAT_YVU420:
			offsets[0] = 0;
			handles[0] = buf->handle; /* Y */
			strides[0] = buf->stride;
			/* plane 1: V, plane 2: U */
			strides[1] = strides[0]/2;
			offsets[1] = strides[0] * buf->height;
			handles[1] = buf->handle; /* V */
			strides[2] = strides[1];
			offsets[2] = offsets[1] + strides[1] * buf->height / 2;
			handles[2] = buf->handle; /* U */
			break;

		case DRM_FORMAT_NV12:
			handles[0] = buf->handle;
			handles[1] = buf->handle;
			strides[0] = buf->stride;
			strides[1] = buf->stride;
			offsets[0] = 0;
			offsets[1] = buf->width * buf->height;
			break;
		case DRM_FORMAT_NV21:
			handles[0] = buf->handle;
			handles[1] = buf->handle;
			strides[0] = buf->stride;
			strides[1] = buf->stride;
			offsets[0] = 0;
			offsets[1] = buf->width * buf->height; /* interleaved VU */
			break;

		case DRM_FORMAT_P010:
			handles[0] = buf->handle;
			handles[1] = buf->handle;
			strides[0] = buf->stride * 2;
			strides[1] = buf->stride * 2;
			offsets[0] = 0;
			offsets[1] = buf->width * 2 * buf->height;
			break;

		default:
			fprintf(stderr, "Unsupported YUV format: 0x%08x\n", fmti->drm_format);
			ret = -EINVAL;
			goto err_destroy;
	}

	if (drmModeAddFB2(fd, buf->width, buf->height, fmti->drm_format,
						handles, strides, offsets, &buf->fb, 0)) {
		perror("Failed to create FB");
		return -1;
	}

	#endif

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* perform actual memory mapping */
	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	if(!photo){
		memset(buf->map, 0, buf->size);
	}else{
		memset(buf->map, 0, buf->size);
		FILE *fp = fopen(DATA, "rb");

		if (!fp) {
			perror("fopen");
			ret = -errno;
			goto err_fb;
		}

		size_t bytes_read = fread(buf->map, 1, buf->size, fp);
		if (bytes_read != buf->size) {
			fprintf(stderr, "Failed to read full frame (expected %zu, got %zu)\n",
					buf->size, bytes_read);
			fclose(fp);
			ret = -EIO;
			goto err_fb;
		}

		fclose(fp);
	}

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

/*
 * modeset_destroy_fb() stays the same.
 */

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	/* unmap buffer */
	munmap(buf->map, buf->size);

	/* delete framebuffer */
	drmModeRmFB(fd, buf->fb);

	/* delete dumb buffer */
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

/*
 * modeset_setup_framebuffers() creates framebuffers for the back and front
 * buffers of a certain output. Also, it copies the connector mode to these
 * buffers.
 */

static int modeset_setup_framebuffers(int fd, drmModeConnector *conn,
				      struct modeset_output *out, int index)
{
	int i, ret;

	/* setup the front and back framebuffers */
	for (i = 0; i < 2; i++) {

		/* copy mode info to buffer */
		out->bufs[i].width = conn->modes[index].hdisplay;
		out->bufs[i].height = conn->modes[index].vdisplay;

		/* create a framebuffer for the buffer */
		ret = modeset_create_fb(fd, &out->bufs[i], out->format);
		if (ret) {
			/* the second framebuffer creation failed, so
			 * we have to destroy the first before returning */
			if (i == 1)
				modeset_destroy_fb(fd, &out->bufs[0]);
			return ret;
		}
	}

	return 0;
}

/*
 * modeset_output_destroy() is new. It destroys the objects (connector, crtc and
 * plane), front and back buffers, the mode blob property and then destroys the
 * output itself.
 */

static void modeset_output_destroy(int fd, struct modeset_output *out)
{
	/* destroy connector, crtc and plane objects */
	modeset_destroy_objects(fd, out);

	/* destroy front/back framebuffers */
	modeset_destroy_fb(fd, &out->bufs[0]);
	modeset_destroy_fb(fd, &out->bufs[1]);

	/* destroy mode blob property */
	drmModeDestroyPropertyBlob(fd, out->mode_blob_id);

	free(out);
}

/*
 * With a certain combination of connector+CRTC, we look for a suitable primary
 * plane for it. After that, we retrieve connector, CRTC and plane objects
 * properties from the device. These objects are used during the atomic modeset
 * setup (see modeset_atomic_prepare_commit()) and also during the page-flips
 * (see modeset_draw_out() and modeset_atomic_commit()).
 *
 * Besides that, we have to create a blob property that receives the output
 * mode. When we perform an atomic commit, the driver expects a CRTC property
 * named "MODE_ID", which points to the id of a blob. This usually happens for
 * properties that are not simple types. In this particular case, out->mode is a
 * struct. But we could have another property that expects the id of a blob that
 * holds an array, for instance.
 */

static struct modeset_output *modeset_output_create(int fd, drmModeRes *res,
						    drmModeConnector *conn, uint32_t requested_format,
						    int req_width, int req_height)
{
	int ret;
	struct modeset_output *out;

	/* creates an output structure */
	out = malloc(sizeof(*out));
	memset(out, 0, sizeof(*out));
	out->connector.id = conn->connector_id;
	out->format = requested_format;
	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		goto out_error;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		goto out_error;
	}

	/* copy the mode information into our output structure */
	bool found = false;
	int index;
	for (int i = 0; i < conn->count_modes; i++) {
		if (conn->modes[i].hdisplay == req_width &&
			conn->modes[i].vdisplay == req_height) {
			memcpy(&out->mode, &conn->modes[i], sizeof(out->mode));
			found = true;
			index = i;
			break;
		}
	}

	if (!found) {
		fprintf(stderr, "Requested mode %dx%d not found for connector %u\n",
				req_width, req_height, conn->connector_id);
		goto out_error;
	}
	/* create the blob property using out->mode and save its id in the output*/
	if (drmModeCreatePropertyBlob(fd, &out->mode, sizeof(out->mode),
	                              &out->mode_blob_id) != 0) {
		fprintf(stderr, "couldn't create a blob property\n");
		goto out_error;
	}
	fprintf(stderr, "mode for connector %u is %ux%u\n",
	        conn->connector_id, req_width, req_height);

	/* find a crtc for this connector */
	ret = modeset_find_crtc(fd, res, conn, out);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		goto out_blob;
	}

	/* with a connector and crtc, find a primary plane */
	ret = modeset_find_plane(fd, out);
	if (ret) {
		fprintf(stderr, "no valid plane for crtc %u\n", out->crtc.id);
		goto out_blob;
	}

	/* gather properties of our connector, CRTC and planes */
	ret = modeset_setup_objects(fd, out);
	if (ret) {
		fprintf(stderr, "cannot get plane properties\n");
		goto out_blob;
	}

	/* setup front/back framebuffers for this CRTC */
	ret = modeset_setup_framebuffers(fd, conn, out, index);
	if (ret) {
		fprintf(stderr, "cannot create framebuffers for connector %u\n",
			conn->connector_id);
		goto out_obj;
	}

	return out;

out_obj:
	modeset_destroy_objects(fd, out);
out_blob:
	drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
out_error:
	free(out);
	return NULL;
}

/*
 * modeset_prepare() changes a little bit. Now we use the new function
 * modeset_output_create() to allocate memory and setup the output.
 */

static int modeset_prepare(int fd, uint32_t requested_format)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_output *out;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return -errno;
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		/* create an output structure and free connector data */
		out = modeset_output_create(fd, res, conn, requested_format, width, height);
		drmModeFreeConnector(conn);
		if (!out)
			continue;

		/* link output into global list */
		out->next = output_list;
		output_list = out;
	}
	if (!output_list) {
		fprintf(stderr, "couldn't create any outputs\n");
		return -1;
	}

	/* free resources again */
	drmModeFreeResources(res);
	return 0;
}

uint16_t calculate_gamma_value(int index, int table_size, float gamma)
{
	if (index <= 0) return 0;
	if (index >= table_size - 1) return 4095;

	// 简单的平方根近似 (对应 gamma=2.0)
	// 对于 gamma=2.2，这是一个近似解
	uint32_t value = (index * 4095) / 255;

	// 简单的 gamma 校正近似
	if (value < 128) {
		value = (value * value) / 128;
	} else {
		value = 4095 - ((4095 - value) * (4095 - value)) / 128;
	}

	return (uint16_t)value;
}

/*
 * modeset_atomic_prepare_commit() is new. Here we set the values of properties
 * (of our connector, CRTC and plane objects) that we want to change in the
 * atomic commit. These changes are temporarily stored in drmModeAtomicReq *req
 * until the commit actually happens.
 */

static int modeset_atomic_prepare_commit(int fd, struct modeset_output *out,
					 drmModeAtomicReq *req)
{
	struct drm_object *plane = &out->plane;
	struct drm_object *crtc = &out->crtc;
	struct modeset_buf *buf = &out->bufs[out->front_buf ^ 1];
	uint16_t gamma_table[256];

	uint32_t blob_id;

	// if(set_gamma){
	// 	for (int i = 0; i < 256; i++)
	// 		gamma_table[i] = calculate_gamma_value(i, 256, 2.2f);

	// 	if (set_drm_object_property(req, &out->crtc, "pp_gamma", gamma_table) < 0)
	// 		return -1;
	// }

	/* set id of the CRTC id that the connector is using */
	if (set_drm_object_property(req, &out->connector, "CRTC_ID", out->crtc.id) < 0)
		return -1;

	/* set the mode id of the CRTC; this property receives the id of a blob
	 * property that holds the struct that actually contains the mode info */
	if (set_drm_object_property(req, &out->crtc, "MODE_ID", out->mode_blob_id) < 0)
		return -1;

	/* set the CRTC object as active */
	if (set_drm_object_property(req, &out->crtc, "ACTIVE", 1) < 0)
		return -1;

	/* set properties of the plane related to the CRTC and the framebuffer */
	if (set_drm_object_property(req, plane, "FB_ID", buf->fb) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_ID", out->crtc.id) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_X", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_Y", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_W", SRC_W << 16) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_H", SRC_H << 16) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_X", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_Y", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_W", CRTC_W) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_H", CRTC_H) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "RDMA_ID", rdma_id) < 0)
		return -1;

	return 0;
}

/*
 * A short helper function to compute a changing color value. No need to
 * understand it.
 */

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod)
{
	uint8_t next;

	next = cur + (*up ? 1 : -1) * (rand() % mod);
	if ((*up && next < cur) || (!*up && next > cur)) {
		*up = !*up;
		next = cur;
	}

	return next;
}

/*
 * Draw on back framebuffer before the page-flip is requested.
 */

static void modeset_paint_framebuffer(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;

	/* draw on back framebuffer */
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);
	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			off = buf->stride * j + k * 4;
			*(uint32_t*)&buf->map[off] =
				     (out->r << 16) | (out->g << 8) | out->b;
		}
	}

}

static void modeset_paint_framebuffer_red(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;

	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			off = buf->stride * j + k * 4;
			*(uint32_t*)&buf->map[off] = 0x00FF0000;  // 纯红色
		}
	}
}

static void modeset_paint_framebuffer_green(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;

	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			off = buf->stride * j + k * 4;
			*(uint32_t*)&buf->map[off] = 0x0000FF00;  // 绿色
		}
	}
}

static void modeset_paint_framebuffer_blue(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;

	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			off = buf->stride * j + k * 4;
			*(uint32_t*)&buf->map[off] = 0xFF0000FF;  // 蓝色
		}
	}
}


static void modeset_paint_framebuffer_xyuv8888(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;

	/* 动态变色效果 */
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	/* 使用后备 framebuffer */
	buf = &out->bufs[out->front_buf ^ 1];

	/* 预计算 YUV */
	uint8_t R = out->r;
	uint8_t G = out->g;
	uint8_t B = out->b;

	uint8_t Y = (uint8_t)(0.299f * R + 0.587f * G + 0.114f * B);
	uint8_t U = (uint8_t)(-0.169f * R - 0.331f * G + 0.500f * B + 128);
	uint8_t V = (uint8_t)( 0.500f * R - 0.419f * G - 0.081f * B + 128);

	/* 每像素写入 XYUV8888：X=0 */
	uint32_t pixel = (0x00 << 24) | (Y << 16) | (U << 8) | V;

	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			off = buf->stride * j + k * 4;
			*(uint32_t *)&buf->map[off] = pixel;
		}
	}
}

static void modeset_paint_framebuffer565(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;
	uint16_t color;

	/* draw on back framebuffer */
	out->r = next_color(&out->r_up, out->r, 5) & 0x1F;
	out->g = next_color(&out->g_up, out->g, 6) & 0x3F;
	out->b = next_color(&out->b_up, out->b, 5) & 0x1F;
	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			off = buf->stride * j + k * 2;  // 2 bytes per pixel (RGB565)
			*(uint16_t*)&buf->map[off] = 
				     (out->r << 11) | (out->g << 5) | out->b;
		}
	}
}

static void modeset_paint_framebuffer_1555_like(struct modeset_output *out, uint32_t fourcc)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;
	uint16_t r5, g5, b5, a1;
	uint16_t pixel;

	out->r = next_color(&out->r_up, out->r, 5) & 0x1F;
	out->g = next_color(&out->g_up, out->g, 5) & 0x1F;
	out->b = next_color(&out->b_up, out->b, 5) & 0x1F;

	r5 = out->r;
	g5 = out->g;
	b5 = out->b;
	a1 = 1; /* 对于 X*** 变体会置 0 */

	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
		switch (fourcc) {
			case DRM_FORMAT_ARGB1555: /* A RRRRR GGGGG BBBBB */
			pixel = (a1 << 15) | (r5 << 10) | (g5 << 5) | b5;
			break;
			case DRM_FORMAT_ABGR1555: /* A BBBBB GGGGG RRRRR */
			pixel = (a1 << 15) | (b5 << 10) | (g5 << 5) | r5;
			break;
			case DRM_FORMAT_XRGB1555: /* X RRRRR GGGGG BBBBB */
			pixel = (0 << 15) | (r5 << 10) | (g5 << 5) | b5;
			break;
			case DRM_FORMAT_XBGR1555: /* X BBBBB GGGGG RRRRR */
			pixel = (0 << 15) | (b5 << 10) | (g5 << 5) | r5;
			break;
			case DRM_FORMAT_RGBA5551: /* RRRRR GGGGG BBBBB A */
			pixel = (r5 << 11) | (g5 << 6) | (b5 << 1) | a1;
			break;
			case DRM_FORMAT_BGRA5551: /* BBBBB GGGGG RRRRR A */
			pixel = (b5 << 11) | (g5 << 6) | (r5 << 1) | a1;
			break;
			case DRM_FORMAT_RGBX5551: /* RRRRR GGGGG BBBBB X */
			pixel = (r5 << 11) | (g5 << 6) | (b5 << 1);
			break;
			case DRM_FORMAT_BGRX5551: /* BBBBB GGGGG RRRRR X */
			pixel = (b5 << 11) | (g5 << 6) | (r5 << 1);
			break;
			default:
			pixel = 0;
			break;
		}
		off = buf->stride * j + k * 2;
		*(uint16_t*)&buf->map[off] = pixel;
		}
	}
}

static void modeset_paint_nv21(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k;
	uint8_t *y_plane, *vu_plane;

	buf = &out->bufs[out->front_buf ^ 1];

	y_plane = buf->map;
	vu_plane = y_plane + buf->stride * buf->height;

	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
		y_plane[j * buf->stride + k] = (77 * out->r + 150 * out->g + 29 * out->b) >> 8;
		}
	}

	for (j = 0; j < buf->height / 2; ++j) {
		for (k = 0; k < buf->width / 2; ++k) {
		int u = ((-43 * out->r - 84 * out->g + 127 * out->b) >> 8) + 128;
		int v = ((127 * out->r - 106 * out->g - 21 * out->b) >> 8) + 128;
		u = u < 0 ? 0 : (u > 255 ? 255 : u);
		v = v < 0 ? 0 : (v > 255 ? 255 : v);
		vu_plane[j * buf->stride + k * 2] = v;     /* V */
		vu_plane[j * buf->stride + k * 2 + 1] = u; /* U */
		}
	}
}

static void modeset_draw_yvu420(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k;
	uint8_t *y_plane, *v_plane, *u_plane;

	buf = &out->bufs[out->front_buf ^ 1];
	y_plane = buf->map;
	v_plane = y_plane + buf->stride * buf->height;
	u_plane = v_plane + (buf->stride/2) * (buf->height/2);

	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
		y_plane[j * buf->stride + k] = (77 * out->r + 150 * out->g + 29 * out->b) >> 8;
		}
	}
	for (j = 0; j < buf->height / 2; ++j) {
		for (k = 0; k < buf->width / 2; ++k) {
		int u = ((-43 * out->r - 84 * out->g + 127 * out->b) >> 8) + 128;
		int v = ((127 * out->r - 106 * out->g - 21 * out->b) >> 8) + 128;
		u = u < 0 ? 0 : (u > 255 ? 255 : u);
		v = v < 0 ? 0 : (v > 255 ? 255 : v);
		v_plane[j * (buf->stride/2) + k] = v;
		u_plane[j * (buf->stride/2) + k] = u;
		}
	}
}

static uint16_t float_to_half(float f)
{
	union { uint32_t u; float f; } v = { .f = f };
	uint32_t sign = (v.u >> 31) & 0x1;
	int32_t exp = (int32_t)((v.u >> 23) & 0xFF) - 127 + 15;
	uint32_t mant = (v.u >> 13) & 0x3FF; /* 10 bits */
	uint16_t h;
	if (exp <= 0) {
		if (exp < -10) return (uint16_t)(sign << 15);
		mant |= 0x400; /* hidden bit */
		h = (uint16_t)((sign << 15) | (mant >> (1 - exp)));
		return h;
	} else if (exp >= 31) {
		return (uint16_t)((sign << 15) | (0x1F << 10)); /* Inf */
	} else {
		return (uint16_t)((sign << 15) | ((exp & 0x1F) << 10) | (mant & 0x3FF));
	}
}

static void modeset_draw_16161616f(struct modeset_output *out, uint32_t fourcc)
{
	struct modeset_buf *buf = &out->bufs[out->front_buf ^ 1];
	unsigned int j, k;
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	float fr = out->r / 255.0f;
	float fg = out->g / 255.0f;
	float fb = out->b / 255.0f;
	uint16_t hr = float_to_half(fr);
	uint16_t hg = float_to_half(fg);
	uint16_t hb = float_to_half(fb);
	uint16_t ha = float_to_half(1.0f);

	for (j = 0; j < buf->height; ++j) {
		uint16_t *row = (uint16_t *)(buf->map + j * buf->stride);
		for (k = 0; k < buf->width; ++k) {
		switch (fourcc) {
			case DRM_FORMAT_ARGB16161616F: /* A R G B */
			row[0] = ha; row[1] = hr; row[2] = hg; row[3] = hb; break;
			case DRM_FORMAT_ABGR16161616F: /* A B G R */
			row[0] = ha; row[1] = hb; row[2] = hg; row[3] = hr; break;
			default:
			row[0] = ha; row[1] = hr; row[2] = hg; row[3] = hb; break;
		}
		row += 4;
		}
	}
}

static void modeset_draw_rgb888(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;

	/* 更新颜色值，制造动态效果 */
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	/* 获取当前 back framebuffer 并按 RGB888 写入（3 字节/像素） */
	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
		off = buf->stride * j + k * 3;
		buf->map[off + 0] = out->r; /* R */
		buf->map[off + 1] = out->g; /* G */
		buf->map[off + 2] = out->b; /* B */
		}
	}
}

static void modeset_draw_bgr888(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;

	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
		off = buf->stride * j + k * 3;
		buf->map[off + 0] = out->b; /* B */
		buf->map[off + 1] = out->g; /* G */
		buf->map[off + 2] = out->r; /* R */
		}
	}
}

static inline void rgb_to_yuv_bt601(uint8_t r, uint8_t g, uint8_t b,
                                    uint8_t *y, uint8_t *u, uint8_t *v)
{
	int yi = (  66 * r + 129 * g +  25 * b + 128) >> 8; yi += 16;
	int ui = ( -38 * r -  74 * g + 112 * b + 128) >> 8; ui += 128;
	int vi = ( 112 * r -  94 * g -  18 * b + 128) >> 8; vi += 128;
	*y = (uint8_t)(yi < 0 ? 0 : (yi > 255 ? 255 : yi));
	*u = (uint8_t)(ui < 0 ? 0 : (ui > 255 ? 255 : ui));
	*v = (uint8_t)(vi < 0 ? 0 : (vi > 255 ? 255 : vi));
}

static void modeset_draw_yuyv422(struct modeset_output *out, uint32_t fourcc)
{
	struct modeset_buf *buf = &out->bufs[out->front_buf ^ 1];
	unsigned int j, k;

	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	uint8_t Y0, Uc, Vc;
	rgb_to_yuv_bt601(out->r, out->g, out->b, &Y0, &Uc, &Vc);

	for (j = 0; j < buf->height; ++j) {
		uint8_t *row = buf->map + j * buf->stride;
		for (k = 0; k < buf->width; k += 2) {
		uint8_t Y1 = Y0; /* 使用相同颜色，简单填充 */
		switch (fourcc) {
			case DRM_FORMAT_YUYV: /* Y0 U Y1 V */
			row[0] = Y0; row[1] = Uc; row[2] = Y1; row[3] = Vc; break;
			case DRM_FORMAT_YVYU: /* Y0 V Y1 U */
			row[0] = Y0; row[1] = Vc; row[2] = Y1; row[3] = Uc; break;
			case DRM_FORMAT_UYVY: /* U Y0 V Y1 */
			row[0] = Uc; row[1] = Y0; row[2] = Vc; row[3] = Y1; break;
			case DRM_FORMAT_VYUY: /* V Y0 U Y1 */
			row[0] = Vc; row[1] = Y0; row[2] = Uc; row[3] = Y1; break;
			default:
			row[0] = Y0; row[1] = Uc; row[2] = Y1; row[3] = Vc; break;
		}
		row += 4;
		}
	}
}

static void modeset_draw_y410(struct modeset_output *out)
{
	struct modeset_buf *buf = &out->bufs[out->front_buf ^ 1];
	unsigned int j, k;

	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	uint8_t y8, u8, v8;
	rgb_to_yuv_bt601(out->r, out->g, out->b, &y8, &u8, &v8);
	/* 10-bit 扩展：简单左移2位 */
	uint32_t Y10 = (uint32_t)(y8) << 2;
	uint32_t U10 = (uint32_t)(u8) << 2;
	uint32_t V10 = (uint32_t)(v8) << 2;
	uint32_t A2  = 0x3; /* full alpha */

	for (j = 0; j < buf->height; ++j) {
		uint32_t *row = (uint32_t *)(buf->map + j * buf->stride);
		for (k = 0; k < buf->width; ++k) {
		uint32_t pixel = (A2 << 30) | ((Y10 & 0x3FF) << 20) |
				((U10 & 0x3FF) << 10) | (V10 & 0x3FF);
		row[k] = pixel;
		}
	}
}

static void modeset_draw_yuv420(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k;
	uint8_t *y_plane, *u_plane, *v_plane;

	/* 获取当前back buffer */
	buf = &out->bufs[out->front_buf ^ 1];

	/* 计算平面指针 */
	y_plane = buf->map;
	u_plane = y_plane + buf->stride * buf->height;          // U平面在Y之后
	v_plane = u_plane + (buf->stride/2) * (buf->height/2); // V平面在U之后

	/* 更新颜色值 */
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	/* 填充Y平面 (亮度) */
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			/* BT.601亮度计算 */
			y_plane[j * buf->stride + k] = 
				(77 * out->r + 150 * out->g + 29 * out->b) >> 8;
		}
	}

	/* 填充U平面 (色度) */
	for (j = 0; j < buf->height / 2; ++j) {
		for (k = 0; k < buf->width / 2; ++k) {
			/* BT.601 U分量计算 */
			int u = ((-43 * out->r - 84 * out->g + 127 * out->b) >> 8) + 128;
			u = u < 0 ? 0 : (u > 255 ? 255 : u);
			u_plane[j * (buf->stride/2) + k] = u;
		}
	}

	/* 填充V平面 (色度) */
	for (j = 0; j < buf->height / 2; ++j) {
		for (k = 0; k < buf->width / 2; ++k) {
			/* BT.601 V分量计算 */
			int v = ((127 * out->r - 106 * out->g - 21 * out->b) >> 8) + 128;
			v = v < 0 ? 0 : (v > 255 ? 255 : v);
			v_plane[j * (buf->stride/2) + k] = v;
		}
	}
}

static void modeset_paint_nv12(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k;
	uint8_t *y_plane, *uv_plane;

	/* 获取当前back buffer */
	buf = &out->bufs[out->front_buf ^ 1];

	/* 计算平面指针 */
	y_plane = buf->map;
	uv_plane = y_plane + buf->stride * buf->height;

	/* 更新颜色值 */
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	/* 填充Y平面 (亮度) */
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			/* 简单亮度计算 (ITU-R BT.601) */
			y_plane[j * buf->stride + k] = 
				(77 * out->r + 150 * out->g + 29 * out->b) >> 8;
		}
	}

	/* 填充UV平面 (色度) - 注意NV12是UV交错存储 */
	for (j = 0; j < buf->height / 2; ++j) {
		for (k = 0; k < buf->width / 2; ++k) {
			/* UV计算 (BT.601) */
			int u = ((-43 * out->r - 84 * out->g + 127 * out->b) >> 8) + 128;
			int v = ((127 * out->r - 106 * out->g - 21 * out->b) >> 8) + 128;
			
			/* 限制范围 */
			u = u < 0 ? 0 : (u > 255 ? 255 : u);
			v = v < 0 ? 0 : (v > 255 ? 255 : v);
			
			/* NV12 UV交错存储 */
			uv_plane[j * buf->stride + k * 2] = u;     // U分量
			uv_plane[j * buf->stride + k * 2 + 1] = v; // V分量
		}
	}
}


static void modeset_draw_2101010(struct modeset_output *out, uint32_t fourcc)
{
	struct modeset_buf *buf = &out->bufs[out->front_buf ^ 1];
	uint32_t *pixel_buf = (uint32_t *)buf->map;
	int pixel_stride = buf->stride / sizeof(uint32_t);
	
	// 更新颜色值（8位）
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);
	
	// 将8位颜色值转换为10位精度（左移2位）
	uint16_t r10 = (uint16_t)(out->r << 2);
	uint16_t g10 = (uint16_t)(out->g << 2);
	uint16_t b10 = (uint16_t)(out->b << 2);
	uint8_t a8 = 0xFF; // 假设使用完全不透明alpha
	
	// 提取2位alpha值（取高2位）
	uint8_t a2 = (uint8_t)((a8 >> 6) & 0x03);
	
	// 根据不同格式处理通道顺序
	for (int j = 0; j < buf->height; ++j) {
		for (int k = 0; k < buf->width; ++k) {
			uint32_t pixel;
			
			switch (fourcc) {
				case DRM_FORMAT_ARGB2101010:
					// A(2) R(10) G(10) B(10)
					pixel = (a2 << 30) | (r10 << 20) | (g10 << 10) | b10;
					break;
				case DRM_FORMAT_ABGR2101010:
					// A(2) B(10) G(10) R(10)
					pixel = (a2 << 30) | (b10 << 20) | (g10 << 10) | r10;
					break;
				case DRM_FORMAT_RGBA1010102:
					// R(10) G(10) B(10) A(2)
					pixel = (r10 << 22) | (g10 << 12) | (b10 << 2) | a2;
					break;
				case DRM_FORMAT_BGRA1010102:
					// B(10) G(10) R(10) A(2)
					pixel = (b10 << 22) | (g10 << 12) | (r10 << 2) | a2;
					break;
				default:
					pixel = 0;
					fprintf(stderr, "Unsupported 2101010 format: 0x%08x\n", fourcc);
					break;
			}
			
			pixel_buf[j * pixel_stride + k] = pixel;
		}
	}
}

static void modeset_draw_p010(struct modeset_output *out)
{
	struct modeset_buf *buf = &out->bufs[out->front_buf ^ 1];
	uint16_t *y_plane = (uint16_t *)buf->map;
	// P010格式：Y平面步长是buf->stride * 2字节，UV平面在Y平面之后
	uint16_t *uv_plane = (uint16_t *)(buf->map + (buf->stride * 2) * buf->height);
	// stride_pixels应该是字节步长除以2（因为uint16_t是2字节）
	int stride_pixels = buf->stride;

	// 更新颜色值（假设 out->r/g/b 是 8-bit）
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);

	// 计算 10-bit YUV (BT.601)
	int y_val = (( 66 * out->r + 129 * out->g +  25 * out->b + 128) >> 8) +  16;
	int u_val = ((-38 * out->r -  74 * out->g + 112 * out->b + 128) >> 8) + 128;
	int v_val = ((112 * out->r -  94 * out->g -  18 * out->b + 128) >> 8) + 128;

	// 裁剪并左移 6 位（P010 高位对齐）
	uint16_t y10 = ((y_val < 0 ? 0 : (y_val > 1023 ? 1023 : y_val)) << 6);
	uint16_t uv_u = ((u_val < 0 ? 0 : (u_val > 1023 ? 1023 : u_val)) << 6);
	uint16_t uv_v = ((v_val < 0 ? 0 : (v_val > 1023 ? 1023 : v_val)) << 6);

	// 填充 Y 平面
	for (int j = 0; j < buf->height; ++j) {
		uint16_t *y_line = y_plane + j * stride_pixels;
		for (int k = 0; k < buf->width; ++k) y_line[k] = y10;
	}

	// 填充 UV 平面（NV12 风格交错）
	// 注意：UV平面在水平方向是2:1下采样的，所以宽度应该是buf->width / 2
	for (int j = 0; j < buf->height / 2; ++j) {
		uint16_t *uv_line = uv_plane + j * stride_pixels;
		for (int k = 0; k < buf->width / 2; ++k) {
			uv_line[2 * k] = uv_u;     // U
			uv_line[2 * k + 1] = uv_v; // V
		}
	}
}


/*
 * modeset_draw_out() prepares the framebuffer with the drawing and then it asks
 * for the driver to perform an atomic commit. This will lead to a page-flip and
 * the content of the framebuffer will be displayed. In this simple example
 * we're only using the primary plane, but we could also be updating other
 * planes in the same atomic commit.
 *
 * Just like in modeset_perform_modeset(), we first setup everything with
 * modeset_atomic_prepare_commit() and then actually perform the atomic commit.
 * But there are some important differences:
 *
 * 1. Here we just want to perform a commit that changes the state of a specific
 *    output, and in modeset_perform_modeset() we did an atomic commit that was
 *    supposed to setup all the outputs at once. So there's no need to prepare
 *    every output before performing the atomic commit. But let's suppose you
 *    prepare every output and then perform the commit. It should schedule a
 *    page-flip for all of them, but modeset_draw_out() was called because the
 *    page-flip for a specific output has finished. The others may not be
 *    prepared for a page-flip yet (e.g. in the middle of a scanout), so these
 *    page-flips will fail.
 *
 * 2. Here we have already painted the framebuffer and also we don't use the
 *    flag DRM_MODE_ALLOW_MODESET anymore, since the modeset already happened.
 *    We could continue to use this flag, as it makes no difference if
 *    modeset_perform_modeset() is correct and there's no bug in the kernel.
 *    The flag only allows (it doesn't force) the driver to perform a modeset,
 *    but we have already performed it in modeset_perform_modeset() and now we
 *    just want page-flips to occur. If we still need to perform modesets it
 *    means that we have a bug somewhere, and it may be better to fail than to
 *    glitch (a modeset can cause unecessary latency and also blank the screen).
 */

static void modeset_draw_out(int fd, struct modeset_output *out)
{
	drmModeAtomicReq *req;
	int ret, flags;

	/* draw on framebuffer of the output */
	switch (out->format) {
		case DRM_FORMAT_ARGB8888:
		case DRM_FORMAT_ABGR8888:
		case DRM_FORMAT_RGBA8888:
		case DRM_FORMAT_BGRA8888:
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_XBGR8888:
		case DRM_FORMAT_RGBX8888:
		case DRM_FORMAT_BGRX8888:
			// modeset_paint_framebuffer(out);
			modeset_paint_framebuffer_green(out);
			break;
		case DRM_FORMAT_RGB888:
			modeset_draw_rgb888(out);
			break;
		case DRM_FORMAT_BGR888:
			modeset_draw_bgr888(out);
			break;
		case DRM_FORMAT_RGB565:
		case DRM_FORMAT_BGR565:
			modeset_paint_framebuffer565(out);
			break;
		case DRM_FORMAT_YUV420:
			modeset_draw_yuv420(out);
			break;
		case DRM_FORMAT_NV12:
			modeset_paint_nv12(out);
			break;
		case DRM_FORMAT_P010:
			modeset_draw_p010(out);
			break;
		case DRM_FORMAT_XYUV8888:
			modeset_paint_framebuffer_xyuv8888(out);
			break;
		case DRM_FORMAT_YUYV:
			modeset_draw_yuyv422(out, DRM_FORMAT_YUYV);
			break;
		case DRM_FORMAT_YVYU:
			modeset_draw_yuyv422(out, DRM_FORMAT_YVYU);
			break;
		case DRM_FORMAT_UYVY:
			modeset_draw_yuyv422(out, DRM_FORMAT_UYVY);
			break;
		case DRM_FORMAT_VYUY:
			modeset_draw_yuyv422(out, DRM_FORMAT_VYUY);
			break;
		case DRM_FORMAT_Y410:
			modeset_draw_y410(out);
			break;
		case DRM_FORMAT_NV21:
			modeset_paint_nv21(out);
			break;
		case DRM_FORMAT_YVU420:
			modeset_draw_yvu420(out);
			break;
		case DRM_FORMAT_ARGB1555:
			modeset_paint_framebuffer_1555_like(out, DRM_FORMAT_ARGB1555);
			break;
		case DRM_FORMAT_ABGR1555:
			modeset_paint_framebuffer_1555_like(out, DRM_FORMAT_ABGR1555);
			break;
		case DRM_FORMAT_XRGB1555:
			modeset_paint_framebuffer_1555_like(out, DRM_FORMAT_XRGB1555);
			break;
		case DRM_FORMAT_XBGR1555:
			modeset_paint_framebuffer_1555_like(out, DRM_FORMAT_XBGR1555);
			break;
		case DRM_FORMAT_RGBA5551:
			modeset_paint_framebuffer_1555_like(out, DRM_FORMAT_RGBA5551);
			break;
		case DRM_FORMAT_BGRA5551:
			modeset_paint_framebuffer_1555_like(out, DRM_FORMAT_BGRA5551);
			break;
		case DRM_FORMAT_RGBX5551:
			modeset_paint_framebuffer_1555_like(out, DRM_FORMAT_RGBX5551);
			break;
		case DRM_FORMAT_BGRX5551:
			modeset_paint_framebuffer_1555_like(out, DRM_FORMAT_BGRX5551);
			break;
		case DRM_FORMAT_ARGB16161616F:
			modeset_draw_16161616f(out, DRM_FORMAT_ARGB16161616F);
			break;
		case DRM_FORMAT_ABGR16161616F:
			modeset_draw_16161616f(out, DRM_FORMAT_ABGR16161616F);
			break;
		case DRM_FORMAT_ARGB2101010:
			modeset_draw_2101010(out, DRM_FORMAT_ARGB2101010);
			break;
		case DRM_FORMAT_ABGR2101010:
			modeset_draw_2101010(out, DRM_FORMAT_ABGR2101010);
			break;
		case DRM_FORMAT_RGBA1010102:
			modeset_draw_2101010(out, DRM_FORMAT_RGBA1010102);
			break;
		case DRM_FORMAT_BGRA1010102:
			modeset_draw_2101010(out, DRM_FORMAT_BGRA1010102);
			break;
		default:
			fprintf(stderr, "No draw function for format 0x%08x\n", out->format);
			break;
	}


	/* prepare output for atomic commit */
	req = drmModeAtomicAlloc();
	ret = modeset_atomic_prepare_commit(fd, out, req);
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed, %d\n", errno);
		return;
	}

	/* We've just draw on the framebuffer, prepared the commit and now it's
	 * time to perform a page-flip to display its content.
	 *
	 * DRM_MODE_PAGE_FLIP_EVENT signalizes that we want to receive a
	 * page-flip event in the DRM-fd when the page-flip happens. This flag
	 * is also used in the non-atomic examples, so you're probably familiar
	 * with it.
	 *
	 * DRM_MODE_ATOMIC_NONBLOCK makes the page-flip non-blocking. We don't
	 * want to be blocked waiting for the commit to happen, since we can use
	 * this time to prepare a new framebuffer, for instance. We can only do
	 * this because there are mechanisms to know when the commit is complete
	 * (like page flip event, explained above).
	 */
	flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	drmModeAtomicFree(req);

	if (ret < 0) {
		fprintf(stderr, "atomic commit failed, %d\n", errno);
		return;
	}
	out->front_buf ^= 1;
	out->pflip_pending = true;
}

/*
 * modeset_page_flip_event() changes. Now that we are using page_flip_handler2,
 * we also receive the CRTC that is responsible for this event. When using the
 * atomic API we commit multiple CRTC's at once, so we need the information of
 * what output caused the event in order to schedule a new page-flip for it.
 */

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    unsigned int crtc_id, void *data)
{
	struct modeset_output *out, *iter;

	/* find the output responsible for this event */
	out = NULL;
	for (iter = output_list; iter; iter = iter->next) {
		if (iter->crtc.id == crtc_id) {
			out = iter;
			break;
		}
	}
	if (out == NULL)
		return;

	out->pflip_pending = false;
	if (!out->cleanup && !photo)
		modeset_draw_out(fd, out);
}

/*
 * modeset_perform_modeset() is new. First we define what properties have to be
 * changed and the values that they will receive. To check if the modeset will
 * work as expected, we perform an atomic commit with the flag
 * DRM_MODE_ATOMIC_TEST_ONLY. With this flag the DRM driver tests if the atomic
 * commit would work, but it doesn't commit it to the hardware. After, the same
 * atomic commit is performed without the TEST_ONLY flag, but not only before we
 * draw on the framebuffers of the outputs. This is necessary to avoid
 * displaying unwanted content.
 *
 * NOTE: we can't perform an atomic commit without an attached frambeuffer
 * (even when we have DRM_MODE_ATOMIC_TEST_ONLY). It will simply fail.
 */

static int modeset_perform_modeset(int fd)
{
	int ret, flags;
	struct modeset_output *iter;
	drmModeAtomicReq *req;

	/* prepare modeset on all outputs */
	req = drmModeAtomicAlloc();
	for (iter = output_list; iter; iter = iter->next) {
		ret = modeset_atomic_prepare_commit(fd, iter, req);
		if (ret < 0)
			break;
	}
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed, %d\n", errno);
		return ret;
	}

	/* perform test-only atomic commit */
	flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret < 0) {
		fprintf(stderr, "test-only atomic commit failed, ret = %d\n", ret);
		drmModeAtomicFree(req);
		return ret;
	}

	if(!photo){
		/* draw on back framebuffer of all outputs */
		for (iter = output_list; iter; iter = iter->next) {

			/* colors initialization, this is the first time we're drawing */
			iter->r = rand() % 0xff;
			iter->g = rand() % 0xff;
			iter->b = rand() % 0xff;
			iter->r_up = iter->g_up = iter->b_up = true;
			switch (iter->format) {
				case DRM_FORMAT_ARGB8888:
				case DRM_FORMAT_ABGR8888:
				case DRM_FORMAT_RGBA8888:
				case DRM_FORMAT_BGRA8888:
				case DRM_FORMAT_XRGB8888:
				case DRM_FORMAT_XBGR8888:
				case DRM_FORMAT_RGBX8888:
				case DRM_FORMAT_BGRX8888:
					// modeset_paint_framebuffer(iter);
					modeset_paint_framebuffer_green(iter);
					break;
				case DRM_FORMAT_RGB888:
					modeset_draw_rgb888(iter);
					break;
				case DRM_FORMAT_BGR888:
					modeset_draw_bgr888(iter);
					break;
				case DRM_FORMAT_RGB565:
				case DRM_FORMAT_BGR565:
					modeset_paint_framebuffer565(iter);
					break;
				case DRM_FORMAT_YUV420:
					modeset_draw_yuv420(iter);
					break;
				case DRM_FORMAT_NV12:
					modeset_paint_nv12(iter);
					break;
				case DRM_FORMAT_P010:
					modeset_draw_p010(iter);
					break;
				case DRM_FORMAT_XYUV8888:
					modeset_paint_framebuffer_xyuv8888(iter);
					break;
				case DRM_FORMAT_YUYV:
					modeset_draw_yuyv422(iter, DRM_FORMAT_YUYV);
					break;
				case DRM_FORMAT_YVYU:
					modeset_draw_yuyv422(iter, DRM_FORMAT_YVYU);
					break;
				case DRM_FORMAT_UYVY:
					modeset_draw_yuyv422(iter, DRM_FORMAT_UYVY);
					break;
				case DRM_FORMAT_VYUY:
					modeset_draw_yuyv422(iter, DRM_FORMAT_VYUY);
					break;
				case DRM_FORMAT_Y410:
					modeset_draw_y410(iter);
					break;
				case DRM_FORMAT_NV21:
					modeset_paint_nv21(iter);
					break;
				case DRM_FORMAT_YVU420:
					modeset_draw_yvu420(iter);
					break;
				case DRM_FORMAT_ARGB1555:
					modeset_paint_framebuffer_1555_like(iter, DRM_FORMAT_ARGB1555);
					break;
				case DRM_FORMAT_ABGR1555:
					modeset_paint_framebuffer_1555_like(iter, DRM_FORMAT_ABGR1555);
					break;
				case DRM_FORMAT_XRGB1555:
					modeset_paint_framebuffer_1555_like(iter, DRM_FORMAT_XRGB1555);
					break;
				case DRM_FORMAT_XBGR1555:
					modeset_paint_framebuffer_1555_like(iter, DRM_FORMAT_XBGR1555);
					break;
				case DRM_FORMAT_RGBA5551:
					modeset_paint_framebuffer_1555_like(iter, DRM_FORMAT_RGBA5551);
					break;
				case DRM_FORMAT_BGRA5551:
					modeset_paint_framebuffer_1555_like(iter, DRM_FORMAT_BGRA5551);
					break;
				case DRM_FORMAT_RGBX5551:
					modeset_paint_framebuffer_1555_like(iter, DRM_FORMAT_RGBX5551);
					break;
				case DRM_FORMAT_BGRX5551:
					modeset_paint_framebuffer_1555_like(iter, DRM_FORMAT_BGRX5551);
					break;
				case DRM_FORMAT_ARGB16161616F:
					modeset_draw_16161616f(iter, DRM_FORMAT_ARGB16161616F);
					break;
				case DRM_FORMAT_ABGR16161616F:
					modeset_draw_16161616f(iter, DRM_FORMAT_ABGR16161616F);
					break;
				case DRM_FORMAT_ARGB2101010:
					modeset_draw_2101010(iter, DRM_FORMAT_ARGB2101010);
					break;
				case DRM_FORMAT_ABGR2101010:
					modeset_draw_2101010(iter, DRM_FORMAT_ABGR2101010);
					break;
				case DRM_FORMAT_RGBA1010102:
					modeset_draw_2101010(iter, DRM_FORMAT_RGBA1010102);
					break;
				case DRM_FORMAT_BGRA1010102:
					modeset_draw_2101010(iter, DRM_FORMAT_BGRA1010102);
					break;
				default:
					fprintf(stderr, "No draw function for format 0x%08x\n", iter->format);
					break;
			}
		}

	}

	/* initial modeset on all outputs */
	flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret < 0)
		fprintf(stderr, "modeset atomic commit failed, %d\n", errno);

	drmModeAtomicFree(req);
	return ret;
}

/*
 * modeset_draw() changes. If we got here, the modeset already occurred. When
 * the page-flip for a certain output is done, an event will be fired and we'll
 * be able to handle it.
 *
 * Here we define the function that should handle these events, which is
 * modeset_page_flip_event(). This function calls modeset_draw_out(), which is
 * responsible for preparing a new framebuffer and performing another atomic
 * commit for us.
 *
 * Then we have a 5 seconds loop that keeps waiting for the events that are
 * fired when the page-flip is complete. drmHandleEvent() is reponsible for
 * reading the events from the fd and to call modeset_page_flip_event() for
 * each one of them.
 */

static int modeset_draw(int fd)
{
	int ret;
	fd_set fds;
	time_t start, cur;
	struct timeval v;
	drmEventContext ev;

	/* init variables */
	srand(time(&start));
	FD_ZERO(&fds);
	memset(&v, 0, sizeof(v));
	memset(&ev, 0, sizeof(ev));

	/* 3 is the first version that allow us to use page_flip_handler2, which
	 * is just like page_flip_handler but with the addition of passing the
	 * crtc_id as argument to the function that will handle page-flip events
	 * (in our case, modeset_page_flip_event()). This is good because we can
	 * find out for what output the page-flip happened.
	 *
	 * The usage of page_flip_handler2 is the reason why we needed to verify
	 * the support for DRM_CAP_CRTC_IN_VBLANK_EVENT.
	 */
	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	/* perform modeset using atomic commit */
	ret = modeset_perform_modeset(fd);
	if (ret < 0){
		fprintf(stderr, "modeset_perform_modeset failed\n");
		return ret;
	}
	/* wait 5s for VBLANK or input events */
	while (time(&cur) < start + 5) {
		FD_SET(0, &fds);
		FD_SET(fd, &fds);
		v.tv_sec = start + 5 - cur;

		ret = select(fd + 1, &fds, NULL, NULL, &v);
		if (ret < 0) {
			fprintf(stderr, "select() failed with %d: %m\n", errno);
			break;
		} else if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "exit due to user-input\n");
			break;
		} else if (FD_ISSET(fd, &fds)) {
			/* read the fd looking for events and handle each event
			 * by calling modeset_page_flip_event() */
			drmHandleEvent(fd, &ev);
		}
	}
}

/*
 * modeset_cleanup() stays the same.
 */

static void modeset_cleanup(int fd)
{
	struct modeset_output *iter;
	drmEventContext ev;
	int ret;

	/* init variables */
	memset(&ev, 0, sizeof(ev));
	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	while (output_list) {
		/* get first output from list */
		iter = output_list;

		/* if a page-flip is pending, wait for it to complete */
		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending) {
			ret = drmHandleEvent(fd, &ev);
			if (ret)
				break;
		}

		/* move head of the list to the next output */
		output_list = iter->next;

		/* destroy current output */
		modeset_output_destroy(fd, iter);
	}
}

/*
 * main() also changes. Instead of performing the KMS setup calling
 * drmModeSetCrtc(), we instead setup it using the atomic API with the
 * function modeset_perform_modeset(), which is called by modeset_draw().
 */

int main(int argc, char **argv)
{
	int ret = 0, fd;
	const char *card = "/dev/dri/card1";
	uint32_t fmt = DRM_FORMAT_ARGB8888;
	char *format_str = "ARGB8888";
	char *photo_str = NULL;
	struct modeset_dev *iter;
	int c;

	while ((c = getopt(argc, argv, "d:f:w:h:c:C:W:H:t:s:r:p:g:")) != -1) {
		switch (c) {
		case 'd':
			card = optarg;
			break;
		case 'f':
			format_str = optarg;
			fmt = parse_format(optarg);
			if (fmt == 0) {
				fprintf(stderr, "Invalid format '%s'\n", optarg);
				print_usage(argv[0]);
				return EXIT_FAILURE;
			}
			break;
		case 'w':
			width = atoi(optarg);
			CRTC_W = width;
			SRC_W = width;
			break;
		case 'h':
			height = atoi(optarg);
			CRTC_H = height;
			SRC_H = height;
			break;
		case 'c':
			CRTC_W = atoi(optarg);
			break;
		case 'C':
			CRTC_H = atoi(optarg);
			break;
		case 'W':
			SRC_W = atoi(optarg);
			break;
		case 'H':
			SRC_H = atoi(optarg);
			break;
		case 't':
			photo_str = optarg;
			if (strcmp(photo_str, "photo") == 0)
				photo = true;
			break;
		case 's':
			scaling_factor = atof(optarg);
			fprintf(stderr, "scaling_factor '%f'\n", scaling_factor);
			break;
		case 'r':
			rdma_id = atoi(optarg);
			break;
		case 'p':
			p_id = atoi(optarg);
			break;
		// case 'g':
		// 	set_gamma = true;
		// 	break;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	fprintf(stderr, "Using DRM device '%s' with format 0x%08x\n", card, fmt);

	const char *ext = ".yuv";
	switch (fmt) {
		case DRM_FORMAT_NV12:
		case DRM_FORMAT_YUV420:
		case DRM_FORMAT_YVU420:
		case DRM_FORMAT_P010:
		case DRM_FORMAT_XYUV8888:
			ext = ".yuv";
			break;
		default:
			ext = ".rgb";
			break;
	}
	snprintf(DATA, sizeof(DATA), DATA_DIR"/%d_%d_%s%s", width, height, format_str, ext);
	fprintf(stderr, "DATA =  '%s'\n", DATA);

	/* open the DRM device */
	ret = modeset_open(&fd, card);
	if (ret)
		goto out_return;

	/* prepare all connectors and CRTCs */
	ret = modeset_prepare(fd, fmt);
	if (ret)
		goto out_close;

	/* draw some colors for 5seconds */
	ret = modeset_draw(fd);
	if (ret)
		goto out_close;

	/* cleanup everything */
	modeset_cleanup(fd);

	ret = 0;

out_close:
	close(fd);
out_return:
	if (ret) {
		errno = -ret;
		fprintf(stderr, "modeset failed with error %d: %m\n", errno);
	} else {
		fprintf(stderr, "drm modeset-atomic test case successful!\n");
	}
	return ret;
}
