#ifndef TYPES_WLR_DRM_LEASE_MANAGER_V1
#define TYPES_WLR_DRM_LEASE_MANAGER_V1

#include <wlr/types/wlr_drm_lease_v1.h>

struct wlr_drm_backend;

struct wlr_drm_lease_device_v1 *drm_lease_device_v1_create(
	struct wl_display *display, struct wlr_drm_backend *backend);

#endif
