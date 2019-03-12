#define _GNU_SOURCE
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <unistd.h>
#include <fcntl.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>
#include <time.h>
#include <signal.h>

#include <error.h>

#include "../src/log.h"

/* simulation parameters */
#define SETPOINT_MIN 31500
#define SETPOINT_MAX 1016052
/* offset to apply at maximum at each turn in ns */
#define TREND_OFFSET_NS 10
/* maximum absolute value of the error offset to add at each turn */
#define ERROR_AMPLITUDE_NS 100
#define INITIAL_ERROR_AMPLITUDE_NS 10000000

#define SETPOINT_AMPLITUDE ((SETPOINT_MAX - SETPOINT_MIN) + 1)

#define CONTROL_FIFO_PATH "oscillator_sim.control"

static void dummy_print_progname(void)
{
	fprintf(stderr, ERR);
}

static volatile int loop = true;

static void signal_handler(int signum)
{
	info("Caught signal %s.\n", strsignal(signum));
	if (!loop) {
		err("Signalled twice, brutal exit.\n");
		exit(EXIT_FAILURE);
	}
	loop = false;
}

/* returns a random number in [min, max) */
static long random_in_range(long min, long max)
{
	return (random() % (min - max)) + min;
}

/* compute what to add to the current phase offset */
static long compute_delta(uint32_t setpoint)
{
	/* find the center of the setpoints interval */
	int center = (SETPOINT_MIN + SETPOINT_MAX) / 2;
	/* compute the alg. distance of the current setpoint to this center */
	int distance = abs(setpoint - center);
	/*
	 * base offset added is so that the maximum distance increases the
	 * offset by TREND_OFFSET ns
	 */
	long base_offset = (TREND_OFFSET_NS * distance) / SETPOINT_AMPLITUDE;
	/* add a random error */
	int error = (random() % (2 * ERROR_AMPLITUDE_NS)) - ERROR_AMPLITUDE_NS;

	return base_offset + error;
}

static void cleanup(void)
{
	unlink(CONTROL_FIFO_PATH);
}

int main(int argc, char *argv[])
{
	const char *prog_name;
	int ret;
	int tfd;
	struct itimerspec its;
	int flags;
	struct timeval tv;
	fd_set readfds;
	uint64_t expired;
	ssize_t sret;
	/* read from the 1PPS device by oscillatord containint the error */
	int32_t phase_error;
	/* data written by oscillatord to the 1PPS device */
	int32_t phase_offset;
	uint32_t setpoint;
	time_t seed;
	int control_fifo;
	int phase_error_fd;
	const char *phase_error_pts;

	seed = time(NULL);
	srand(seed);

	/* remove the line startup in error() calls */
	error_print_progname = dummy_print_progname;

	prog_name = basename(argv[0]);
	if (argc != 2)
		error(EXIT_FAILURE, 0, "%s phase_error_pts", prog_name);
	phase_error_pts = argv[1];

	cleanup();
	ret = mkfifo(CONTROL_FIFO_PATH, 0600);
	if (ret == -1)
		error(EXIT_FAILURE, errno, "mkfifo(%s)", CONTROL_FIFO_PATH);
	atexit(cleanup);
	control_fifo = open(CONTROL_FIFO_PATH, O_RDONLY);
	if (control_fifo == -1)
		error(EXIT_FAILURE, errno, "open(%s)", CONTROL_FIFO_PATH);

	phase_error_fd = open(phase_error_pts, O_RDWR);
	if (phase_error_fd == -1)
		error(EXIT_FAILURE, errno, "open(%s)", phase_error_pts);

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
	if (tfd == -1)
		error(EXIT_FAILURE, errno, "timerfd_create");

	debug("control_fifo %d, tfd %d\n", control_fifo, tfd);

	its.it_value.tv_sec = its.it_interval.tv_sec = 1;
	its.it_value.tv_nsec = its.it_interval.tv_nsec = 0;
	flags = 0;
	ret = timerfd_settime(tfd, flags, &its, NULL);
	if (ret == -1)
		error(EXIT_FAILURE, errno, "timerfd_settime");

	info("%s[%jd] started, seed %jd.\n", prog_name, (intmax_t)getpid(),
			(intmax_t)seed);

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* choose initial setpoint */
	setpoint = random_in_range(SETPOINT_MIN, SETPOINT_MAX + 1);
	setpoint = SETPOINT_MIN;
	info("initial setpoint %"PRIu32"\n", setpoint);
	/* choose initial phase_error */
	phase_error = random_in_range(-INITIAL_ERROR_AMPLITUDE_NS,
			INITIAL_ERROR_AMPLITUDE_NS);
	info("initial phase_error: %"PRIi32"\n", phase_error);

	memset(&its, 0, sizeof(its));
	while (loop) {
		memset(&tv, 0, sizeof(tv));
		tv.tv_sec = 2;
		FD_ZERO(&readfds);
		FD_SET(control_fifo, &readfds);
		FD_SET(phase_error_fd, &readfds);
		FD_SET(tfd, &readfds);
		ret = select(tfd + 1, &readfds, NULL, NULL, &tv);
		if (ret == -1) {
			if (errno == EINTR && !loop)
				break;
			else
				error(EXIT_FAILURE, errno, "select");
		}
		if (ret == 0) /* timeout */
			continue;
		if (FD_ISSET(tfd, &readfds)) {
			sret = read(tfd, &expired, sizeof(expired));
			if (sret < 0)
				error(EXIT_FAILURE, errno, "read");
			phase_error += compute_delta(setpoint);
			debug("phase error: %"PRIi32"\n", phase_error);

			if (phase_error_fd != -1) {
				sret = write(phase_error_fd, &phase_error,
						sizeof(phase_error));
				if (sret == -1)
					error(EXIT_FAILURE, errno, "write");
			}
		}
		if (FD_ISSET(control_fifo, &readfds)) {
			sret = read(control_fifo, &setpoint, sizeof(setpoint));
			if (sret < 0)
				error(EXIT_FAILURE, errno, "read");
			if (sret == 0) {
				info("Peer closed the control fifo\n");
				break;
			}
			debug("new setpoint: %"PRIu32"\n", setpoint);
		}
		if (FD_ISSET(phase_error_fd, &readfds)) {
			sret = read(phase_error_fd, &phase_offset,
					sizeof(phase_offset));
			if (sret < 0)
				error(EXIT_FAILURE, errno, "read");
			debug("applying phase offset: %"PRIi32"\n",
					phase_offset);
			phase_error += phase_offset;
		}

		usleep(100000);
	}

	close(tfd);

	info("%s exiting.\n", prog_name);

	return EXIT_SUCCESS;
}