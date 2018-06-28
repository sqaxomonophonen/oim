#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#include <linux/input.h>
#include <errno.h>

int osc_fd;
int osc_buffer_length;
uint8_t osc_buffer[2048];

#define MAX_POLLFD (32)

static void osc_open(char* host, char* service)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	struct addrinfo *result;
	int err;
	if ((err = getaddrinfo(host, service, &hints, &result)) != 0) {
		fprintf(stderr, "getaddrinfo for %s: %s\n", host, gai_strerror(err));
		exit(EXIT_FAILURE);
	}

	for (struct addrinfo* rp = result; rp != NULL; rp = rp->ai_next) {
		osc_fd = socket(
			rp->ai_family,
			rp->ai_socktype,
			rp->ai_protocol);
		if (osc_fd == -1) continue;
		if (connect(osc_fd, rp->ai_addr, rp->ai_addrlen) != -1) break;
		close(osc_fd);
	}

	if (osc_fd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	freeaddrinfo(result);
}

static void osc_begin()
{
	osc_buffer_length = 0;
}

static void osc_str(char* s)
{
	char c;
	int n = 0;
	while ((c = *(s++))) {
		osc_buffer[osc_buffer_length++] = c;
		n++;
	}
	int zero_padding = 4 - (n & 3);
	for (int i = 0; i < zero_padding; i++) osc_buffer[osc_buffer_length++] = 0;
}

static void osc_f32(float f)
{
	union {
		float f;
		uint8_t b[4];
	} v;
	v.f = f;
	for (int i = 0; i < 4; i++) osc_buffer[osc_buffer_length++] = v.b[3 - i];
}

static void osc_end()
{
	if (osc_buffer_length == 0) return;
	if (write(osc_fd, osc_buffer, osc_buffer_length) != osc_buffer_length) {
		perror("write");
	}
	osc_buffer_length = 0;
}

static void prep_fd_at_path_for_poll(int* fd, const char* path, int* n_pollfds, struct pollfd* pollfds) {
	if (*fd == -1) {
		*fd = open(path, O_RDONLY);
		if (*fd != -1) fprintf(stderr, "open %s -> %d\n", path, *fd);
	}
	if (*fd >= 0) {
		pollfds[*n_pollfds].fd = *fd;
		pollfds[*n_pollfds].events = POLLIN;
		(*n_pollfds)++;
	}
}

static inline int handle_input_event(int* fd, struct pollfd* event, struct input_event* ev) {
	if (event->fd != *fd) return 0;

	if (event->revents & (POLLERR | POLLHUP)) {
		fprintf(stderr, "input event fd=%d HUP\n", *fd);
		close(*fd);
		*fd = -1;
		return 0;
	} else if (!(event->revents & POLLIN)) {
		return 0;
	}

	int n = read(*fd, ev, sizeof *ev);
	if (n != sizeof *ev) {
		fprintf(stderr, "input event fd=%d read error: %s\n", *fd, n == -1 ? strerror(errno) : "wrong size");
		close(*fd);
		return 0;
	} else {
		return 1;
	}
}


int main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	osc_open(argv[1], argv[2]);

	int fd_pen = -1;
	int fd_touch = -1;
	int fd_padbtns = -1;

	float pen_x = 0;
	float pen_y = 0;
	float pen_pressure = 0;

	for (;;) {
		int err;
		int n_pollfds = 0;
		struct pollfd pollfds[MAX_POLLFD];

		prep_fd_at_path_for_poll(&fd_pen,     "/dev/tablet_pen",     &n_pollfds, pollfds);
		prep_fd_at_path_for_poll(&fd_touch,   "/dev/tablet_touch",   &n_pollfds, pollfds);
		prep_fd_at_path_for_poll(&fd_padbtns, "/dev/tablet_padbtns", &n_pollfds, pollfds);

		int n_simple_pollfds = n_pollfds;

		err = poll(pollfds, n_pollfds, 10);
		if (err == -1) {
			perror("poll");
			sleep(1);
			continue;
		}

		for (int i = 0; i < n_simple_pollfds; i++) {
			struct pollfd* event = &pollfds[i];
			if (event->revents == 0) continue;

			struct input_event ev;
			if (handle_input_event(&fd_pen, event, &ev)) {
				if (ev.type == EV_ABS) {
					if (ev.code == ABS_X) {
						/* range = [0:14720] */
						pen_x = (float)ev.value / 14720.0f;
					} else if (ev.code == ABS_Y) {
						/* range = [0:9200] */
						pen_y = (float)ev.value / 9200.0f;
					} else if (ev.code == ABS_PRESSURE) {
						/* range = [0:1023] */
						pen_pressure = (float)ev.value / 1023.0f;
					} else if (ev.code == ABS_DISTANCE) {
						/* range = [0:31] */
					}

				}
			}

			if (handle_input_event(&fd_touch, event, &ev)) {
			}

			if (handle_input_event(&fd_padbtns, event, &ev)) {
			}
		}

		osc_begin();
		osc_str("/tablet");
		osc_str(",fff");
		osc_f32(pen_x);
		osc_f32(pen_y);
		osc_f32(pen_pressure);
		osc_end();
	}

	float x = 0;
	for (;;) {
		printf("PING x=%f\n", x);
		sleep(1);
		x += 1.0f;
	}

	return EXIT_SUCCESS;
}
