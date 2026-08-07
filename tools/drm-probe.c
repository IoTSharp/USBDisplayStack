// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

static uint64_t pointer_to_u64(const void *pointer)
{
	return (uint64_t)(uintptr_t)pointer;
}

static int load_resources(int descriptor, struct drm_mode_card_res *resources,
			  uint32_t **connector_ids)
{
	uint32_t *ids = NULL;
	int result = 0;

	memset(resources, 0, sizeof(*resources));
	if (ioctl(descriptor, DRM_IOCTL_MODE_GETRESOURCES, resources) != 0) {
		result = -errno;
	} else if (resources->count_connectors == 0) {
		result = -ENODEV;
	} else {
		ids = calloc(resources->count_connectors, sizeof(*ids));
		if (ids == NULL) {
			result = -ENOMEM;
		} else {
			resources->connector_id_ptr = pointer_to_u64(ids);
			resources->count_fbs = 0;
			resources->count_crtcs = 0;
			resources->count_encoders = 0;
			if (ioctl(descriptor, DRM_IOCTL_MODE_GETRESOURCES,
				  resources) != 0) {
				result = -errno;
			} else {
				*connector_ids = ids;
			}
		}
	}
	if (result != 0) {
		free(ids);
	}

	return result;
}

static int probe_connector(int descriptor, uint32_t connector_id,
			   unsigned int expected_width,
			   unsigned int expected_height)
{
	struct drm_mode_get_connector connector;
	struct drm_mode_modeinfo *modes = NULL;
	uint32_t index;
	int found = 0;
	int result = 0;

	memset(&connector, 0, sizeof(connector));
	connector.connector_id = connector_id;
	if (ioctl(descriptor, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0) {
		result = -errno;
	} else if (connector.count_modes == 0) {
		printf("connector=%u connection=%u modes=0\n", connector_id,
		       connector.connection);
	} else {
		modes = calloc(connector.count_modes, sizeof(*modes));
		if (modes == NULL) {
			result = -ENOMEM;
		} else {
			connector.modes_ptr = pointer_to_u64(modes);
			connector.count_props = 0;
			connector.count_encoders = 0;
			if (ioctl(descriptor, DRM_IOCTL_MODE_GETCONNECTOR,
				  &connector) != 0) {
				result = -errno;
			} else {
				printf("connector=%u connection=%u modes=%u\n",
				       connector_id, connector.connection,
				       connector.count_modes);
				for (index = 0; index < connector.count_modes; ++index) {
					printf("  mode=%ux%u name=%s type=0x%x\n",
					       modes[index].hdisplay,
					       modes[index].vdisplay,
					       modes[index].name,
					       modes[index].type);
					if ((expected_width == 0 ||
					     modes[index].hdisplay == expected_width) &&
					    (expected_height == 0 ||
					     modes[index].vdisplay == expected_height)) {
						found = 1;
					}
				}
			}
		}
	}
	if (result == 0 && found == 0) {
		result = -ENOMSG;
	}
	free(modes);

	return result;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/card1";
	struct drm_mode_card_res resources;
	uint32_t *connector_ids = NULL;
	unsigned int expected_width = 0;
	unsigned int expected_height = 0;
	uint32_t index;
	int descriptor = -1;
	int connector_result;
	int result = 0;

	if (argc == 4) {
		expected_width = (unsigned int)strtoul(argv[2], NULL, 10);
		expected_height = (unsigned int)strtoul(argv[3], NULL, 10);
	} else if (argc != 1 && argc != 2) {
		fprintf(stderr, "Usage: %s [CARD [WIDTH HEIGHT]]\n", argv[0]);
		result = -EINVAL;
	}
	if (result == 0) {
		descriptor = open(path, O_RDWR | O_CLOEXEC);
		if (descriptor < 0) {
			result = -errno;
		}
	}
	if (result == 0) {
		result = load_resources(descriptor, &resources, &connector_ids);
	}
	if (result == 0) {
		printf("card=%s connectors=%u min=%ux%u max=%ux%u\n", path,
		       resources.count_connectors, resources.min_width,
		       resources.min_height, resources.max_width,
		       resources.max_height);
		result = -ENODEV;
		for (index = 0; index < resources.count_connectors; ++index) {
			connector_result = probe_connector(descriptor,
				connector_ids[index], expected_width,
				expected_height);
			if (connector_result == 0) {
				result = 0;
			}
		}
	}

	free(connector_ids);
	if (descriptor >= 0) {
		close(descriptor);
	}
	if (result != 0) {
		fprintf(stderr, "drm-probe: %s\n", strerror(-result));
	}

	return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
