// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the tinker-edge-r project contributors
/* npu-daemon: shares the NPU of the RK3399Pro between the programs of the board.
 *
 * Over USB only one program can claim the channel to the NPU die (the vendor interface 0xff / 0x4e / 0x50 of 2207:0019). The daemon holds it and
 * listens on a unix socket (/run/npu/npu.sock, group plugdev); the Python module rk3399pro_npu connects to the socket when it exists, so any number
 * of programs can load models, attach to the models of others and run them at the same time. Messages (npu-open/server/protocol.h) are passed on
 * unchanged except their tag, which the daemon replaces with its own (the tags of two programs may be equal) and puts back in the reply.
 * When the NPU goes away (its firmware is reloaded) the requests in flight fail with an error reply and the daemon waits for it to come back.
 *
 *   npu-daemon [--socket PATH]
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol.h"

static const int MAX_RUNS_IN_FLIGHT = 2;     /* RUN requests on the bus at once (the server queues a few; more only lengthens the queue) */
/* A request sent right after a reply while other requests are in flight (pipelining, as async_exec or several programs do) was seen to hang
 * the USB of this SoC, and its xHCI with it; a pause of 300 us after the last reply avoids it (cause not found). One request at a time (exec)
 * never hung: the pause is made only when another request is on its way. */
static int g_gap_us = 300;
static const size_t MAX_MSG = 96u << 20;

static void logf_(const char *fmt, ...)
{
	va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); fputc('\n', stderr); fflush(stderr); va_end(a);
}
static uint64_t now_us() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

/* ---------------- a client (a program on the board) ---------------- */
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
		for (int i = 0; i < 2; i++) while (len[i]) {
			ssize_t w = ::send(fd, q[i], len[i], MSG_NOSIGNAL);
			if (w < 0) { if (errno == EINTR) continue; gone = true; return; }
			q[i] += w; len[i] -= w;
		}
	}
	void error(uint16_t type, uint32_t tag, const std::string &t)
	{
		npu_hdr h = { NPU_MAGIC, type, NPU_FLAG_ERROR, tag, 0, (uint32_t)t.size() };
		send(h, t.data(), t.size());
	}
};

/* ---------------- the USB side ---------------- */
struct Pending { std::shared_ptr<Client> cl; uint32_t tag; uint16_t type; };
static std::mutex g_mu;                        /* g_pending, g_tag, g_usb_fd state */
static std::condition_variable g_cv;           /* USB came up / a RUN slot is free */
static std::unordered_map<uint32_t, Pending> g_pending;
static uint32_t g_tag = 0;
static int g_runs = 0;
static int g_usb_fd = -1;
static unsigned g_ep_in, g_ep_out, g_iface;
static std::mutex g_usb_wmu;
static volatile uint64_t g_last_reply_us;

static int bulk(unsigned ep, void *buf, unsigned len, unsigned timeout_ms)
{
	struct usbdevfs_bulktransfer bt; bt.ep = ep; bt.len = len; bt.timeout = timeout_ms; bt.data = buf;
	return ioctl(g_usb_fd, USBDEVFS_BULK, &bt);
}

/* the reply channel: one asynchronous bulk read, waited for in poll(). A blocking USBDEVFS_BULK without a timeout waits uninterruptibly in the
 * kernel, and the freezer of system sleep cannot stop the daemon there: suspend then fails after 20 s ("1 tasks refusing to freeze"). A timeout
 * would cancel transfers over and over (a 'stop endpoint' command each, which the xHCI of this SoC can hang on). The read stays submitted, poll()
 * is an interruptible wait. Returns the bytes read, or -1 with errno (ENODEV: the NPU went away). */
static int bulk_in(unsigned ep, void *buf, unsigned len)
{
	struct usbdevfs_urb urb;
	memset(&urb, 0, sizeof urb);
	urb.type = USBDEVFS_URB_TYPE_BULK; urb.endpoint = ep; urb.buffer = buf; urb.buffer_length = len;
	if (ioctl(g_usb_fd, USBDEVFS_SUBMITURB, &urb) < 0) return -1;
	for (;;) {
		struct pollfd pf = { g_usb_fd, POLLOUT, 0 };
		if (poll(&pf, 1, -1) < 0 && errno != EINTR) return -1;
		void *done = nullptr;
		if (ioctl(g_usb_fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
			if (done != &urb) continue;
			if (urb.status < 0) { errno = -urb.status; return -1; }
			return urb.actual_length;
		}
		if (errno != EAGAIN && errno != EINTR) {   /* the device is gone: the read is given back when the file is closed */
			int e = errno; ioctl(g_usb_fd, USBDEVFS_DISCARDURB, &urb); errno = e;
			return -1;
		}
	}
}

static std::string read_sys(const std::string &path)
{
	std::string s; FILE *f = fopen(path.c_str(), "rb"); if (!f) return s;
	char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\0')) s.pop_back();
	return s;
}

/* the NPU die on the bus: open its device node and claim the open channel; false while it is not there */
static bool usb_open()
{
	DIR *d = opendir("/sys/bus/usb/devices"); if (!d) return false;
	struct dirent *e; std::string dev;
	while ((e = readdir(d))) {
		std::string p = std::string("/sys/bus/usb/devices/") + e->d_name;
		if (read_sys(p + "/idVendor") == "2207" && read_sys(p + "/idProduct") == "0019") { dev = p; break; }
	}
	closedir(d);
	if (dev.empty()) return false;
	std::string raw;
	{ FILE *f = fopen((dev + "/descriptors").c_str(), "rb"); if (!f) return false; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) raw.append(b, n); fclose(f); }
	int iface = -1; std::vector<unsigned> eps; bool cur = false;
	for (size_t i = 18; i + 2 <= raw.size();) {
		unsigned len = (uint8_t)raw[i], typ = (uint8_t)raw[i + 1];
		if (typ == 4 && i + 8 <= raw.size()) { cur = (uint8_t)raw[i + 5] == 0xff && (uint8_t)raw[i + 6] == 0x4e && (uint8_t)raw[i + 7] == 0x50; if (cur) iface = (uint8_t)raw[i + 2]; }
		else if (typ == 5 && cur && i + 3 <= raw.size()) eps.push_back((uint8_t)raw[i + 2]);
		i += len ? len : 1;
	}
	if (iface < 0 || eps.size() != 2) { logf_("the NPU does not offer the open channel: its firmware is not ours"); return false; }
	int bus = atoi(read_sys(dev + "/busnum").c_str()), num = atoi(read_sys(dev + "/devnum").c_str());
	char node[64]; snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", bus, num);
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) { logf_("cannot open %s: %s", node, strerror(errno)); return false; }
	unsigned ifc = iface;
	if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) { logf_("cannot claim the interface: %s", strerror(errno)); close(fd); return false; }
	g_iface = ifc;
	g_ep_out = (eps[0] & 0x80) ? eps[1] : eps[0];
	g_ep_in = (eps[0] & 0x80) ? eps[0] : eps[1];
	std::lock_guard<std::mutex> lk(g_mu);
	g_usb_fd = fd;
	g_cv.notify_all();
	return true;
}

static void fail_all(const char *why)
{
	std::unordered_map<uint32_t, Pending> lost;
	{ std::lock_guard<std::mutex> lk(g_mu); lost.swap(g_pending); g_runs = 0; g_cv.notify_all(); }
	for (auto &kv : lost) kv.second.cl->error(kv.second.type, kv.second.tag, why);
}

/* reads replies from the NPU and hands them to their programs */
static void usb_loop()
{
	std::vector<uint8_t> buf(64 * 1024), rx;
	for (;;) {
		while (!usb_open()) sleep(1);
		logf_("NPU connected");
		rx.clear();
		for (;;) {
			int n = bulk_in(g_ep_in, buf.data(), buf.size());   /* no timeout, but freezable for system sleep (see bulk_in) */
			if (n < 0) { if (errno == EINTR || errno == ETIMEDOUT) continue; logf_("NPU lost: %s", strerror(errno)); break; }
			rx.insert(rx.end(), buf.begin(), buf.begin() + n);
			size_t off = 0;
			while (rx.size() - off >= sizeof(npu_hdr)) {
				npu_hdr h; memcpy(&h, rx.data() + off, sizeof h);
				if (h.magic != NPU_MAGIC) { off++; continue; }   /* stray bytes of an earlier connection: look for the next header */
				size_t total = sizeof h + h.len + ((h.flags & NPU_FLAG_PAD) ? 1 : 0);
				if (rx.size() - off < total) break;
				g_last_reply_us = now_us();
				Pending p; bool found = false;
				{ std::lock_guard<std::mutex> lk(g_mu);
				  auto it = g_pending.find(h.tag);
				  if (it != g_pending.end()) { p = it->second; found = true; g_pending.erase(it); if (p.type == NPU_RUN) { g_runs--; g_cv.notify_all(); } } }
				if (found) { npu_hdr o = h; o.tag = p.tag; o.flags &= NPU_FLAG_ERROR; p.cl->send(o, rx.data() + off + sizeof h, h.len); }
				off += total;
			}
			rx.erase(rx.begin(), rx.begin() + off);
		}
		{ std::lock_guard<std::mutex> lk(g_mu); unsigned ifc = g_iface; ioctl(g_usb_fd, USBDEVFS_RELEASEINTERFACE, &ifc); close(g_usb_fd); g_usb_fd = -1; }
		fail_all("the NPU went away (its firmware is reloaded?); the models on it are gone");
		sleep(1);
	}
}

static void forward(const std::shared_ptr<Client> &cl, const npu_hdr &h, const std::vector<uint8_t> &payload)
{
	uint32_t dtag; bool busy;
	{
		std::unique_lock<std::mutex> lk(g_mu);
		if (!g_cv.wait_for(lk, std::chrono::seconds(10), [] { return g_usb_fd >= 0; })) {
			lk.unlock(); cl->error(h.type, h.tag, "the NPU is not on the USB bus (is its firmware loaded? systemctl status npu-firmware)"); return;
		}
		if (h.type == NPU_RUN) { g_cv.wait(lk, [] { return g_runs < MAX_RUNS_IN_FLIGHT || g_usb_fd < 0; }); g_runs++; }
		busy = !g_pending.empty();
		do dtag = g_tag = (g_tag + 1) & 0x7fffffff; while (!dtag || g_pending.count(dtag));
		g_pending[dtag] = Pending{ cl, h.tag, h.type };
	}
	/* one stream per message, never ending on a multiple of 1024 bytes (see NPU_FLAG_PAD) */
	bool pad = (sizeof(npu_hdr) + payload.size()) % 1024 == 0;
	std::vector<uint8_t> msg(sizeof(npu_hdr) + payload.size() + (pad ? 1 : 0));
	npu_hdr o = h; o.tag = dtag; o.flags = pad ? NPU_FLAG_PAD : 0; o.len = payload.size();
	memcpy(msg.data(), &o, sizeof o);
	if (!payload.empty()) memcpy(msg.data() + sizeof o, payload.data(), payload.size());
	if (pad) msg.back() = 0;
	bool ok = true;
	{
		std::lock_guard<std::mutex> lk(g_usb_wmu);
		int64_t gap = (int64_t)g_gap_us - (int64_t)(now_us() - g_last_reply_us);
		if (busy && gap > 0) usleep(gap);
		for (size_t sent = 0; ok && sent < msg.size();) {
			unsigned n = msg.size() - sent < 256 * 1024 ? msg.size() - sent : 256 * 1024;
			int r = g_usb_fd >= 0 ? bulk(g_ep_out, msg.data() + sent, n, 60000) : -1;
			if (r <= 0) { if (r < 0 && errno == EINTR) continue; ok = false; break; }
			sent += r;
		}
	}
	if (!ok) {
		bool mine;
		{ std::lock_guard<std::mutex> lk(g_mu); mine = g_pending.erase(dtag); if (mine && h.type == NPU_RUN) { g_runs--; g_cv.notify_all(); } }
		if (mine) cl->error(h.type, h.tag, std::string("USB write failed: ") + strerror(errno));
	}
}

static void client_loop(std::shared_ptr<Client> cl)
{
	std::vector<uint8_t> payload;
	for (;;) {
		npu_hdr h;
		auto rd = [&](void *b, size_t n) {
			uint8_t *p = (uint8_t *)b;
			while (n) { ssize_t r = recv(cl->fd, p, n, 0); if (r < 0 && errno == EINTR) continue; if (r <= 0) return false; p += r; n -= r; }
			return true;
		};
		if (!rd(&h, sizeof h) || h.magic != NPU_MAGIC || h.len > MAX_MSG) break;
		payload.resize(h.len);
		if (h.len && !rd(payload.data(), h.len)) break;
		if (h.flags & NPU_FLAG_PAD) { uint8_t z; if (!rd(&z, 1)) break; }
		forward(cl, h, payload);
	}
	{ std::lock_guard<std::mutex> lk(cl->wmu); cl->gone = true; }   /* replies still on their way are dropped (and RUN slots still freed) */
	close(cl->fd);
}

int main(int argc, char **argv)
{
	const char *path = "/run/npu/npu.sock";
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--socket") && i + 1 < argc) path = argv[++i];
	if (getenv("NPU_DAEMON_GAP_US")) g_gap_us = atoi(getenv("NPU_DAEMON_GAP_US"));
	signal(SIGPIPE, SIG_IGN);
	int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	unlink(path);
	mode_t old = umask(007);
	if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 16) < 0) { logf_("cannot listen on %s: %s", path, strerror(errno)); return 1; }
	umask(old);
	if (struct group *g = getgrnam("plugdev")) if (chown(path, -1, g->gr_gid) < 0) logf_("chown %s: %s", path, strerror(errno));
	logf_("listening on %s", path);
	std::thread(usb_loop).detach();
	for (;;) {
		int c = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
		if (c < 0) { if (errno == EINTR) continue; logf_("accept: %s", strerror(errno)); sleep(1); continue; }
		std::thread(client_loop, std::make_shared<Client>(c)).detach();
	}
}
