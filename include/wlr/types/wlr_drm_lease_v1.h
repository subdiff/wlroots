/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_DRM_LEASE_V1_H
#define WLR_TYPES_WLR_DRM_LEASE_V1_H

#include <wayland-server.h>
#include <wlr/backend.h>

struct wlr_drm_backend;

struct wlr_drm_lease_manager {
	struct wl_list devices;

	struct {
		/**
		 * Upon receiving this signal, call
		 * wlr_drm_lease_device_v1_grant_lease_request to grant a lease of the
		 * requested DRM resources, or
		 * wlr_drm_lease_device_v1_reject_lease_request to reject the request.
		 */
		struct wl_signal request;
	} events;
};

struct wlr_drm_lease_device_v1 {
	struct wl_list resources; // wl_resource_get_link
	struct wl_global *global;

	struct wlr_drm_lease_manager *manager;
	struct wlr_drm_backend *backend;

	struct wl_list connectors; // wlr_drm_lease_connector_v1::link
	struct wl_list leases; // wl_resource_get_link
	struct wl_list requests; // wl_resource_get_link
	struct wl_list link; // wlr_drm_lease_manager::devices

	struct wl_listener display_destroy;

	void *data;
};

struct wlr_drm_lease_v1;

struct wlr_drm_lease_connector_v1 {
	struct wl_list resources; // wl_resource_get_link

	struct wlr_output *output;
	struct wlr_drm_connector *drm_connector;

	/** NULL if no client is currently leasing this connector */
	struct wlr_drm_lease_v1 *active_lease;

	struct wl_list link; // wlr_drm_lease_device_v1::connectors
};

struct wlr_drm_lease_request_v1 {
	struct wl_resource *resource; // wlr_drm_device_v1::lease_requests

	struct wlr_drm_lease_device_v1 *device;
	struct wlr_drm_lease_connector_v1 *connector;
	bool invalid;

	/** NULL until the lease is submitted */
	struct wlr_drm_lease_v1 *lease;
};

struct wlr_drm_lease_v1 {
	struct wl_resource *resource; // wlr_drm_device_v1::leases

	struct wlr_drm_lease_device_v1 *device;
	struct wlr_drm_lease_connector_v1 *connector;

	uint32_t lessee_id;

	void *data;
};

/**
 * Creates a DRM lease manager. A DRM lease device will be created for each
 * DRM backend supplied.
 * Returns NULL if no DRM backend is given.
 */
struct wlr_drm_lease_manager *wlr_drm_lease_manager_create(
	struct wl_display *display, struct wlr_backend *backend);

/**
 * Offers a wlr_output for lease.
 * Returns false if the output can't be offered to lease.
 */
bool wlr_drm_lease_manager_offer_output(
	struct wlr_drm_lease_manager *manager, struct wlr_output *output);

/**
 * Withdraws a previously offered output for lease. If the output is leased to
 * a client, a finished event will be send and the lease will be terminated.
 */
void wlr_drm_lease_manager_withdraw_output(
	struct wlr_drm_lease_manager *manager, struct wlr_output *output);

/**
 * Grants a client's lease request. The lease device will then provision the
 * DRM lease and transfer the file descriptor to the client. After calling this,
 * each wlr_output leased is destroyed, and will be re-issued through
 * wlr_backend.events.new_outputs when the lease is revoked.
 *
 * This will return NULL without leasing any resources if the lease is invalid;
 * this can happen for example if two clients request the same resources and an
 * attempt to grant both leases is made.
 */
struct wlr_drm_lease_v1 *wlr_drm_lease_request_v1_grant(
	struct wlr_drm_lease_request_v1 *request);

/**
 * Rejects a client's lease request. The output will still be available to
 * lease until withdrawn by the compositor.
 */
void wlr_drm_lease_request_v1_reject(struct wlr_drm_lease_request_v1 *request);

/**
 * Revokes a client's lease request. The output will still be available to
 * lease until withdrawn by the compositor.
 */
void wlr_drm_lease_v1_revoke(struct wlr_drm_lease_v1 *lease);

#endif
