/*
 * uvcstream - serve a USB (UVC) webcam as an MJPEG HTTP stream using only
 * libusb in userspace. Built for routers whose kernel lacks V4L2/uvcvideo
 * (e.g. GL.iNet GL-AXT1800 stock firmware), but works on any Linux with usbfs.
 *
 *   GET /stream    multipart/x-mixed-replace MJPEG
 *   GET /snapshot  latest frame as image/jpeg
 *   GET /          tiny viewer page
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libuvc/libuvc.h>

#define MAX_CLIENTS 8

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint8_t *buf;
	size_t len, cap;
	uint64_t seq;
	int dead;
} frame = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static int clients;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * UVC MJPEG frames usually omit the Huffman tables (DHT) and rely on the
 * standard ones from JPEG Annex K. Many decoders cope, some do not, so we
 * insert them. The segment is built once from the standard bits/values.
 */
static const uint8_t dc_lum_bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t dc_chr_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t dc_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t ac_lum_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t ac_lum_vals[162] = {
	0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
	0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
	0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
	0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
	0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
	0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
	0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
	0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
	0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
	0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
	0xf9,0xfa };
static const uint8_t ac_chr_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t ac_chr_vals[162] = {
	0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
	0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
	0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
	0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
	0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
	0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
	0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
	0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
	0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
	0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
	0xf9,0xfa };

static uint8_t dht[512];
static size_t dht_len;

static void dht_add(uint8_t class_id, const uint8_t *bits, const uint8_t *vals, size_t nvals)
{
	dht[dht_len++] = class_id;
	memcpy(dht + dht_len, bits, 16);
	dht_len += 16;
	memcpy(dht + dht_len, vals, nvals);
	dht_len += nvals;
}

static void dht_build(void)
{
	dht[0] = 0xff;
	dht[1] = 0xc4;
	dht_len = 4;
	dht_add(0x00, dc_lum_bits, dc_vals, sizeof dc_vals);
	dht_add(0x10, ac_lum_bits, ac_lum_vals, sizeof ac_lum_vals);
	dht_add(0x01, dc_chr_bits, dc_vals, sizeof dc_vals);
	dht_add(0x11, ac_chr_bits, ac_chr_vals, sizeof ac_chr_vals);
	dht[2] = (dht_len - 2) >> 8;
	dht[3] = (dht_len - 2) & 0xff;
}

/* Offset of the SOS marker if the JPEG has no DHT before it, else 0. */
static size_t needs_dht(const uint8_t *p, size_t n)
{
	size_t i = 2;
	while (i + 4 <= n && p[i] == 0xff) {
		uint8_t m = p[i + 1];
		if (m == 0xc4)
			return 0;
		if (m == 0xda)
			return i;
		i += 2 + ((p[i + 2] << 8) | p[i + 3]);
	}
	return 0;
}

static void on_frame(uvc_frame_t *f, void *user)
{
	(void)user;
	const uint8_t *p = f->data;
	size_t n = f->data_bytes;
	if (n < 4 || p[0] != 0xff || p[1] != 0xd8)
		return; /* incomplete/corrupt frame */
	size_t sos = needs_dht(p, n);
	size_t total = n + (sos ? dht_len : 0);

	pthread_mutex_lock(&frame.lock);
	if (total > frame.cap) {
		uint8_t *nb = realloc(frame.buf, total);
		if (!nb) {
			pthread_mutex_unlock(&frame.lock);
			return;
		}
		frame.buf = nb;
		frame.cap = total;
	}
	if (sos) {
		memcpy(frame.buf, p, sos);
		memcpy(frame.buf + sos, dht, dht_len);
		memcpy(frame.buf + sos + dht_len, p + sos, n - sos);
	} else {
		memcpy(frame.buf, p, n);
	}
	frame.len = total;
	frame.seq++;
	pthread_cond_broadcast(&frame.cond);
	pthread_mutex_unlock(&frame.lock);
}

static int send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len) {
		ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
		if (w <= 0) {
			if (w < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		len -= w;
	}
	return 0;
}

/* Wait for a frame newer than *seq and copy it into *buf. */
static int next_frame(uint64_t *seq, uint8_t **buf, size_t *cap, size_t *len)
{
	struct timespec ts;
	pthread_mutex_lock(&frame.lock);
	while (frame.seq == *seq && !frame.dead) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 5;
		if (pthread_cond_timedwait(&frame.cond, &frame.lock, &ts) == ETIMEDOUT)
			break;
	}
	if (frame.seq == *seq || frame.dead) {
		pthread_mutex_unlock(&frame.lock);
		return -1;
	}
	if (frame.len > *cap) {
		uint8_t *nb = realloc(*buf, frame.len);
		if (!nb) {
			pthread_mutex_unlock(&frame.lock);
			return -1;
		}
		*buf = nb;
		*cap = frame.len;
	}
	memcpy(*buf, frame.buf, frame.len);
	*len = frame.len;
	*seq = frame.seq;
	pthread_mutex_unlock(&frame.lock);
	return 0;
}

static const char page[] =
	"<!doctype html><title>uvcstream</title>"
	"<body style=margin:0;background:#000>"
	"<img src=/stream style=width:100%;height:100vh;object-fit:contain>";

static void *serve(void *arg)
{
	int fd = (int)(intptr_t)arg;
	char req[1024];
	ssize_t n = recv(fd, req, sizeof req - 1, 0);
	uint8_t *buf = NULL;
	size_t cap = 0, len = 0;
	uint64_t seq = 0;
	char hdr[256];

	if (n <= 0)
		goto out;
	req[n] = 0;

	if (!strncmp(req, "GET /stream", 11)) {
		static const char h[] =
			"HTTP/1.0 200 OK\r\n"
			"Cache-Control: no-cache\r\n"
			"Connection: close\r\n"
			"Content-Type: multipart/x-mixed-replace;boundary=frame\r\n\r\n";
		if (send_all(fd, h, sizeof h - 1))
			goto out;
		while (!next_frame(&seq, &buf, &cap, &len)) {
			int hl = snprintf(hdr, sizeof hdr,
				"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", len);
			if (send_all(fd, hdr, hl) || send_all(fd, buf, len) || send_all(fd, "\r\n", 2))
				break;
		}
	} else if (!strncmp(req, "GET /snapshot", 13)) {
		if (next_frame(&seq, &buf, &cap, &len)) {
			static const char e[] = "HTTP/1.0 503 Service Unavailable\r\n\r\n";
			send_all(fd, e, sizeof e - 1);
			goto out;
		}
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
			"Cache-Control: no-cache\r\nConnection: close\r\n\r\n", len);
		if (!send_all(fd, hdr, hl))
			send_all(fd, buf, len);
	} else if (!strncmp(req, "GET / ", 6)) {
		int hl = snprintf(hdr, sizeof hdr,
			"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\n\r\n",
			sizeof page - 1);
		if (!send_all(fd, hdr, hl))
			send_all(fd, page, sizeof page - 1);
	} else {
		static const char e[] = "HTTP/1.0 404 Not Found\r\n\r\n";
		send_all(fd, e, sizeof e - 1);
	}
out:
	free(buf);
	close(fd);
	pthread_mutex_lock(&clients_lock);
	clients--;
	pthread_mutex_unlock(&clients_lock);
	return NULL;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-p port] [-W width] [-H height] [-f fps] [-d vid:pid] [-l]\n"
		"  -p  HTTP port (default 8080)\n"
		"  -W  frame width (default 1280)\n"
		"  -H  frame height (default 720)\n"
		"  -f  frames per second (default 15)\n"
		"  -d  USB vendor:product in hex, e.g. 046d:08e5 (default: first UVC device)\n"
		"  -l  print the camera's supported formats and exit\n",
		argv0);
}

int main(int argc, char **argv)
{
	int port = 8080, width = 1280, height = 720, fps = 15, list = 0;
	unsigned vid = 0, pid = 0;
	int opt;

	while ((opt = getopt(argc, argv, "p:W:H:f:d:lh")) != -1) {
		switch (opt) {
		case 'p': port = atoi(optarg); break;
		case 'W': width = atoi(optarg); break;
		case 'H': height = atoi(optarg); break;
		case 'f': fps = atoi(optarg); break;
		case 'd':
			if (sscanf(optarg, "%x:%x", &vid, &pid) != 2) {
				usage(argv[0]);
				return 2;
			}
			break;
		case 'l': list = 1; break;
		default: usage(argv[0]); return 2;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	dht_build();

	uvc_context_t *ctx;
	uvc_device_t *dev;
	uvc_device_handle_t *devh;
	uvc_stream_ctrl_t ctrl;
	uvc_error_t res;

	if ((res = uvc_init(&ctx, NULL)) < 0) {
		uvc_perror(res, "uvc_init");
		return 1;
	}
	if ((res = uvc_find_device(ctx, &dev, vid, pid, NULL)) < 0) {
		uvc_perror(res, "uvc_find_device");
		return 1;
	}
	if ((res = uvc_open(dev, &devh)) < 0) {
		uvc_perror(res, "uvc_open");
		return 1;
	}
	if (list) {
		uvc_print_diag(devh, stdout);
		uvc_close(devh);
		uvc_exit(ctx);
		return 0;
	}
	res = uvc_get_stream_ctrl_format_size(devh, &ctrl, UVC_FRAME_FORMAT_MJPEG, width, height, fps);
	if (res < 0) {
		uvc_perror(res, "no matching MJPEG mode (try -l)");
		return 1;
	}
	if ((res = uvc_start_streaming(devh, &ctrl, on_frame, NULL, 0)) < 0) {
		uvc_perror(res, "uvc_start_streaming");
		return 1;
	}
	fprintf(stderr, "uvcstream: %dx%d@%d MJPEG on :%d\n", width, height, fps, port);

	int ls = socket(AF_INET6, SOCK_STREAM, 0);
	int one = 1, zero = 0;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
	struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = htons(port),
				   .sin6_addr = IN6ADDR_ANY_INIT };
	if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 16)) {
		perror("bind/listen");
		return 1;
	}

	/* Watchdog: if the camera stops delivering frames, exit so the service
	 * manager (procd) restarts us and re-enumerates the device. */
	uint64_t last_seq = 0;
	time_t last_change = time(NULL);

	for (;;) {
		fd_set rfds;
		struct timeval tv = { 2, 0 };
		FD_ZERO(&rfds);
		FD_SET(ls, &rfds);
		if (select(ls + 1, &rfds, NULL, NULL, &tv) > 0) {
			int fd = accept(ls, NULL, NULL);
			if (fd >= 0) {
				pthread_mutex_lock(&clients_lock);
				int ok = clients < MAX_CLIENTS;
				if (ok)
					clients++;
				pthread_mutex_unlock(&clients_lock);
				pthread_t t;
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
				if (!ok || pthread_create(&t, NULL, serve, (void *)(intptr_t)fd)) {
					if (ok) {
						pthread_mutex_lock(&clients_lock);
						clients--;
						pthread_mutex_unlock(&clients_lock);
					}
					close(fd);
				} else {
					pthread_detach(t);
				}
			}
		}
		pthread_mutex_lock(&frame.lock);
		uint64_t s = frame.seq;
		pthread_mutex_unlock(&frame.lock);
		if (s != last_seq) {
			last_seq = s;
			last_change = time(NULL);
		} else if (time(NULL) - last_change > 10) {
			fprintf(stderr, "uvcstream: no frames for 10s, exiting\n");
			pthread_mutex_lock(&frame.lock);
			frame.dead = 1;
			pthread_cond_broadcast(&frame.cond);
			pthread_mutex_unlock(&frame.lock);
			return 1;
		}
	}
}
