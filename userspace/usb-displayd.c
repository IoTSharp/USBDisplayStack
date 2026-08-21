// SPDX-License-Identifier: GPL-2.0-only

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <usbdisplay/backend.h>
#include <usbdisplay/uapi.h>

#define DEFAULT_DEVICE "/dev/usbdisplay0"
#define BACKEND_TICK_INTERVAL_MS 100
#define DEFAULT_BACKEND_RETRY_MS 2000U
#define DEFAULT_READY_FILE "/run/usbdisplay/ready"
#define IDLE_FRAME_REFRESH_NS 2000000000ULL

static volatile sig_atomic_t stop_requested;

static uint64_t monotonic_nanoseconds(void)
{
	struct timespec value;
	uint64_t result = 0;

	if (clock_gettime(CLOCK_MONOTONIC, &value) == 0) {
		result = (uint64_t)value.tv_sec * 1000000000ULL;
		result += (uint64_t)value.tv_nsec;
	}

	return result;
}

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --backend PATH [--device PATH] "
		"[--backend-option VALUE] [--ready-file PATH] [--retry-ms N]\n",
		program);
}

static int parse_arguments(int argc, char **argv, const char **device_path,
			   const char **backend_path, const char **backend_option,
			   const char **ready_file, unsigned int *retry_ms)
{
	int index;
	int result = 0;
	char *end = NULL;
	unsigned long parsed;

	*device_path = DEFAULT_DEVICE;
	*backend_path = NULL;
	*backend_option = NULL;
	*ready_file = DEFAULT_READY_FILE;
	*retry_ms = DEFAULT_BACKEND_RETRY_MS;
	for (index = 1; index < argc && result == 0; ++index) {
		if (strcmp(argv[index], "--device") == 0 && index + 1 < argc) {
			*device_path = argv[++index];
		} else if (strcmp(argv[index], "--backend") == 0 &&
			   index + 1 < argc) {
			*backend_path = argv[++index];
		} else if (strcmp(argv[index], "--backend-option") == 0 &&
			   index + 1 < argc) {
			*backend_option = argv[++index];
		} else if (strcmp(argv[index], "--ready-file") == 0 &&
			   index + 1 < argc) {
			*ready_file = argv[++index];
		} else if (strcmp(argv[index], "--retry-ms") == 0 &&
			   index + 1 < argc) {
			errno = 0;
			parsed = strtoul(argv[++index], &end, 10);
			if (errno != 0 || end == argv[index] || *end != '\0' ||
			    parsed == 0 || parsed > 60000U) {
				result = -EINVAL;
			} else {
				*retry_ms = (unsigned int)parsed;
			}
		} else {
			result = -EINVAL;
		}
	}
	if (*backend_path == NULL) {
		result = -EINVAL;
	}

	return result;
}

/* 物理后端断开时保持一个进程等待，避免 systemd 每两秒重复创建守护进程。 */
static int sleep_milliseconds(unsigned int milliseconds)
{
	struct timespec delay;
	int result = 0;

	delay.tv_sec = (time_t)(milliseconds / 1000U);
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	while (!stop_requested && result == 0 && nanosleep(&delay, &delay) != 0) {
		if (errno != EINTR) {
			result = -errno;
		}
	}

	return result;
}

static int write_all(int descriptor, const char *buffer, size_t length)
{
	size_t written_total = 0;
	ssize_t written;
	int result = 0;

	while (written_total < length && result == 0) {
		written = write(descriptor, buffer + written_total,
				length - written_total);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written < 0) {
			result = -errno;
		} else if (written == 0) {
			result = -EIO;
		} else {
			written_total += (size_t)written;
		}
	}

	return result;
}

/* 就绪文件同时携带 PID 和传输代数，副屏可识别快速重连并拒绝陈旧标记。 */
static int publish_ready_file(const char *ready_file, const char *backend_name,
			      bool physical, unsigned long long generation)
{
	char contents[256];
	char temporary_file[320];
	int descriptor = -1;
	int length;
	int temporary_length;
	int result = 0;

	if (ready_file != NULL && ready_file[0] != '\0') {
		temporary_length = snprintf(temporary_file, sizeof(temporary_file),
					"%s.tmp.%ld", ready_file, (long)getpid());
		if (temporary_length < 0 ||
		    (size_t)temporary_length >= sizeof(temporary_file)) {
			result = -ENAMETOOLONG;
		}
		if (result == 0) {
			descriptor = open(temporary_file, O_WRONLY | O_CREAT | O_TRUNC |
					  O_CLOEXEC, 0644);
		}
		if (result == 0 && descriptor < 0) {
			result = -errno;
		}
		if (result == 0) {
			length = snprintf(contents, sizeof(contents),
					"pid=%ld\ngeneration=%llu\nbackend=%s\n"
					"physical=%u\n",
					(long)getpid(), generation,
					backend_name != NULL ? backend_name : "unknown",
					physical ? 1U : 0U);
			if (length < 0 || (size_t)length >= sizeof(contents)) {
				result = -EOVERFLOW;
			} else {
				result = write_all(descriptor, contents, (size_t)length);
			}
			if (close(descriptor) != 0 && result == 0) {
				result = -errno;
			}
			if (result == 0 && rename(temporary_file, ready_file) != 0) {
				result = -errno;
			}
			if (result != 0) {
				(void)unlink(temporary_file);
			}
		}
	}

	return result;
}

static void remove_ready_file(const char *ready_file)
{
	if (ready_file != NULL && ready_file[0] != '\0') {
		(void)unlink(ready_file);
	}
}

/* 后端重开后先释放旧 mmap 再 close/open 帧流，避免映射引用使内核返回 EBUSY。 */
static int reopen_frame_stream(const char *device_path, size_t map_bytes,
			       int *device_fd, void **mapping)
{
	int descriptor = -1;
	void *new_mapping = MAP_FAILED;
	int result = 0;

	if (*mapping != MAP_FAILED) {
		if (munmap(*mapping, map_bytes) != 0) {
			result = -errno;
		} else {
			*mapping = MAP_FAILED;
		}
	}
	if (result == 0 && *device_fd >= 0) {
		(void)close(*device_fd);
		*device_fd = -1;
	}
	if (result == 0) {
		descriptor = open(device_path, O_RDONLY | O_CLOEXEC);
		if (descriptor < 0) {
			result = -errno;
		}
	}
	if (result == 0) {
		new_mapping = mmap(NULL, map_bytes, PROT_READ, MAP_SHARED,
				   descriptor, 0);
		if (new_mapping == MAP_FAILED) {
			result = -errno;
		}
	}
	if (result == 0) {
		*device_fd = descriptor;
		*mapping = new_mapping;
		descriptor = -1;
	}
	if (descriptor >= 0) {
		(void)close(descriptor);
	}

	return result;
}

static int validate_update(const struct usbdisplay_device_info *info,
			   const struct usbdisplay_update *update)
{
	size_t frame_bytes;
	int result = 0;

	if (update->slot >= info->slot_count || update->width == 0 ||
	    update->height == 0 || update->width > info->width ||
	    update->height > info->height) {
		result = -EPROTO;
	} else if (update->stride > info->slot_bytes / update->height) {
		result = -EOVERFLOW;
	} else {
		frame_bytes = (size_t)update->stride * update->height;
		if (frame_bytes > info->slot_bytes) {
			result = -EOVERFLOW;
		}
	}

	return result;
}

static int run_loop(int device_fd, const struct usbdisplay_device_info *info,
			    const void *mapping,
			    const struct usbdisplay_backend_v1 *backend,
			    void *backend_context)
{
	struct pollfd descriptor;
	struct usbdisplay_update update;
	struct usbdisplay_frame frame;
	ssize_t bytes_read;
	int poll_result;
	uint64_t now_ns = 0;
	uint64_t last_submit_ns = 0;
	bool frame_valid = false;
	int result = 0;

	descriptor.fd = device_fd;
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	while (!stop_requested && result == 0) {
		poll_result = poll(&descriptor, 1, BACKEND_TICK_INTERVAL_MS);
		if (poll_result < 0) {
			if (errno != EINTR) {
				result = -errno;
			}
		} else if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) {
			bytes_read = read(device_fd, &update, sizeof(update));
			if (bytes_read < 0) {
				if (errno != EINTR) {
					result = -errno;
				}
			} else if ((size_t)bytes_read != sizeof(update)) {
				result = -EPROTO;
			} else {
				result = validate_update(info, &update);
				if (result == 0) {
					memset(&frame, 0, sizeof(frame));
					frame.struct_size = sizeof(frame);
					frame.pixels = (const char *)mapping +
						       ((size_t)update.slot * info->slot_bytes);
					frame.bytes = (size_t)update.stride * update.height;
					frame.sequence = update.sequence;
					frame.timestamp_ns = update.timestamp_ns;
					frame.width = update.width;
					frame.height = update.height;
					frame.stride = update.stride;
					frame.format = update.format;
					frame.source = update.source;
					frame.damage_x = update.damage_x;
					frame.damage_y = update.damage_y;
					frame.damage_width = update.damage_width;
					frame.damage_height = update.damage_height;
					result = backend->submit(backend_context, &frame);
					if (result == 0) {
						frame_valid = true;
						last_submit_ns = monotonic_nanoseconds();
					}
				}
			}
		} else if (poll_result > 0 &&
			   (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			result = -EIO;
		}
		now_ns = monotonic_nanoseconds();
		/* 静止界面也每两秒重交最新帧，避免固件因长期无视频退回等待页。 */
		if (result == 0 && frame_valid &&
		    (backend->capabilities & USBDISPLAY_BACKEND_CAP_PHYSICAL) != 0 &&
		    now_ns >= last_submit_ns &&
		    now_ns - last_submit_ns >= IDLE_FRAME_REFRESH_NS) {
			result = backend->submit(backend_context, &frame);
			if (result == 0) {
				last_submit_ns = monotonic_nanoseconds();
			}
		}
		if (result == 0 &&
		    (backend->capabilities & USBDISPLAY_BACKEND_CAP_TICK) != 0 &&
		    backend->struct_size >= USBDISPLAY_BACKEND_V1_TICK_SIZE &&
		    backend->tick != NULL) {
			/* submit 可能发送心跳，tick 必须使用提交完成后的时间记录设备响应。 */
			now_ns = monotonic_nanoseconds();
			result = backend->tick(backend_context, now_ns);
		}
	}

	return result;
}

int main(int argc, char **argv)
{
	const char *device_path;
	const char *backend_path;
	const char *backend_option;
	const char *ready_file;
	unsigned int retry_ms;
	struct usbdisplay_device_info info;
	struct usbdisplay_backend_config backend_config;
	usbdisplay_backend_entry_fn backend_entry;
	const struct usbdisplay_backend_v1 *backend = NULL;
	void *backend_library = NULL;
	void *backend_context = NULL;
	void *mapping = MAP_FAILED;
	int device_fd = -1;
	int result;
	int open_result;
	int loop_result;
	const char *open_stage = "backend";
	unsigned int retry_count = 0U;
	unsigned long long reopen_count = 0ULL;
	unsigned long long generation = 1ULL;
	/* 仅统计 run_loop 报告传输丢失之后完整成功的重开。 */
	bool reopen_pending = false;

	result = parse_arguments(argc, argv, &device_path, &backend_path,
				 &backend_option, &ready_file, &retry_ms);
	if (result != 0) {
		print_usage(argv[0]);
	} else {
		backend_library = dlopen(backend_path, RTLD_NOW | RTLD_LOCAL);
		if (backend_library == NULL) {
			fprintf(stderr, "usb-displayd: dlopen: %s\n", dlerror());
			result = -ENOENT;
		}
	}
	if (result == 0) {
		dlerror();
		backend_entry = (usbdisplay_backend_entry_fn)dlsym(
			backend_library, USBDISPLAY_BACKEND_ENTRY);
		if (backend_entry == NULL || dlerror() != NULL) {
			fprintf(stderr, "usb-displayd: backend entry is missing\n");
			result = -EPROTO;
		} else {
			backend = backend_entry();
			if (backend == NULL ||
			    backend->abi_version != USBDISPLAY_BACKEND_ABI_VERSION ||
			    backend->struct_size <
				    USBDISPLAY_BACKEND_V1_REQUIRED_SIZE ||
			    backend->open == NULL || backend->submit == NULL ||
			    backend->close == NULL) {
				fprintf(stderr, "usb-displayd: incompatible backend ABI\n");
				result = -EPROTO;
			} else if ((backend->capabilities &
				    USBDISPLAY_BACKEND_CAP_TICK) != 0 &&
				   (backend->struct_size <
				    USBDISPLAY_BACKEND_V1_TICK_SIZE ||
				    backend->tick == NULL)) {
				fprintf(stderr,
					"usb-displayd: backend tick capability is invalid\n");
				result = -EPROTO;
			}
		}
	}
	if (result == 0) {
		device_fd = open(device_path, O_RDONLY | O_CLOEXEC);
		if (device_fd < 0) {
			result = -errno;
		}
	}
	if (result == 0) {
		memset(&info, 0, sizeof(info));
		if (ioctl(device_fd, USBDISPLAY_IOCTL_GET_INFO, &info) != 0) {
			result = -errno;
		} else if (info.abi_version != USBDISPLAY_ABI_VERSION ||
			   info.slot_count == 0 || info.slot_bytes == 0 ||
			   info.map_bytes != info.slot_count * info.slot_bytes) {
			result = -EPROTO;
		}
	}
	if (result == 0) {
		mapping = mmap(NULL, info.map_bytes, PROT_READ, MAP_SHARED,
			       device_fd, 0);
		if (mapping == MAP_FAILED) {
			result = -errno;
		}
	}
	if (result == 0) {
		memset(&backend_config, 0, sizeof(backend_config));
		backend_config.struct_size = sizeof(backend_config);
		backend_config.option = backend_option;
		backend_config.device_width = info.width;
		backend_config.device_height = info.height;
		signal(SIGINT, handle_signal);
		signal(SIGTERM, handle_signal);
		signal(SIGPIPE, SIG_IGN);
		fprintf(stderr, "usb-displayd: waiting for %s (%ums retry)\n",
			backend->name, retry_ms);
		while (result == 0 && !stop_requested) {
			backend_context = NULL;
			open_stage = "backend";
			open_result = backend->open(&backend_config, &backend_context);
			/* 后端重开完成后再重开帧流，使首个 poll 立即收到内核保留的最新帧。 */
			if (open_result == 0 && reopen_pending) {
				open_stage = "frame stream";
				open_result = reopen_frame_stream(device_path,
							  info.map_bytes, &device_fd,
							  &mapping);
			}
			if (open_result != 0) {
				if (retry_count == 0U || retry_count % 5U == 0U) {
					fprintf(stderr,
						"usb-displayd: %s unavailable: %s "
						"reopen_count=%llu\n",
						open_stage, strerror(-open_result),
						reopen_count);
				}
				if (backend_context != NULL) {
					backend->close(backend_context);
					backend_context = NULL;
				}
				retry_count += 1U;
				result = sleep_milliseconds(retry_ms);
			} else {
				retry_count = 0U;
				if (reopen_pending) {
					++reopen_count;
					reopen_pending = false;
				}
				generation = reopen_count + 1ULL;
				result = publish_ready_file(ready_file, backend->name,
						(backend->capabilities &
						 USBDISPLAY_BACKEND_CAP_PHYSICAL) != 0,
						generation);
				if (result != 0) {
					fprintf(stderr,
						"usb-displayd: cannot publish readiness: %s\n",
						strerror(-result));
					backend->close(backend_context);
					backend_context = NULL;
				} else {
					fprintf(stderr,
						"usb-displayd: %s -> %s (%ux%u) ready\n",
						device_path, backend->name, info.width, info.height);
					fprintf(stderr,
						"usb-displayd: transport open generation=%llu "
						"reopen_count=%llu\n",
						generation, reopen_count);
					loop_result = run_loop(device_fd, &info, mapping, backend,
						backend_context);
					remove_ready_file(ready_file);
					backend->close(backend_context);
					backend_context = NULL;
					if (loop_result != 0 && !stop_requested) {
						reopen_pending = true;
						fprintf(stderr,
							"usb-displayd: transport lost: %s\n",
							strerror(-loop_result));
						fprintf(stderr,
							"usb-displayd: waiting to reopen "
							"reopen_count=%llu retry_count=%u\n",
							reopen_count, retry_count);
						result = sleep_milliseconds(retry_ms);
					} else {
						result = 0;
					}
				}
			}
		}
		remove_ready_file(ready_file);
		if (stop_requested) {
			result = 0;
		}
	}

	if (backend != NULL && backend_context != NULL) {
		backend->close(backend_context);
	}
	if (mapping != MAP_FAILED) {
		munmap(mapping, info.map_bytes);
	}
	if (device_fd >= 0) {
		close(device_fd);
	}
	if (backend_library != NULL) {
		dlclose(backend_library);
	}
	if (result != 0) {
		fprintf(stderr, "usb-displayd: %s\n", strerror(-result));
	}

	return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
