// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the tinker-edge-r project contributors
/* The client side of npu-daemon: the socket of one program on the board (npu_daemon.cc). A client that stops reading its replies must not hold
 * up the daemon, so send() delivers one whole reply within a monotonic deadline: every write is non-blocking (MSG_DONTWAIT on a blocking socket),
 * partial writes are advanced, and on the deadline the connection is marked gone and woken with shutdown (the close stays with client_loop). */
#pragma once

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>

#include <mutex>
#include <string>

#include "protocol.h"   // npu_hdr

static inline uint64_t now_us()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* default 10 s; NPU_DAEMON_DELIVER_TIMEOUT_US overrides it (tests) */
static inline uint64_t reply_deliver_timeout_us()
{
	// a const function-local static: the magic-static initialization runs once and synchronizes it
	// with these later reads, so two clients' first sends (each under its own wmu) cannot race
	static const uint64_t us = [] {
		const char *e = getenv("NPU_DAEMON_DELIVER_TIMEOUT_US");
		char *end = nullptr;
		uint64_t v = (e && *e >= '0' && *e <= '9') ? strtoull(e, &end, 10) : 0;
		// A negative poll timeout would block indefinitely.
		return v && !*end && v <= uint64_t(INT_MAX) * 1000 ? v : uint64_t(10000000);
	}();
	return us;
}

struct Client {
	int fd;
	std::mutex wmu;
	bool gone = false;
	explicit Client(int f) : fd(f) {}
	void send(const npu_hdr &h, const void *p, size_t n)
	{
		std::lock_guard<std::mutex> lk(wmu);
		if (gone) return;
		const uint8_t *q[2] = { (const uint8_t *)&h, (const uint8_t *)p }; size_t len[2] = { sizeof h, n };
		uint64_t deadline = now_us() + reply_deliver_timeout_us();   /* one whole reply: partial writes, EINTR and EAGAIN included */
		for (int i = 0; i < 2; i++) while (len[i]) {
			uint64_t now = now_us();
			if (now >= deadline) { gone = true; break; }
			ssize_t w = ::send(fd, q[i], len[i], MSG_NOSIGNAL | MSG_DONTWAIT);
			if (w >= 0) { q[i] += w; len[i] -= w; continue; }
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pf = { fd, POLLOUT, 0 };
				int pr = poll(&pf, 1, (int)((deadline - now + 999) / 1000));   // the remainder is computed after the deadline check above
				if (pr < 0) { if (errno == EINTR) continue; gone = true; break; }
				if (pr == 0 || (pf.revents & (POLLERR | POLLHUP | POLLNVAL))) { gone = true; break; }
				continue;
			}
			gone = true;
			break;
		}
		if (gone) shutdown(fd, SHUT_RDWR);   /* wake the thread that reads this connection; the close stays with its owner in client_loop */
	}
	void error(uint16_t type, uint32_t tag, const std::string &t)
	{
		npu_hdr h = { NPU_MAGIC, type, NPU_FLAG_ERROR, tag, 0, (uint32_t)t.size() };
		send(h, t.data(), t.size());
	}
};
