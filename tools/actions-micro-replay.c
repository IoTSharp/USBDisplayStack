// SPDX-License-Identifier: GPL-2.0-only

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define ACTIONS_MICRO_VID 0x185b
#define ACTIONS_MICRO_PID 0x2d1d
#define REPLAY_VERSION 1U
#define HID_REPORT_LENGTH 4096U
#define MAX_REPLAY_RECORDS 10000000U

struct replay_header {
	unsigned char magic[8];
	uint32_t version;
	uint32_t record_count;
} __attribute__((packed));

struct replay_record {
	uint64_t timestamp_us;
	uint8_t endpoint;
	uint8_t report_id;
	uint16_t reserved;
	uint32_t length;
} __attribute__((packed));

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [--dry-run] [--speed N] [--max-seconds N] "
		"REPLAY [HID_INPUT0 HID_INPUT1]\n",
		program);
}

static int read_exact(FILE *stream, void *buffer, size_t length)
{
	unsigned char *bytes = buffer;
	size_t total = 0;
	size_t count;
	int result = 0;

	while (result == 0 && total < length) {
		count = fread(bytes + total, 1, length - total, stream);
		if (count == 0) {
			result = -EIO;
		} else {
			total += count;
		}
	}

	return result;
}

static uint64_t monotonic_microseconds(void)
{
	struct timespec value;
	uint64_t result = 0;

	if (clock_gettime(CLOCK_MONOTONIC, &value) == 0) {
		result = (uint64_t)value.tv_sec * 1000000ULL;
		result += (uint64_t)value.tv_nsec / 1000ULL;
	}

	return result;
}

static int sleep_until(uint64_t target_us)
{
	struct timespec delay;
	uint64_t now_us = monotonic_microseconds();
	uint64_t remaining_us;
	int result = 0;

	while (result == 0 && stop_requested == 0 && now_us < target_us) {
		remaining_us = target_us - now_us;
		delay.tv_sec = (time_t)(remaining_us / 1000000ULL);
		delay.tv_nsec = (long)((remaining_us % 1000000ULL) * 1000ULL);
		if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
			result = -errno;
		}
		now_us = monotonic_microseconds();
	}

	return result;
}

static int verify_hidraw(int descriptor, const char *path,
			 int expected_input)
{
	struct hidraw_devinfo info;
	char physical_path[256];
	char expected_suffix[32];
	int result = 0;

	memset(&info, 0, sizeof(info));
	memset(physical_path, 0, sizeof(physical_path));
	snprintf(expected_suffix, sizeof(expected_suffix), "/input%d",
		 expected_input);
	if (ioctl(descriptor, HIDIOCGRAWINFO, &info) != 0) {
		result = -errno;
	} else if (info.vendor != ACTIONS_MICRO_VID ||
		   info.product != ACTIONS_MICRO_PID) {
		fprintf(stderr,
			"%s: refused VID/PID %04hx:%04hx, expected %04x:%04x\n",
			path, info.vendor, info.product, ACTIONS_MICRO_VID,
			ACTIONS_MICRO_PID);
		result = -ENODEV;
	} else if (ioctl(descriptor, HIDIOCGRAWPHYS(sizeof(physical_path)),
			 physical_path) < 0) {
		result = -errno;
	} else if (strstr(physical_path, expected_suffix) == NULL) {
		fprintf(stderr, "%s: expected HID physical suffix %s, got %s\n",
			path, expected_suffix, physical_path);
		result = -ENODEV;
	} else {
		fprintf(stderr, "verified %s as %04hx:%04hx %s\n", path,
			info.vendor, info.product, physical_path);
	}

	return result;
}

static int write_report(int descriptor, const unsigned char *report)
{
	ssize_t written = -1;
	int result = 0;

	while (written < 0 && result == 0) {
		written = write(descriptor, report, HID_REPORT_LENGTH);
		if (written < 0 && errno != EINTR) {
			result = -errno;
		}
	}
	if (result == 0 && written != (ssize_t)HID_REPORT_LENGTH) {
		result = -EIO;
	}

	return result;
}

static int parse_positive_double(const char *text, double *value)
{
	char *end = NULL;
	double parsed;
	int result = 0;

	errno = 0;
	parsed = strtod(text, &end);
	if (errno != 0 || end == text || *end != '\0' || parsed <= 0.0) {
		result = -EINVAL;
	} else {
		*value = parsed;
	}

	return result;
}

static int parse_arguments(int argc, char **argv, int *dry_run,
			   double *speed, double *max_seconds,
			   const char **replay_path,
			   const char **hid_input0,
			   const char **hid_input1)
{
	int positional = 0;
	int index;
	int result = 0;

	for (index = 1; index < argc && result == 0; ++index) {
		if (strcmp(argv[index], "--dry-run") == 0) {
			*dry_run = 1;
		} else if ((strcmp(argv[index], "--speed") == 0 ||
			    strcmp(argv[index], "--max-seconds") == 0) &&
			   index + 1 < argc) {
			if (strcmp(argv[index], "--speed") == 0) {
				result = parse_positive_double(argv[++index], speed);
			} else {
				result = parse_positive_double(argv[++index],
						       max_seconds);
			}
		} else if (argv[index][0] == '-') {
			result = -EINVAL;
		} else {
			if (positional == 0) {
				*replay_path = argv[index];
			} else if (positional == 1) {
				*hid_input0 = argv[index];
			} else if (positional == 2) {
				*hid_input1 = argv[index];
			} else {
				result = -EINVAL;
			}
			++positional;
		}
	}
	if (result == 0 && (*replay_path == NULL ||
	    (*dry_run == 0 && (*hid_input0 == NULL || *hid_input1 == NULL)))) {
		result = -EINVAL;
	}

	return result;
}

int main(int argc, char **argv)
{
	static const unsigned char replay_magic[8] = {
		'D', 'P', 'R', 'P', 'L', '0', '0', '1'
	};
	const char *replay_path = NULL;
	const char *hid_input0 = NULL;
	const char *hid_input1 = NULL;
	struct replay_header header;
	struct replay_record record;
	unsigned char report[HID_REPORT_LENGTH];
	FILE *stream = NULL;
	double speed = 1.0;
	double max_seconds = 0.0;
	uint64_t replay_start_us = 0;
	uint64_t last_timestamp_us = 0;
	uint32_t processed = 0;
	uint32_t commands = 0;
	uint32_t video = 0;
	int dry_run = 0;
	int descriptor0 = -1;
	int descriptor1 = -1;
	int target_descriptor;
	int expected_report_id;
	int result;

	memset(&header, 0, sizeof(header));
	result = parse_arguments(argc, argv, &dry_run, &speed, &max_seconds,
				 &replay_path,
				 &hid_input0, &hid_input1);
	if (result != 0) {
		print_usage(argv[0]);
	} else {
		stream = fopen(replay_path, "rb");
		if (stream == NULL) {
			result = -errno;
		}
	}
	if (result == 0) {
		result = read_exact(stream, &header, sizeof(header));
		if (result == 0 &&
		    (memcmp(header.magic, replay_magic, sizeof(replay_magic)) != 0 ||
		     header.version != REPLAY_VERSION ||
		     header.record_count > MAX_REPLAY_RECORDS)) {
			result = -EPROTO;
		}
	}
	if (result == 0 && dry_run == 0) {
		descriptor0 = open(hid_input0, O_RDWR | O_CLOEXEC);
		descriptor1 = open(hid_input1, O_RDWR | O_CLOEXEC);
		if (descriptor0 < 0 || descriptor1 < 0) {
			result = -errno;
		} else {
			result = verify_hidraw(descriptor0, hid_input0, 0);
			if (result == 0) {
				result = verify_hidraw(descriptor1, hid_input1, 1);
			}
		}
		if (result == 0) {
			replay_start_us = monotonic_microseconds();
		}
	}
	if (result == 0) {
		signal(SIGINT, handle_signal);
		signal(SIGTERM, handle_signal);
	}
	while (result == 0 && stop_requested == 0 &&
	       processed < header.record_count) {
		memset(&record, 0, sizeof(record));
		result = read_exact(stream, &record, sizeof(record));
		if (result == 0 && record.length != HID_REPORT_LENGTH) {
			result = -EPROTO;
		}
		if (result == 0) {
			result = read_exact(stream, report, sizeof(report));
		}
		if (record.endpoint == 0x03) {
			expected_report_id = 2;
			target_descriptor = descriptor0;
		} else if (record.endpoint == 0x04) {
			expected_report_id = 1;
			target_descriptor = descriptor1;
		} else {
			expected_report_id = -1;
			target_descriptor = -1;
		}
		if (result == 0 &&
		    (expected_report_id < 0 || record.report_id != expected_report_id ||
		     report[0] != expected_report_id ||
		     (report[1] != 1 && report[1] != 2))) {
			result = -EPROTO;
		}
		if (result == 0 && max_seconds > 0.0 &&
		    (double)record.timestamp_us > max_seconds * 1000000.0) {
			stop_requested = 1;
		}
		if (result == 0 && stop_requested == 0 && dry_run == 0) {
			result = sleep_until(replay_start_us +
				(uint64_t)((double)record.timestamp_us / speed));
			if (result == 0 && stop_requested == 0) {
				result = write_report(target_descriptor, report);
			}
		}
		if (result == 0 && stop_requested == 0) {
			++processed;
			last_timestamp_us = record.timestamp_us;
			if (report[1] == 1) {
				++commands;
			} else {
				++video;
			}
		}
	}
	if (result == 0) {
		printf("%s: records=%u commands=%u video=%u duration=%.3f s\n",
		       dry_run != 0 ? "dry-run complete" : "replay complete",
		       processed, commands, video,
		       (double)last_timestamp_us / 1000000.0);
	}

	if (descriptor1 >= 0) {
		close(descriptor1);
	}
	if (descriptor0 >= 0) {
		close(descriptor0);
	}
	if (stream != NULL) {
		fclose(stream);
	}
	if (result != 0) {
		fprintf(stderr, "actions-micro-replay: %s\n", strerror(-result));
	}

	return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
