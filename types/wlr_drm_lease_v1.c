#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/util/log.h>
#include <wayland-util.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "backend/drm/drm.h"
#include "drm-lease-v1-protocol.h"
#include "util/shm.h"
#include "util/signal.h"

static struct wp_drm_lease_device_v1_interface lease_device_impl;
static struct wp_drm_lease_connector_v1_interface lease_connector_impl;
static struct wp_drm_lease_request_v1_interface lease_request_impl;
static struct wp_drm_lease_v1_interface lease_impl;

static void drm_lease_connector_v1_send_to_client(
		struct wlr_drm_lease_connector_v1 *connector,
		struct wl_client *wl_client, struct wl_resource *device);

static struct wlr_drm_lease_device_v1 *wlr_drm_lease_device_v1_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
				&wp_drm_lease_device_v1_interface, &lease_device_impl));
	return wl_resource_get_user_data(resource);
}

static struct wlr_drm_lease_request_v1 *wlr_drm_lease_request_v1_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
				&wp_drm_lease_request_v1_interface, &lease_request_impl));
	return wl_resource_get_user_data(resource);
}

static struct wlr_drm_lease_connector_v1 *
wlr_drm_lease_connector_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
				&wp_drm_lease_connector_v1_interface, &lease_connector_impl));
	return wl_resource_get_user_data(resource);
}

static struct wlr_drm_lease_v1 *wlr_drm_lease_v1_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
				&wp_drm_lease_v1_interface, &lease_impl));
	return wl_resource_get_user_data(resource);
}

struct wlr_drm_lease_v1 *wlr_drm_lease_request_v1_grant(
		struct wlr_drm_lease_request_v1 *request) {
	assert(request->lease);

	struct wlr_drm_lease_v1 *lease = request->lease;

	if (request->invalid || request->connector->active_lease) {
		wlr_log(WLR_ERROR, "Invalid lease request");
		wp_drm_lease_v1_send_finished(lease->resource);
		return NULL;
	}

	struct wlr_drm_lease_connector_v1 *connector = request->connector;
	wl_list_remove(&connector->link);
	wl_list_init(&connector->link);

	struct wlr_drm_lease_device_v1 *device = request->device;

	int fd = wlr_drm_backend_create_lease(device->backend, connector->output,
			&lease->lessee_id);

	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "drm_create_lease failed");
		wp_drm_lease_v1_send_finished(lease->resource);
		return NULL;
	}

	connector->active_lease = lease;
	lease->connector = connector;

	struct wl_resource *wl_resource, *tmp;
	wl_resource_for_each_safe(wl_resource, tmp, &connector->resources) {
		wp_drm_lease_connector_v1_send_withdrawn(wl_resource);
		wl_resource_set_user_data(wl_resource, NULL);
		wl_list_remove(wl_resource_get_link(wl_resource));
		wl_list_init(wl_resource_get_link(wl_resource));
	}

	wp_drm_lease_v1_send_lease_fd(lease->resource, fd);
	close(fd);
	return lease;
}

void wlr_drm_lease_request_v1_reject(
		struct wlr_drm_lease_request_v1 *request) {
	assert(request && request->lease);
	wp_drm_lease_v1_send_finished(request->lease->resource);
	request->invalid = true;
}

void wlr_drm_lease_v1_revoke(struct wlr_drm_lease_v1 *lease) {
	assert(lease);
	if (lease->resource != NULL) {
		wp_drm_lease_v1_send_finished(lease->resource);
	}

	struct wlr_drm_lease_device_v1 *device = lease->device;
	if (lease->lessee_id != 0) {
		if (wlr_drm_backend_terminate_lease(device->backend,
				lease->lessee_id) < 0) {
			wlr_log_errno(WLR_DEBUG, "drm_terminate_lease");
		}
	}
	struct wlr_drm_lease_connector_v1 *connector = lease->connector;
	connector->active_lease = NULL;

	struct wl_resource *resource;
	wl_resource_for_each(resource, &device->resources) {
		struct wl_client *client = wl_resource_get_client(resource);
		drm_lease_connector_v1_send_to_client(connector, client, resource);
	}
}

static void drm_lease_v1_destroy(struct wlr_drm_lease_v1 *lease) {
	wlr_drm_lease_v1_revoke(lease);
	free(lease);
}

static void drm_lease_v1_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_drm_lease_v1 *lease = wlr_drm_lease_v1_from_resource(resource);
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
	lease->resource = NULL;
	drm_lease_v1_destroy(lease);
}

static void drm_lease_v1_handle_destroy(
		struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static struct wp_drm_lease_v1_interface lease_impl = {
	.destroy = drm_lease_v1_handle_destroy,
};

static void drm_lease_request_v1_destroy(struct wlr_drm_lease_request_v1 *req) {
	if (!req) {
		return;
	}
	free(req->connector);
	free(req);
}

static void drm_lease_request_v1_handle_resource_destroy(
		struct wl_resource *resource) {
	struct wlr_drm_lease_request_v1 *req =
		wlr_drm_lease_request_v1_from_resource(resource);
	drm_lease_request_v1_destroy(req);
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
}

static void drm_lease_request_v1_handle_request_connector(
		struct wl_client *client, struct wl_resource *request_resource,
		struct wl_resource *connector_resource) {
	struct wlr_drm_lease_request_v1 *request =
		wlr_drm_lease_request_v1_from_resource(request_resource);
	struct wlr_drm_lease_connector_v1 *connector =
		wlr_drm_lease_connector_v1_from_resource(connector_resource);

	if (!connector) {
		/* This connector offer has been withdrawn */
		request->invalid = true;
		return;
	}

	request->connector = connector;
}

static void drm_lease_request_v1_handle_submit(
		struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct wlr_drm_lease_request_v1 *request =
			wlr_drm_lease_request_v1_from_resource(resource);

	struct wlr_drm_lease_v1 *lease = calloc(1, sizeof(struct wlr_drm_lease_v1));
	if (!lease) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_drm_lease_v1");
		wl_resource_post_no_memory(resource);
		return;
	}

	struct wl_resource *wl_resource = wl_resource_create(
			client, &wp_drm_lease_v1_interface, 1, id);
	if (!wl_resource) {
		wlr_log(WLR_ERROR, "Failed to allocate wl_resource");
		wl_resource_post_no_memory(resource);
		free(lease);
		return;
	}

	lease->device = request->device;
	lease->resource = wl_resource;
	request->lease = lease;
	wl_list_insert(&lease->device->leases, wl_resource_get_link(wl_resource));

	wl_resource_set_implementation(wl_resource, &lease_impl,
			lease, drm_lease_v1_handle_resource_destroy);

	if (request->invalid || request->connector->active_lease) {
		/* Pre-emptively reject invalid lease requests */
		wp_drm_lease_v1_send_finished(lease->resource);
	} else {
		wlr_signal_emit_safe(&request->device->manager->events.request,
				request);
	}
	wl_resource_destroy(resource);
}

static struct wp_drm_lease_request_v1_interface lease_request_impl = {
	.request_connector = drm_lease_request_v1_handle_request_connector,
	.submit = drm_lease_request_v1_handle_submit,
};

static void drm_lease_device_v1_handle_resource_destroy(
		struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
}

static void drm_lease_device_v1_handle_stop(
		struct wl_client *client, struct wl_resource *resource) {
	wp_drm_lease_device_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static void drm_lease_device_v1_handle_create_lease_request(
		struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct wlr_drm_lease_device_v1 *device =
		wlr_drm_lease_device_v1_from_resource(resource);

	struct wlr_drm_lease_request_v1 *req =
		calloc(1, sizeof(struct wlr_drm_lease_request_v1));
	if (!req) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_drm_lease_request_v1");
		wl_resource_post_no_memory(resource);
		return;
	}

	struct wl_resource *wl_resource = wl_resource_create(client,
		&wp_drm_lease_request_v1_interface, 1, id);
	if (!wl_resource) {
		wlr_log(WLR_ERROR, "Failed to allocate wl_resource");
		wl_resource_post_no_memory(resource);
		free(req);
		return;
	}

	req->device = device;
	req->resource = wl_resource;

	wl_resource_set_implementation(wl_resource, &lease_request_impl,
		req, drm_lease_request_v1_handle_resource_destroy);

	wl_list_insert(&device->requests, wl_resource_get_link(wl_resource));
}

static struct wp_drm_lease_device_v1_interface lease_device_impl = {
	.stop = drm_lease_device_v1_handle_stop,
	.create_lease_request = drm_lease_device_v1_handle_create_lease_request,
};

static void drm_connector_v1_handle_resource_destroy(
		struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
}

static void drm_connector_v1_handle_destroy(
		struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static struct wp_drm_lease_connector_v1_interface lease_connector_impl = {
	.destroy = drm_connector_v1_handle_destroy,
};

static void drm_lease_connector_v1_send_to_client(
		struct wlr_drm_lease_connector_v1 *connector,
		struct wl_client *wl_client, struct wl_resource *device) {
	if (connector->active_lease) {
		return;
	}

	struct wl_resource *wl_resource = wl_resource_create(wl_client,
			&wp_drm_lease_connector_v1_interface, 1, 0);
	if (!wl_resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(wl_resource, &lease_connector_impl,
			connector, drm_connector_v1_handle_resource_destroy);
	wp_drm_lease_device_v1_send_connector(device, wl_resource);

	struct wlr_output *output = connector->output;
	wp_drm_lease_connector_v1_send_name(wl_resource, output->name);

	char description[128];
	snprintf(description, sizeof(description), "%s %s %s (%s)",
		output->make, output->model, output->serial, output->name);
	wp_drm_lease_connector_v1_send_description(wl_resource, description);

	struct wlr_drm_connector *drm_connector =
		(struct wlr_drm_connector *)output;
	wp_drm_lease_connector_v1_send_connector_id(
			wl_resource, drm_connector->id);

	wl_list_insert(&connector->resources, wl_resource_get_link(wl_resource));
}

static void lease_device_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_drm_lease_device_v1 *device = data;

	struct wl_resource *wl_resource  = wl_resource_create(wl_client,
		&wp_drm_lease_device_v1_interface, version, id);

	if (!wl_resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(wl_resource, &lease_device_impl, device,
			drm_lease_device_v1_handle_resource_destroy);

	struct wlr_drm_backend *backend = get_drm_backend_from_backend(
			device->backend);
	char *path = drmGetDeviceNameFromFd2(backend->fd);
	int fd = open(path, O_RDWR);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "Unable to clone DRM fd for leasing.\n");
		free(path);
	} else {
		if (drmIsMaster(fd)) {
			drmDropMaster(fd);
		}
		assert(!drmIsMaster(fd) && "Won't send master fd to client");
		wp_drm_lease_device_v1_send_drm_fd(wl_resource, fd);
		free(path);
		close(fd);
	}

	wl_list_insert(&device->resources, wl_resource_get_link(wl_resource));

	struct wlr_drm_lease_connector_v1 *connector;
	wl_list_for_each(connector, &device->connectors, link) {
		drm_lease_connector_v1_send_to_client(connector, wl_client,
				wl_resource);
	}
}

bool wlr_drm_lease_manager_offer_output(
		struct wlr_drm_lease_manager *manager, struct wlr_output *output) {
	assert(manager && output);
	assert(wlr_output_is_drm(output));

	struct wlr_drm_connector *drm_connector =
		(struct wlr_drm_connector *)output;

	struct wlr_drm_lease_device_v1 *device = NULL;
	wl_list_for_each(device, &manager->devices, link) {
		struct wlr_drm_backend *backend = get_drm_backend_from_backend(
				device->backend);
		if (backend == drm_connector->backend) {
			break;
		}
	}

	if (!device) {
		wlr_log(WLR_ERROR, "No wlr_drm_lease_device_v1 associated with the "
				"offered output");
		return false;
	}

	/*
	 * When the compositor grants a lease, we "destroy" all of the outputs on
	 * that lease. When the lease ends, the outputs re-appear. However, the
	 * underlying DRM connector remains the same. If the compositor offers
	 * outputs based on some criteria, then sees the output re-appear with the
	 * same critera, this code allows it to safely re-offer outputs which are
	 * backed by DRM connectors it has leased in the past.
	 */
	struct wlr_drm_lease_connector_v1 *connector;
	wl_list_for_each(connector, &device->connectors, link) {

		struct wlr_drm_connector *drm_conn =
			(struct wlr_drm_connector *)connector->output;
		if (drm_conn == drm_connector) {
			return false;
		}
	}

	connector = calloc(1, sizeof(struct wlr_drm_lease_connector_v1));
	if (!connector) {
		wlr_log(WLR_ERROR, "Failed to allocatr wlr_drm_lease_connector_v1");
		return false;
	}

	connector->output = output;
	wl_list_init(&connector->resources);
	wl_list_insert(&device->connectors, &connector->link);

	struct wl_resource *resource;
	wl_resource_for_each(resource, &device->resources) {
		drm_lease_connector_v1_send_to_client(
				connector, wl_resource_get_client(resource), resource);
	}

	return true;
}

void wlr_drm_lease_manager_withdraw_output(
	struct wlr_drm_lease_manager *manager, struct wlr_output *output) {
	assert(manager && output);
	assert(wlr_output_is_drm(output));

	struct wlr_drm_connector *drm_connector =
		(struct wlr_drm_connector *)output;

	struct wlr_drm_lease_device_v1 *device = NULL;
	wl_list_for_each(device, &manager->devices, link) {
		struct wlr_drm_backend *backend = get_drm_backend_from_backend(
				device->backend);
		if (backend == drm_connector->backend) {
			break;
		}
	}

	if (!device) {
		wlr_log(WLR_ERROR, "No wlr_drm_lease_device_v1 associated with the "
				"given output");
		return;
	}


	struct wlr_drm_lease_connector_v1 *connector = NULL, *_connector;
	wl_list_for_each(_connector, &device->connectors, link) {
		if (_connector->output == output) {
			connector = _connector;
			break;
		}
	}
	if (!connector) {
		return;
	}
	assert(connector->active_lease == NULL && "Cannot withdraw a leased output");

	struct wl_resource *wl_resource, *temp;
	wl_resource_for_each_safe(wl_resource, temp, &connector->resources) {
		wp_drm_lease_connector_v1_send_withdrawn(wl_resource);
		wl_resource_set_user_data(wl_resource, NULL);
		wl_list_remove(wl_resource_get_link(wl_resource));
		wl_list_init(wl_resource_get_link(wl_resource));
	}

	wl_resource_for_each(wl_resource, &device->requests) {
		struct wlr_drm_lease_request_v1 *request =
			wlr_drm_lease_request_v1_from_resource(wl_resource);
		request->invalid = true;
	}

	wl_list_remove(&connector->link);
	wl_list_init(&connector->link);
	free(connector);
}

static void handle_backend_destroy(struct wl_listener *listener, void *data) {
	struct wlr_drm_lease_device_v1 *device =
		wl_container_of(listener, device, backend_destroy);
	struct wl_resource *resource;
	struct wl_resource *tmp_resource;
	wl_resource_for_each_safe(resource, tmp_resource, &device->resources) {
		wl_resource_destroy(resource);
	}

	wl_resource_for_each_safe(resource, tmp_resource, &device->requests) {
		wl_resource_destroy(resource);
	}

	wl_resource_for_each_safe(resource, tmp_resource, &device->leases) {
		struct wlr_drm_lease_v1 *lease =
			wlr_drm_lease_v1_from_resource(resource);
		wlr_drm_lease_v1_revoke(lease);
	}

	struct wlr_drm_lease_connector_v1 *connector, *tmp_connector;
	wl_list_for_each_safe(connector, tmp_connector,
			&device->connectors, link) {
		wl_list_remove(&connector->link);
		wl_list_init(&connector->link);
		free(connector);
	}

	free(device);
}

struct wlr_drm_lease_device_v1 *drm_lease_device_v1_create(
		struct wl_display *display, struct wlr_backend *backend) {
	assert(display && backend);

	struct wlr_drm_lease_device_v1 *lease_device =
		calloc(1, sizeof(struct wlr_drm_lease_device_v1));

	if (!lease_device) {
		return NULL;
	}

	lease_device->backend = backend;
	wl_list_init(&lease_device->resources);
	wl_list_init(&lease_device->connectors);
	wl_list_init(&lease_device->requests);
	wl_list_init(&lease_device->leases);
	wl_list_init(&lease_device->link);

	lease_device->backend_destroy.notify = handle_backend_destroy;

	wl_signal_add(&backend->events.destroy, &lease_device->backend_destroy);

	lease_device->global = wl_global_create(display,
		&wp_drm_lease_device_v1_interface, 1,
		lease_device, lease_device_bind);

	if (!lease_device->global) {
		free(lease_device);
		return NULL;
	}

	return lease_device;
}

struct multi_backend_data {
	struct wl_display *display;
	struct wlr_drm_lease_manager *manager;
};

static void multi_backend_cb(struct wlr_backend *backend, void *data) {
	struct multi_backend_data *backend_data = data;
	if (!wlr_backend_is_drm(backend)) {
		return;
	}

	wlr_log(WLR_DEBUG, "Adding DRM backend to wlr_drm_lease_manager");

	struct wlr_drm_lease_device_v1 *device = drm_lease_device_v1_create(
			backend_data->display, backend);

	device->manager = backend_data->manager;

	wl_list_insert(&backend_data->manager->devices, &device->link);
}

struct wlr_drm_lease_manager *wlr_drm_lease_manager_create(
		struct wl_display *display, struct wlr_backend *backend) {
	struct wlr_drm_lease_manager *manager = calloc(1,
			sizeof(struct wlr_drm_lease_manager));
	if (!manager) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_drm_lease_manager");
		return NULL;
	}

	wl_signal_init(&manager->events.request);

	wl_list_init(&manager->devices);

	if (wlr_backend_is_multi(backend)) {
		struct multi_backend_data data = {
			.display = display,
			.manager = manager,
		};
		wlr_multi_for_each_backend(backend, multi_backend_cb, &data);
	} else if (wlr_backend_is_drm(backend)) {
		wlr_log(WLR_DEBUG, "Adding single DRM backend to wlr_drm_lease_manager");
		struct wlr_drm_lease_device_v1 *device = drm_lease_device_v1_create(
			display, backend);
		wl_list_insert(&manager->devices, &device->link);
	} else {
		wlr_log(WLR_ERROR, "No DRM backend supplied, failed to create "
				"wlr_drm_lease_manager");
		free(manager);
		return NULL;
	}

	return manager;
}
