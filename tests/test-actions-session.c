/* SPDX-License-Identifier: GPL-2.0-only */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../backends/actions-micro/actions_micro_backend.c"

static void make_control_report(unsigned char report[HID_REPORT_LENGTH],
				const char tag[4], uint16_t remote,
				uint16_t echoed)
{
	unsigned char *payload = report + HID_HEADER_LENGTH;

	memset(report, 0, HID_REPORT_LENGTH);
	report[0] = 1;
	report[1] = 1;
	write_le32(report + 8, 1);
	write_le16(report + 12, 24);
	write_le32(payload, 24);
	memcpy(payload + 4, tag, 4);
	write_le16(payload + 8, remote);
	write_le16(payload + 10, echoed);
	write_le32(payload + 12, 3);
}

static void initialize_session(struct actions_context *state)
{
	memset(state, 0, sizeof(*state));
	state->local_session = 0x1111;
	state->remote_session = SESSION_UNKNOWN;
	state->acked_remote_session = SESSION_UNKNOWN;
	state->hid[0] = -1;
	state->hid[1] = -1;
	state->next_command_endpoint = 0x03;
}

static int test_rewrite(void)
{
	unsigned char first[HID_REPORT_LENGTH];
	unsigned char continuation[HID_REPORT_LENGTH];
	unsigned char before[HID_REPORT_LENGTH];
	bool rewritten;
	int result = 0;

	memset(first, 0, sizeof(first));
	first[1] = 2;
	write_le32(first + 8, 2);
	write_le16(first + 12, 32);
	memcpy(first + HID_HEADER_LENGTH + 4, "RRIM", 4);
	write_le16(first + HID_HEADER_LENGTH + 8, 0xaaaa);
	write_le16(first + HID_HEADER_LENGTH + 10, 0xbbbb);
	rewritten = rewrite_report_session(first, 0x1111, 0x2222);
	if (!rewritten || read_le16(first + HID_HEADER_LENGTH + 8) != 0x1111 ||
	    read_le16(first + HID_HEADER_LENGTH + 10) != 0x2222) {
		result = -EINVAL;
	}

	if (result == 0) {
		memset(continuation, 0xa5, sizeof(continuation));
		continuation[1] = 2;
		write_le32(continuation + 8, 2U | (1U << 16));
		write_le16(continuation + 12, 32);
		memcpy(before, continuation, sizeof(before));
		rewritten = rewrite_report_session(continuation, 0x1111, 0x2222);
		if (rewritten || memcmp(continuation, before, sizeof(before)) != 0) {
			result = -EINVAL;
		}
	}

	return result;
}

static int test_parser(void)
{
	struct actions_context state;
	unsigned char report[HID_REPORT_LENGTH];
	int result = 0;

	initialize_session(&state);
	make_control_report(report, "CNYS", 0x2222, 0x1111);
	if (consume_sync_report(&state, report, HID_HEADER_LENGTH + 24U) != 0 ||
	    state.remote_session != 0x2222 || !state.peer_confirmed ||
	    state.session_confirmed || !state.sync_reply_pending) {
		result = -EINVAL;
	}

	if (result == 0) {
		state.acked_remote_session = 0x2222;
		make_control_report(report, "RRIM", 0x2222, 0x1111);
		if (consume_sync_report(&state, report,
					HID_HEADER_LENGTH + 24U) != 0 ||
			!state.session_confirmed) {
			result = -EINVAL;
		}
	}

	if (result == 0) {
		make_control_report(report, "CNYS", 0x3333, 0x1111);
		if (consume_sync_report(&state, report,
					HID_HEADER_LENGTH + 24U) != -ESTALE) {
			result = -EINVAL;
		}
	}

	if (result == 0) {
		initialize_session(&state);
		make_control_report(report, "RRIM", 0x2222, 0x1111);
		if (consume_sync_report(&state, report,
					HID_HEADER_LENGTH + 24U) != 0 ||
			state.session_confirmed) {
			result = -EINVAL;
		}
	}

	if (result == 0) {
		make_control_report(report, "CNYS", 0x2222, 0x1111);
		if (consume_sync_report(&state, report, HID_HEADER_LENGTH + 23U) != 0 ||
			state.remote_session != SESSION_UNKNOWN) {
			result = -EINVAL;
		}
	}

	return result;
}

static int recv_report(int descriptor, unsigned char report[HID_REPORT_LENGTH])
{
	struct pollfd poll_descriptor = {
		.fd = descriptor,
		.events = POLLIN,
	};
	int poll_result;
	ssize_t bytes_read;

	do {
		poll_result = poll(&poll_descriptor, 1, 2000);
	} while (poll_result < 0 && errno == EINTR);
	if (poll_result <= 0 ||
	    (poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
		return -ETIMEDOUT;
	}
	bytes_read = recv(descriptor, report, HID_REPORT_LENGTH, 0);
	return bytes_read == (ssize_t)HID_REPORT_LENGTH ? 0 : -EIO;
}

static int responder(int command_descriptor, int video_descriptor,
			    uint16_t local_session)
{
	unsigned char request[HID_REPORT_LENGTH];
	unsigned char response[HID_REPORT_LENGTH];
	int result = 0;

	result = recv_report(command_descriptor, request);
	if (result == 0) {
		make_control_report(response, "CNYS", 0x2222, local_session);
		if (send(command_descriptor, response, sizeof(response), 0) !=
		    (ssize_t)sizeof(response)) {
			result = -EIO;
		}
	}
	if (result == 0) {
		result = recv_report(video_descriptor, request);
	}

	return result;
}

static int test_negotiate_success(void)
{
	struct actions_context state;
	int descriptors[4] = {-1, -1, -1, -1};
	pid_t child;
	int child_status = 0;
	unsigned int index;
	int result = 0;

	initialize_session(&state);
	for (index = 0; index < 2U && result == 0; ++index) {
		if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, &descriptors[index * 2U]) != 0) {
			result = -errno;
		}
	}
	if (result == 0) {
		state.hid[0] = descriptors[0];
		state.hid[1] = descriptors[2];
		child = fork();
		if (child < 0) {
			result = -errno;
		} else if (child == 0) {
			close(descriptors[0]);
			close(descriptors[2]);
			result = responder(descriptors[1], descriptors[3], state.local_session);
			close(descriptors[1]);
			close(descriptors[3]);
			_exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
		} else {
			close(descriptors[1]);
			descriptors[1] = -1;
			close(descriptors[3]);
			descriptors[3] = -1;
			result = negotiate_session(&state);
			if (result == 0 && (!state.session_confirmed ||
				state.remote_session != 0x2222)) {
				result = -EINVAL;
			}
			if (waitpid(child, &child_status, 0) < 0 ||
				!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
				result = -EIO;
			}
		}
	}
	for (index = 0; index < 4U; ++index) {
		if (descriptors[index] >= 0) {
			close(descriptors[index]);
		}
	}

	return result;
}

static int test_negotiate_timeout(void)
{
	struct actions_context state;
	int descriptors[4] = {-1, -1, -1, -1};
	uint64_t start_ns;
	uint64_t elapsed_ns = 0;
	unsigned int index;
	int result = 0;

	initialize_session(&state);
	for (index = 0; index < 2U && result == 0; ++index) {
		if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, &descriptors[index * 2U]) != 0) {
			result = -errno;
		}
	}
	if (result == 0) {
		state.hid[0] = descriptors[0];
		state.hid[1] = descriptors[2];
		start_ns = monotonic_nanoseconds();
		result = negotiate_session(&state);
		elapsed_ns = monotonic_nanoseconds() - start_ns;
		if (result != -ETIMEDOUT || elapsed_ns > 6000000000ULL) {
			result = -EINVAL;
		} else {
			result = 0;
		}
	}
	for (index = 0; index < 4U; ++index) {
		if (descriptors[index] >= 0) {
			close(descriptors[index]);
		}
	}

	return result;
}

int main(void)
{
	int result = test_rewrite();

	if (result == 0) {
		result = test_parser();
	}
	if (result == 0) {
		result = test_negotiate_success();
	}
	if (result == 0) {
		result = test_negotiate_timeout();
	}
	if (result != 0) {
		fprintf(stderr, "actions session tests failed: %d\n", result);
	} else {
		printf("actions session tests passed\n");
	}

	return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
