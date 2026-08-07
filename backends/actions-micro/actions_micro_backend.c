// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <usbdisplay/backend.h>

#define ACTIONS_MICRO_VID 0x185b
#define ACTIONS_MICRO_PID 0x2d1d
#define REPLAY_VERSION 1U
#define HID_REPORT_LENGTH 4096U
#define HID_HEADER_LENGTH 14U
#define HID_PAYLOAD_LENGTH (HID_REPORT_LENGTH - HID_HEADER_LENGTH)
#define INNER_HEADER_LENGTH 32U
#define MAX_REPLAY_RECORDS 10000000U
#define MAX_COMMAND_REPORTS 10000U
#define MAX_BOOTSTRAP_REPORTS 16384U
#define MAX_ENCODED_BYTES (32U * 1024U * 1024U)
#define DEFAULT_FPS 30U
#define DEFAULT_FRAGMENT_INTERVAL_US 500U
#define DEFAULT_ENCODE_TIMEOUT_MS 2000U
#define DEFAULT_HEARTBEAT_US 1000000ULL
#define MAX_HEARTBEATS_PER_TICK 4U

#define VIDEO_CONFIG 0x800U
#define VIDEO_IDR 0x200U
#define VIDEO_P 0x400U

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

struct stored_report {
	uint64_t timestamp_us;
	uint8_t endpoint;
	unsigned char data[HID_REPORT_LENGTH];
};

struct actions_options {
	char *template_path;
	char *hid0_path;
	char *hid1_path;
	char *encoder;
	unsigned int fps;
	unsigned int fragment_interval_us;
	unsigned int encode_timeout_ms;
	bool full_bootstrap;
};

struct actions_encoder {
	pid_t process;
	int input_fd;
	int output_fd;
	uint32_t format;
	unsigned char *packed_frame;
	size_t packed_capacity;
	unsigned char *encoded;
	size_t encoded_capacity;
};

struct actions_context {
	struct actions_options options;
	struct actions_encoder encoder;
	struct stored_report *initialization;
	size_t initialization_count;
	struct stored_report *heartbeats;
	size_t heartbeat_count;
	size_t heartbeat_index;
	struct stored_report *bootstrap;
	size_t bootstrap_count;
	uint64_t video_start_us;
	uint64_t heartbeat_period_us;
	uint64_t next_heartbeat_ns;
	uint32_t command_sequence;
	uint32_t video_sequence;
	uint64_t video_report_index;
	uint8_t next_command_endpoint;
	uint64_t frames;
	uint64_t reports;
	uint32_t width;
	uint32_t height;
	int hid[2];
};

struct byte_buffer {
	unsigned char *data;
	size_t length;
	size_t capacity;
};

static uint16_t read_le16(const unsigned char *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const unsigned char *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_le16(unsigned char *data, uint16_t value)
{
	data[0] = (unsigned char)value;
	data[1] = (unsigned char)(value >> 8);
}

static void write_le32(unsigned char *data, uint32_t value)
{
	data[0] = (unsigned char)value;
	data[1] = (unsigned char)(value >> 8);
	data[2] = (unsigned char)(value >> 16);
	data[3] = (unsigned char)(value >> 24);
}

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

static int sleep_until_ns(uint64_t target_ns)
{
	struct timespec delay;
	uint64_t now_ns = monotonic_nanoseconds();
	uint64_t remaining_ns;
	int result = 0;

	while (result == 0 && now_ns < target_ns) {
		remaining_ns = target_ns - now_ns;
		delay.tv_sec = (time_t)(remaining_ns / 1000000000ULL);
		delay.tv_nsec = (long)(remaining_ns % 1000000000ULL);
		if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
			result = -errno;
		}
		now_ns = monotonic_nanoseconds();
	}

	return result;
}

static int sleep_microseconds(unsigned int microseconds)
{
	struct timespec delay;

	delay.tv_sec = (time_t)(microseconds / 1000000U);
	delay.tv_nsec = (long)((microseconds % 1000000U) * 1000U);
	while (nanosleep(&delay, &delay) != 0) {
		if (errno != EINTR) {
			return -errno;
		}
	}

	return 0;
}

static void free_options(struct actions_options *options)
{
	free(options->encoder);
	free(options->hid1_path);
	free(options->hid0_path);
	free(options->template_path);
	memset(options, 0, sizeof(*options));
}

static int replace_string(char **target, const char *value)
{
	char *copy = strdup(value);

	if (copy == NULL) {
		return -ENOMEM;
	}
	free(*target);
	*target = copy;

	return 0;
}

static int parse_unsigned(const char *text, unsigned int minimum,
			  unsigned int maximum, unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed < minimum ||
	    parsed > maximum) {
		return -EINVAL;
	}
	*value = (unsigned int)parsed;

	return 0;
}

static int parse_option(struct actions_options *options, char *option)
{
	char *equals = strchr(option, '=');
	const char *key;
	const char *value;

	if (equals == NULL || equals == option || equals[1] == '\0') {
		return -EINVAL;
	}
	*equals = '\0';
	key = option;
	value = equals + 1;
	if (strcmp(key, "template") == 0) {
		return replace_string(&options->template_path, value);
	}
	if (strcmp(key, "hid0") == 0) {
		return replace_string(&options->hid0_path, value);
	}
	if (strcmp(key, "hid1") == 0) {
		return replace_string(&options->hid1_path, value);
	}
	if (strcmp(key, "encoder") == 0) {
		return replace_string(&options->encoder, value);
	}
	if (strcmp(key, "fps") == 0) {
		return parse_unsigned(value, 1, 120, &options->fps);
	}
	if (strcmp(key, "fragment-us") == 0) {
		return parse_unsigned(value, 1, 100000,
				      &options->fragment_interval_us);
	}
	if (strcmp(key, "encode-timeout-ms") == 0) {
		return parse_unsigned(value, 100, 30000,
				      &options->encode_timeout_ms);
	}
	if (strcmp(key, "bootstrap") == 0) {
		if (strcmp(value, "full") == 0) {
			options->full_bootstrap = true;
			return 0;
		}
		if (strcmp(value, "none") == 0) {
			options->full_bootstrap = false;
			return 0;
		}
		return -EINVAL;
	}

	return -EINVAL;
}

static int parse_options(const char *text, struct actions_options *options)
{
	char *copy = NULL;
	char *save = NULL;
	char *token;
	int result = 0;

	memset(options, 0, sizeof(*options));
	options->fps = DEFAULT_FPS;
	options->fragment_interval_us = DEFAULT_FRAGMENT_INTERVAL_US;
	options->encode_timeout_ms = DEFAULT_ENCODE_TIMEOUT_MS;
	options->encoder = strdup("ffmpeg");
	if (options->encoder == NULL) {
		result = -ENOMEM;
	} else if (text == NULL || text[0] == '\0') {
		result = -EINVAL;
	} else {
		copy = strdup(text);
		if (copy == NULL) {
			result = -ENOMEM;
		}
	}
	for (token = result == 0 ? strtok_r(copy, ",", &save) : NULL;
	     token != NULL && result == 0;
	     token = strtok_r(NULL, ",", &save)) {
		result = parse_option(options, token);
	}
	if (result == 0 && options->template_path == NULL) {
		result = -EINVAL;
	}
	if (result != 0) {
		fprintf(stderr,
			"actions-micro: backend option requires template=PATH; "
			"optional hid0, hid1, encoder, fps, fragment-us, "
			"encode-timeout-ms, bootstrap=none|full\n");
		free_options(options);
	}
	free(copy);

	return result;
}

static int read_exact(FILE *stream, void *buffer, size_t length)
{
	unsigned char *bytes = buffer;
	size_t total = 0;
	size_t count;

	while (total < length) {
		count = fread(bytes + total, 1, length - total, stream);
		if (count == 0) {
			return ferror(stream) != 0 && errno != 0 ? -errno : -EIO;
		}
		total += count;
	}

	return 0;
}

static int append_command(struct stored_report **reports, size_t *count,
			  const struct replay_record *record,
			  const unsigned char data[HID_REPORT_LENGTH])
{
	struct stored_report *resized;

	if (*count >= MAX_COMMAND_REPORTS) {
		return -E2BIG;
	}
	resized = realloc(*reports, (*count + 1) * sizeof(**reports));
	if (resized == NULL) {
		return -ENOMEM;
	}
	*reports = resized;
	resized[*count].timestamp_us = record->timestamp_us;
	resized[*count].endpoint = record->endpoint;
	memcpy(resized[*count].data, data, HID_REPORT_LENGTH);
	++*count;

	return 0;
}

static int validate_replay_report(const struct replay_record *record,
				  const unsigned char data[HID_REPORT_LENGTH])
{
	int expected_report_id;
	uint32_t fragment_info;
	uint16_t payload_length;

	if (record->endpoint == 0x03) {
		expected_report_id = 2;
	} else if (record->endpoint == 0x04) {
		expected_report_id = 1;
	} else {
		return -EPROTO;
	}
	fragment_info = read_le32(data + 8);
	payload_length = read_le16(data + 12);
	if (record->length != HID_REPORT_LENGTH ||
	    record->report_id != expected_report_id ||
	    data[0] != expected_report_id ||
	    (data[1] != 1 && data[1] != 2) ||
	    (fragment_info & 0xffffU) == 0 ||
	    (fragment_info >> 16) >= (fragment_info & 0xffffU) ||
	    payload_length > HID_PAYLOAD_LENGTH) {
		return -EPROTO;
	}

	return 0;
}

static bool is_heartbeat(const unsigned char data[HID_REPORT_LENGTH])
{
	return data[1] == 1 && read_le16(data + 12) == 34 &&
	       memcmp(data + 18, "_PPA", 4) == 0;
}

static int load_template(struct actions_context *state)
{
	static const unsigned char magic[8] = {
		'D', 'P', 'R', 'P', 'L', '0', '0', '1'
	};
	struct replay_header header;
	struct replay_record record;
	unsigned char data[HID_REPORT_LENGTH];
	FILE *stream = NULL;
	uint64_t previous_timestamp = 0;
	uint32_t index;
	bool video_seen = false;
	int result = 0;

	memset(&header, 0, sizeof(header));
	stream = fopen(state->options.template_path, "rb");
	if (stream == NULL) {
		result = -errno;
	} else {
		result = read_exact(stream, &header, sizeof(header));
	}
	if (result == 0 &&
	    (memcmp(header.magic, magic, sizeof(magic)) != 0 ||
	     header.version != REPLAY_VERSION ||
	     header.record_count == 0 ||
	     header.record_count > MAX_REPLAY_RECORDS)) {
		result = -EPROTO;
	}
	if (result == 0 && state->options.full_bootstrap &&
	    header.record_count > MAX_BOOTSTRAP_REPORTS) {
		result = -E2BIG;
	}
	if (result == 0 && state->options.full_bootstrap) {
		state->bootstrap = calloc(header.record_count,
					  sizeof(*state->bootstrap));
		if (state->bootstrap == NULL) {
			result = -ENOMEM;
		}
	}
	for (index = 0; index < header.record_count && result == 0; ++index) {
		result = read_exact(stream, &record, sizeof(record));
		if (result == 0 && record.length != HID_REPORT_LENGTH) {
			result = -EPROTO;
		}
		if (result == 0) {
			result = read_exact(stream, data, sizeof(data));
		}
		if (result == 0) {
			result = validate_replay_report(&record, data);
		}
		if (result == 0 && index > 0 &&
		    record.timestamp_us < previous_timestamp) {
			result = -EPROTO;
		}
		previous_timestamp = record.timestamp_us;
		if (result == 0 && state->options.full_bootstrap) {
			state->bootstrap[index].timestamp_us = record.timestamp_us;
			state->bootstrap[index].endpoint = record.endpoint;
			memcpy(state->bootstrap[index].data, data,
			       HID_REPORT_LENGTH);
			state->bootstrap_count = (size_t)index + 1;
		}
		if (result == 0 && data[1] == 2 && !video_seen) {
			state->video_start_us = record.timestamp_us;
			video_seen = true;
		} else if (result == 0 && data[1] == 1 && !video_seen) {
			result = append_command(&state->initialization,
						&state->initialization_count,
						&record, data);
		} else if (result == 0 && video_seen && is_heartbeat(data)) {
			result = append_command(&state->heartbeats,
						&state->heartbeat_count,
						&record, data);
		}
	}
	if (result == 0 && fgetc(stream) != EOF) {
		result = -EPROTO;
	}
	if (result == 0 && (!video_seen || state->initialization_count == 0 ||
			    state->heartbeat_count == 0)) {
		result = -ENODATA;
	}
	if (stream != NULL) {
		fclose(stream);
	}
	if (result != 0) {
		fprintf(stderr, "actions-micro: invalid template %s: %s\n",
			state->options.template_path, strerror(-result));
	}

	return result;
}

static int verify_hidraw(int descriptor, int expected_input)
{
	struct hidraw_devinfo info;
	char physical_path[256];
	char expected_suffix[32];

	memset(&info, 0, sizeof(info));
	memset(physical_path, 0, sizeof(physical_path));
	snprintf(expected_suffix, sizeof(expected_suffix), "/input%d",
		 expected_input);
	if (ioctl(descriptor, HIDIOCGRAWINFO, &info) != 0) {
		return -errno;
	}
	if (info.vendor != ACTIONS_MICRO_VID ||
	    info.product != ACTIONS_MICRO_PID) {
		return -ENODEV;
	}
	if (ioctl(descriptor, HIDIOCGRAWPHYS(sizeof(physical_path)),
		  physical_path) < 0) {
		return -errno;
	}
	if (strstr(physical_path, expected_suffix) == NULL) {
		return -ENODEV;
	}

	return 0;
}

static int open_hidraw(const char *configured_path, int expected_input,
		       char path[64])
{
	unsigned int index;
	int descriptor = -1;
	int result = -ENODEV;

	if (configured_path != NULL) {
		snprintf(path, 64, "%s", configured_path);
		descriptor = open(path, O_RDWR | O_CLOEXEC);
		if (descriptor < 0) {
			result = -errno;
		} else {
			result = verify_hidraw(descriptor, expected_input);
		}
	} else {
		for (index = 0; index < 256 && result != 0; ++index) {
			snprintf(path, 64, "/dev/hidraw%u", index);
			descriptor = open(path, O_RDWR | O_CLOEXEC);
			if (descriptor >= 0) {
				result = verify_hidraw(descriptor, expected_input);
				if (result != 0) {
					close(descriptor);
					descriptor = -1;
				}
			}
		}
	}
	if (result != 0 && descriptor >= 0) {
		close(descriptor);
		descriptor = -1;
	}
	if (result != 0) {
		fprintf(stderr,
			"actions-micro: cannot find 185b:2d1d input%d: %s\n",
			expected_input, strerror(-result));
	}

	return result == 0 ? descriptor : result;
}

static int write_all(int descriptor, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;
	size_t total = 0;
	ssize_t written;

	while (total < length) {
		written = write(descriptor, bytes + total, length - total);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written < 0) {
			return -errno;
		}
		if (written == 0) {
			return -EIO;
		}
		total += (size_t)written;
	}

	return 0;
}

static int send_report(struct actions_context *state, uint8_t endpoint,
		       const unsigned char data[HID_REPORT_LENGTH])
{
	int descriptor;
	int result;

	if (endpoint == 0x03) {
		descriptor = state->hid[0];
	} else if (endpoint == 0x04) {
		descriptor = state->hid[1];
	} else {
		return -EPROTO;
	}
	result = write_all(descriptor, data, HID_REPORT_LENGTH);
	if (result == 0) {
		++state->reports;
	}

	return result;
}

static void advance_transport_state(struct actions_context *state,
				    uint8_t endpoint,
				    const unsigned char data[HID_REPORT_LENGTH])
{
	uint32_t sequence = read_le32(data + 4);

	if (data[1] == 1) {
		if (sequence >= state->command_sequence) {
			state->command_sequence = sequence + 1;
		}
		state->next_command_endpoint = endpoint == 0x04 ? 0x03 : 0x04;
	} else if (data[1] == 2) {
		if (sequence >= state->video_sequence) {
			state->video_sequence = sequence + 1;
		}
		++state->video_report_index;
	}
}

static int send_initialization(struct actions_context *state)
{
	uint64_t start_ns = monotonic_nanoseconds();
	size_t index;
	int result = 0;

	for (index = 0; index < state->initialization_count && result == 0;
	     ++index) {
		result = sleep_until_ns(start_ns +
			state->initialization[index].timestamp_us * 1000ULL);
		if (result == 0) {
			result = send_report(state,
				state->initialization[index].endpoint,
				state->initialization[index].data);
		}
		if (result == 0) {
			advance_transport_state(state,
				state->initialization[index].endpoint,
				state->initialization[index].data);
		}
	}

	return result;
}

static int send_full_bootstrap(struct actions_context *state)
{
	uint64_t start_ns = monotonic_nanoseconds();
	size_t index;
	int result = 0;

	for (index = 0; index < state->bootstrap_count && result == 0;
	     ++index) {
		result = sleep_until_ns(start_ns +
			state->bootstrap[index].timestamp_us * 1000ULL);
		if (result == 0) {
			result = send_report(state, state->bootstrap[index].endpoint,
					     state->bootstrap[index].data);
		}
		if (result == 0) {
			advance_transport_state(state,
				state->bootstrap[index].endpoint,
				state->bootstrap[index].data);
		}
	}

	return result;
}

static void configure_heartbeat_schedule(struct actions_context *state,
					 uint64_t now_ns)
{
	uint64_t first_offset_us;

	if (state->heartbeat_count > 1) {
		state->heartbeat_period_us =
			(state->heartbeats[state->heartbeat_count - 1].timestamp_us -
			 state->heartbeats[0].timestamp_us) /
			(state->heartbeat_count - 1);
	} else {
		state->heartbeat_period_us = DEFAULT_HEARTBEAT_US;
	}
	if (state->heartbeat_period_us < 100000ULL ||
	    state->heartbeat_period_us > 10000000ULL) {
		state->heartbeat_period_us = DEFAULT_HEARTBEAT_US;
	}
	first_offset_us = state->heartbeats[0].timestamp_us -
			  state->video_start_us;
	if (first_offset_us > state->heartbeat_period_us) {
		first_offset_us = state->heartbeat_period_us;
	}
	state->next_heartbeat_ns = now_ns + first_offset_us * 1000ULL;
}

static int send_due_heartbeats(struct actions_context *state, uint64_t now_ns)
{
	struct stored_report *current;
	unsigned char report[HID_REPORT_LENGTH];
	uint64_t delay_us;
	uint8_t endpoint;
	unsigned int sent = 0;
	int result = 0;

	while (result == 0 && now_ns >= state->next_heartbeat_ns &&
	       sent < MAX_HEARTBEATS_PER_TICK) {
		current = &state->heartbeats[state->heartbeat_index];
		memcpy(report, current->data, sizeof(report));
		endpoint = state->next_command_endpoint;
		report[0] = endpoint == 0x04 ? 1 : 2;
		write_le32(report + 4, state->command_sequence++);
		result = send_report(state, endpoint, report);
		if (result == 0) {
			state->next_command_endpoint =
				endpoint == 0x04 ? 0x03 : 0x04;
		}
		if (state->heartbeat_index + 1 < state->heartbeat_count) {
			delay_us = state->heartbeats[state->heartbeat_index + 1].timestamp_us -
				   current->timestamp_us;
			++state->heartbeat_index;
		} else {
			delay_us = state->heartbeat_period_us;
			state->heartbeat_index = 0;
		}
		if (delay_us < 10000ULL || delay_us > 10000000ULL) {
			delay_us = state->heartbeat_period_us;
		}
		state->next_heartbeat_ns += delay_us * 1000ULL;
		++sent;
	}
	if (sent == MAX_HEARTBEATS_PER_TICK &&
	    now_ns >= state->next_heartbeat_ns) {
		state->next_heartbeat_ns =
			now_ns + state->heartbeat_period_us * 1000ULL;
	}

	return result;
}

static void close_encoder_descriptors(struct actions_encoder *encoder)
{
	if (encoder->output_fd >= 0) {
		close(encoder->output_fd);
		encoder->output_fd = -1;
	}
	if (encoder->input_fd >= 0) {
		close(encoder->input_fd);
		encoder->input_fd = -1;
	}
}

static void stop_encoder(struct actions_encoder *encoder)
{
	int status;

	if (encoder->input_fd >= 0) {
		close(encoder->input_fd);
		encoder->input_fd = -1;
	}
	if (encoder->output_fd >= 0) {
		close(encoder->output_fd);
		encoder->output_fd = -1;
	}
	if (encoder->process > 0) {
		if (waitpid(encoder->process, &status, WNOHANG) == 0) {
			kill(encoder->process, SIGTERM);
			while (waitpid(encoder->process, &status, 0) < 0 &&
			       errno == EINTR) {
			}
		}
		encoder->process = -1;
	}
	encoder->format = USBDISPLAY_FORMAT_INVALID;
}

static int set_close_on_exec(int descriptor)
{
	int flags = fcntl(descriptor, F_GETFD);

	if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
		return -errno;
	}

	return 0;
}

static int start_encoder(struct actions_context *state, uint32_t format)
{
	int input_pipe[2] = {-1, -1};
	int output_pipe[2] = {-1, -1};
	char dimensions[64];
	char frame_rate[16];
	const char *pixel_format;
	int flags;
	int result = 0;

	stop_encoder(&state->encoder);
	if (format == USBDISPLAY_FORMAT_XRGB8888) {
		pixel_format = "bgra";
	} else if (format == USBDISPLAY_FORMAT_RGB565) {
		pixel_format = "rgb565le";
	} else {
		return -ENOTSUP;
	}
	snprintf(dimensions, sizeof(dimensions), "%ux%u", state->width,
		 state->height);
	snprintf(frame_rate, sizeof(frame_rate), "%u", state->options.fps);
	if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) {
		result = -errno;
	}
	if (result == 0) {
		result = set_close_on_exec(input_pipe[1]);
	}
	if (result == 0) {
		result = set_close_on_exec(output_pipe[0]);
	}
	if (result == 0) {
		state->encoder.process = fork();
		if (state->encoder.process < 0) {
			result = -errno;
		} else if (state->encoder.process == 0) {
			if (dup2(input_pipe[0], STDIN_FILENO) < 0 ||
			    dup2(output_pipe[1], STDOUT_FILENO) < 0) {
				_exit(126);
			}
			close(input_pipe[0]);
			close(input_pipe[1]);
			close(output_pipe[0]);
			close(output_pipe[1]);
			execlp(state->options.encoder, state->options.encoder,
			       "-hide_banner", "-loglevel", "error", "-nostdin",
			       "-f", "rawvideo", "-pix_fmt", pixel_format,
			       "-s", dimensions, "-r", frame_rate, "-i", "pipe:0",
			       "-an", "-vf", "setsar=1/1", "-c:v", "libx264",
			       "-preset", "ultrafast",
			       "-tune", "zerolatency", "-profile:v", "baseline",
			       "-pix_fmt", "yuv420p", "-g", frame_rate,
			       "-keyint_min", frame_rate, "-bf", "0",
			       "-x264-params",
			       "aud=1:repeat-headers=1:scenecut=0",
			       "-flush_packets", "1", "-f", "h264", "pipe:1",
			       (char *)NULL);
			_exit(127);
		}
	}
	if (input_pipe[0] >= 0) {
		close(input_pipe[0]);
	}
	if (output_pipe[1] >= 0) {
		close(output_pipe[1]);
	}
	if (result == 0) {
		state->encoder.input_fd = input_pipe[1];
		state->encoder.output_fd = output_pipe[0];
		input_pipe[1] = -1;
		output_pipe[0] = -1;
		flags = fcntl(state->encoder.output_fd, F_GETFL);
		if (flags < 0 || fcntl(state->encoder.output_fd, F_SETFL,
				       flags | O_NONBLOCK) != 0) {
			result = -errno;
		} else {
			state->encoder.format = format;
		}
	}
	if (input_pipe[1] >= 0) {
		close(input_pipe[1]);
	}
	if (output_pipe[0] >= 0) {
		close(output_pipe[0]);
	}
	if (result != 0) {
		close_encoder_descriptors(&state->encoder);
		stop_encoder(&state->encoder);
	}

	return result;
}

static int reserve_buffer(unsigned char **buffer, size_t *capacity,
			  size_t required, size_t maximum)
{
	size_t new_capacity;
	unsigned char *resized;

	if (required <= *capacity) {
		return 0;
	}
	new_capacity = *capacity == 0 ? 65536U : *capacity;
	while (new_capacity < required && new_capacity < maximum) {
		new_capacity *= 2;
	}
	if (new_capacity > maximum) {
		new_capacity = maximum;
	}
	if (new_capacity < required) {
		return -E2BIG;
	}
	resized = realloc(*buffer, new_capacity);
	if (resized == NULL) {
		return -ENOMEM;
	}
	*buffer = resized;
	*capacity = new_capacity;

	return 0;
}

static int pack_frame(struct actions_context *state,
		      const struct usbdisplay_frame *frame, size_t *packed_bytes)
{
	size_t bytes_per_pixel;
	size_t row_bytes;
	size_t required;
	uint32_t row;
	int result;

	if (frame->format == USBDISPLAY_FORMAT_XRGB8888) {
		bytes_per_pixel = 4;
	} else if (frame->format == USBDISPLAY_FORMAT_RGB565) {
		bytes_per_pixel = 2;
	} else {
		return -ENOTSUP;
	}
	row_bytes = (size_t)frame->width * bytes_per_pixel;
	required = row_bytes * frame->height;
	if (frame->stride < row_bytes || required == 0) {
		return -EPROTO;
	}
	result = reserve_buffer(&state->encoder.packed_frame,
				&state->encoder.packed_capacity, required,
				required);
	if (result == 0) {
		for (row = 0; row < frame->height; ++row) {
			memcpy(state->encoder.packed_frame + (size_t)row * row_bytes,
			       (const unsigned char *)frame->pixels +
			       (size_t)row * frame->stride, row_bytes);
		}
		*packed_bytes = required;
	}

	return result;
}

static int read_encoded_access_unit(struct actions_context *state,
				    size_t *encoded_bytes)
{
	struct pollfd descriptor;
	ssize_t count;
	int timeout = (int)state->options.encode_timeout_ms;
	int poll_result;
	int result = 0;

	descriptor.fd = state->encoder.output_fd;
	descriptor.events = POLLIN | POLLHUP;
	descriptor.revents = 0;
	*encoded_bytes = 0;
	while (result == 0) {
		poll_result = poll(&descriptor, 1, timeout);
		if (poll_result < 0 && errno == EINTR) {
			continue;
		}
		if (poll_result < 0) {
			result = -errno;
		} else if (poll_result == 0) {
			if (*encoded_bytes == 0) {
				result = -ETIMEDOUT;
			}
			break;
		} else if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
			result = -EPIPE;
		} else {
			result = reserve_buffer(&state->encoder.encoded,
						&state->encoder.encoded_capacity,
						*encoded_bytes + 65536U,
						MAX_ENCODED_BYTES);
			if (result == 0) {
				count = read(state->encoder.output_fd,
					     state->encoder.encoded + *encoded_bytes,
					     state->encoder.encoded_capacity -
					     *encoded_bytes);
				if (count > 0) {
					*encoded_bytes += (size_t)count;
					timeout = 20;
				} else if (count == 0) {
					result = -EPIPE;
				} else if (errno != EAGAIN && errno != EINTR) {
					result = -errno;
				}
			}
		}
	}

	return result;
}

static size_t find_start_code(const unsigned char *data, size_t length,
			      size_t offset, size_t *start_length)
{
	for (; offset + 3 <= length; ++offset) {
		if (offset + 4 <= length && data[offset] == 0 &&
		    data[offset + 1] == 0 && data[offset + 2] == 0 &&
		    data[offset + 3] == 1) {
			*start_length = 4;
			return offset;
		}
		if (data[offset] == 0 && data[offset + 1] == 0 &&
		    data[offset + 2] == 1) {
			*start_length = 3;
			return offset;
		}
	}

	return length;
}

static int append_bytes(struct byte_buffer *buffer, const unsigned char *data,
			size_t length)
{
	int result;

	result = reserve_buffer(&buffer->data, &buffer->capacity,
				buffer->length + length, MAX_ENCODED_BYTES);
	if (result == 0) {
		memcpy(buffer->data + buffer->length, data, length);
		buffer->length += length;
	}

	return result;
}

static int append_annex_b_nal(struct byte_buffer *buffer,
			      const unsigned char *data, size_t length,
			      size_t start_length)
{
	static const unsigned char start_code[4] = {0, 0, 0, 1};
	int result;

	if ((start_length != 3 && start_length != 4) ||
	    length <= start_length) {
		return -EPROTO;
	}
	result = append_bytes(buffer, start_code, sizeof(start_code));
	if (result == 0) {
		result = append_bytes(buffer, data + start_length,
				      length - start_length);
	}

	return result;
}

static int split_access_unit(const unsigned char *data, size_t length,
			     struct byte_buffer *configuration,
			     struct byte_buffer *video, bool *idr)
{
	size_t start_length;
	size_t next_start_length = 0;
	size_t start = find_start_code(data, length, 0, &start_length);
	size_t next;
	unsigned int nal_type;
	int result = 0;

	*idr = false;
	while (start < length && result == 0) {
		next = find_start_code(data, length, start + start_length + 1,
				       &next_start_length);
		if (start + start_length >= length) {
			result = -EPROTO;
			break;
		}
		nal_type = data[start + start_length] & 0x1fU;
		if (nal_type == 6 || nal_type == 7 || nal_type == 8) {
			result = append_annex_b_nal(configuration, data + start,
						    next - start,
						    start_length);
		} else if (nal_type != 9) {
			result = append_annex_b_nal(video, data + start,
						    next - start,
						    start_length);
			if (nal_type == 5) {
				*idr = true;
			}
		}
		start = next;
		start_length = next_start_length;
	}
	if (result == 0 && video->length == 0) {
		result = -EPROTO;
	}

	return result;
}

static int send_video_message(struct actions_context *state,
			      uint32_t data_flags,
			      const unsigned char *video_data,
			      size_t video_bytes)
{
	static const unsigned char protocol_id[4] = {0x29, 0x00, 0x67, 0x45};
	unsigned char report[HID_REPORT_LENGTH];
	unsigned char *message;
	size_t message_bytes;
	size_t offset;
	size_t fragment_bytes;
	uint32_t fragment_count;
	uint32_t fragment_index;
	uint32_t fragment_info;
	uint8_t endpoint;
	int result = 0;

	if (video_bytes > UINT32_MAX - INNER_HEADER_LENGTH) {
		return -E2BIG;
	}
	message_bytes = INNER_HEADER_LENGTH + video_bytes;
	fragment_count = (uint32_t)((message_bytes + HID_PAYLOAD_LENGTH - 1) /
				    HID_PAYLOAD_LENGTH);
	if (fragment_count == 0 || fragment_count > UINT16_MAX) {
		return -E2BIG;
	}
	message = malloc(message_bytes);
	if (message == NULL) {
		return -ENOMEM;
	}
	write_le32(message, (uint32_t)message_bytes);
	memcpy(message + 4, "RRIM", 4);
	memcpy(message + 8, protocol_id, sizeof(protocol_id));
	write_le32(message + 12, 3);
	memcpy(message + 16, "TADV", 4);
	write_le32(message + 20, 0);
	write_le32(message + 24, data_flags);
	write_le32(message + 28, (uint32_t)video_bytes);
	memcpy(message + INNER_HEADER_LENGTH, video_data, video_bytes);
	for (fragment_index = 0; fragment_index < fragment_count && result == 0;
	     ++fragment_index) {
		offset = (size_t)fragment_index * HID_PAYLOAD_LENGTH;
		fragment_bytes = message_bytes - offset;
		if (fragment_bytes > HID_PAYLOAD_LENGTH) {
			fragment_bytes = HID_PAYLOAD_LENGTH;
		}
		memset(report, 0, sizeof(report));
		endpoint = state->video_report_index % 2 == 0 ? 0x04 : 0x03;
		report[0] = endpoint == 0x04 ? 1 : 2;
		report[1] = 2;
		write_le16(report + 2, 0);
		write_le32(report + 4, state->video_sequence);
		fragment_info = fragment_count | (fragment_index << 16);
		write_le32(report + 8, fragment_info);
		write_le16(report + 12, (uint16_t)fragment_bytes);
		memcpy(report + HID_HEADER_LENGTH, message + offset,
		       fragment_bytes);
		result = send_report(state, endpoint, report);
		++state->video_report_index;
		if (result == 0 && fragment_index + 1 < fragment_count) {
			result = sleep_microseconds(
				state->options.fragment_interval_us);
		}
	}
	++state->video_sequence;
	free(message);

	return result;
}

static int actions_submit(void *context,
			  const struct usbdisplay_frame *frame)
{
	struct actions_context *state = context;
	struct byte_buffer configuration = {0};
	struct byte_buffer video = {0};
	size_t packed_bytes = 0;
	size_t encoded_bytes = 0;
	bool idr = false;
	int result;

	if (frame->width != state->width || frame->height != state->height) {
		return -EINVAL;
	}
	result = send_due_heartbeats(state, monotonic_nanoseconds());
	if (result == 0 && state->encoder.format != frame->format) {
		result = start_encoder(state, frame->format);
	}
	if (result == 0) {
		result = pack_frame(state, frame, &packed_bytes);
	}
	if (result == 0) {
		result = write_all(state->encoder.input_fd,
				   state->encoder.packed_frame, packed_bytes);
	}
	if (result == 0) {
		result = read_encoded_access_unit(state, &encoded_bytes);
	}
	if (result == 0) {
		result = split_access_unit(state->encoder.encoded, encoded_bytes,
					   &configuration, &video, &idr);
	}
	if (result == 0 && configuration.length != 0) {
		result = send_video_message(state, VIDEO_CONFIG,
					    configuration.data,
					    configuration.length);
		if (result == 0) {
			result = sleep_microseconds(1000U);
		}
	}
	if (result == 0) {
		result = send_video_message(state, idr ? VIDEO_IDR : VIDEO_P,
					    video.data, video.length);
	}
	if (result == 0) {
		++state->frames;
	}
	free(video.data);
	free(configuration.data);
	if (result != 0) {
		fprintf(stderr, "actions-micro: frame submission failed: %s\n",
			strerror(-result));
	}

	return result;
}

static int actions_tick(void *context, uint64_t now_ns)
{
	struct actions_context *state = context;

	if (now_ns == 0) {
		now_ns = monotonic_nanoseconds();
	}

	return send_due_heartbeats(state, now_ns);
}

static void actions_close(void *context)
{
	struct actions_context *state = context;

	if (state != NULL) {
		stop_encoder(&state->encoder);
		if (state->hid[1] >= 0) {
			close(state->hid[1]);
		}
		if (state->hid[0] >= 0) {
			close(state->hid[0]);
		}
		fprintf(stderr,
			"actions-micro: closed frames=%llu reports=%llu\n",
			(unsigned long long)state->frames,
			(unsigned long long)state->reports);
		free(state->encoder.encoded);
		free(state->encoder.packed_frame);
		free(state->bootstrap);
		free(state->heartbeats);
		free(state->initialization);
		free_options(&state->options);
		free(state);
	}
}

static int actions_open(const struct usbdisplay_backend_config *config,
			void **context)
{
	struct actions_context *state;
	char hid0_path[64];
	char hid1_path[64];
	int result;

	if (config->device_width == 0 || config->device_height == 0 ||
	    (config->device_width & 1U) != 0 ||
	    (config->device_height & 1U) != 0) {
		return -EINVAL;
	}
	state = calloc(1, sizeof(*state));
	if (state == NULL) {
		return -ENOMEM;
	}
	state->hid[0] = -1;
	state->hid[1] = -1;
	state->encoder.process = -1;
	state->encoder.input_fd = -1;
	state->encoder.output_fd = -1;
	state->encoder.format = USBDISPLAY_FORMAT_INVALID;
	state->width = config->device_width;
	state->height = config->device_height;
	result = parse_options(config->option, &state->options);
	if (result == 0) {
		result = load_template(state);
	}
	if (result == 0) {
		state->hid[0] = open_hidraw(state->options.hid0_path, 0,
					    hid0_path);
		if (state->hid[0] < 0) {
			result = state->hid[0];
			state->hid[0] = -1;
		}
	}
	if (result == 0) {
		state->hid[1] = open_hidraw(state->options.hid1_path, 1,
					    hid1_path);
		if (state->hid[1] < 0) {
			result = state->hid[1];
			state->hid[1] = -1;
		}
	}
	if (result == 0) {
		fprintf(stderr,
			"actions-micro: %s + %s, init=%zu heartbeat=%zu "
			"bootstrap=%zu\n",
			hid0_path, hid1_path, state->initialization_count,
			state->heartbeat_count, state->bootstrap_count);
		if (state->options.full_bootstrap) {
			result = send_full_bootstrap(state);
		} else {
			result = send_initialization(state);
		}
	}
	if (result == 0) {
		configure_heartbeat_schedule(state, monotonic_nanoseconds());
		fprintf(stderr,
			"actions-micro: transport next command=%u video=%u "
			"video-report=%llu\n",
			state->command_sequence, state->video_sequence,
			(unsigned long long)state->video_report_index);
		*context = state;
	} else {
		actions_close(state);
	}

	return result;
}

static const struct usbdisplay_backend_v1 actions_backend = {
	.abi_version = USBDISPLAY_BACKEND_ABI_VERSION,
	.struct_size = sizeof(struct usbdisplay_backend_v1),
	.capabilities = USBDISPLAY_BACKEND_CAP_TICK,
	.name = "actions-micro-185b-2d1d",
	.open = actions_open,
	.submit = actions_submit,
	.close = actions_close,
	.tick = actions_tick,
};

const struct usbdisplay_backend_v1 *usbdisplay_backend_v1(void)
{
	return &actions_backend;
}
