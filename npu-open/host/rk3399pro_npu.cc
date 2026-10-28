// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the tinker-edge-r project contributors
// rk3399pro_npu: the C++ library (see rk3399pro_npu.hpp). The connection, the protocol (npu-open/server/protocol.h), the compiled-graph cache.
#include "rk3399pro_npu.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "protocol.h"
#include "op_names.h"
#include "sha256.h"

namespace rk3399pro_npu {

static const char *DEFAULT_SOCKET = "/run/npu/npu.sock";
static const size_t MAX_MSG = 96u << 20;

std::string sha256_hex(const void *data, size_t n) { return npu_sha256::hex(data, n); }

static std::string read_file(const std::string &path, bool trim = true)
{
	std::string s; FILE *f = fopen(path.c_str(), "rb"); if (!f) return s;
	char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f);
	while (trim && !s.empty() && (s.back() == '\n' || s.back() == '\0')) s.pop_back();
	return s;
}
static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

std::vector<Device> find_devices()
{
	std::vector<Device> v;
	DIR *d = opendir("/sys/bus/usb/devices"); if (!d) return v;
	std::vector<std::string> names; struct dirent *e;
	while ((e = readdir(d))) names.push_back(e->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());
	for (auto &n : names) {
		std::string p = "/sys/bus/usb/devices/" + n;
		if (read_file(p + "/idVendor") != "2207" || read_file(p + "/idProduct") != "0019") continue;
		Device dv; dv.bus = atoi(read_file(p + "/busnum").c_str()); dv.dev = atoi(read_file(p + "/devnum").c_str());
		dv.serial = read_file(p + "/serial"); dv.sysfs = n;
		v.push_back(dv);
	}
	return v;
}

// ---------------------------------------------------------------- the connection
class Link {
public:
	using Cb = std::function<void(std::vector<uint8_t> &&data, uint32_t model, const std::string &err)>;
	std::string via;

	static std::shared_ptr<Link> shared()
	{
		static std::mutex mu; static std::weak_ptr<Link> cur;
		std::lock_guard<std::mutex> lk(mu);
		auto s = cur.lock();
		if (s && !s->dead_) return s;
		s = std::shared_ptr<Link>(new Link());
		cur = s;
		return s;
	}

	~Link()
	{
		closing_ = true;
		if (sock_ >= 0) { shutdown(sock_, SHUT_RDWR); if (reader_.joinable()) reader_.join(); close(sock_); }
		else if (usb_ >= 0) {
			try { std::string bye = "bye"; send(NPU_PING, bye.data(), bye.size(), 0, next_tag()); } catch (...) {}   // the answer wakes the reader, which then ends
			if (reader_.joinable()) reader_.join();
			unsigned ifc = iface_; ioctl(usb_, USBDEVFS_RELEASEINTERFACE, &ifc); close(usb_);
		} else if (reader_.joinable()) reader_.join();
	}

	void submit(uint16_t type, const void *p, size_t n, uint32_t model, Cb cb)
	{
		uint32_t tag = next_tag();
		{ std::lock_guard<std::mutex> lk(mu_); if (dead_) throw Error(dead_why_); pending_[tag] = std::move(cb); }
		try { send(type, p, n, model, tag); }
		catch (...) { std::lock_guard<std::mutex> lk(mu_); pending_.erase(tag); throw; }
	}

	struct Reply { std::vector<uint8_t> data; uint32_t model = 0; };
	Reply call(uint16_t type, const void *p = nullptr, size_t n = 0, uint32_t model = 0, int timeout_s = 30)
	{
		struct Box { std::mutex mu; std::condition_variable cv; bool done = false; Reply r; std::string err; };
		auto box = std::make_shared<Box>();
		uint32_t tag = next_tag();
		{ std::lock_guard<std::mutex> lk(mu_); if (dead_) throw Error(dead_why_);
		  pending_[tag] = [box](std::vector<uint8_t> &&d, uint32_t m, const std::string &e) {
			  std::lock_guard<std::mutex> l(box->mu); box->r.data = std::move(d); box->r.model = m; box->err = e; box->done = true; box->cv.notify_all(); }; }
		try { send(type, p, n, model, tag); }
		catch (...) { std::lock_guard<std::mutex> lk(mu_); pending_.erase(tag); throw; }
		std::unique_lock<std::mutex> l(box->mu);
		if (!box->cv.wait_for(l, std::chrono::seconds(timeout_s), [&] { return box->done; })) {
			l.unlock(); std::lock_guard<std::mutex> lk(mu_); pending_.erase(tag);
			throw Error("the NPU did not answer in " + std::to_string(timeout_s) + " s");
		}
		if (!box->err.empty()) throw Error(box->err);
		return std::move(box->r);
	}

	NpuInfo hello()
	{
		Reply r;
		for (int attempt = 0;; attempt++) {
			try { r = call(NPU_HELLO, nullptr, 0, 0, 3); break; }
			catch (Error &e) { if (attempt == 3 || std::string(e.what()).find("did not answer") == std::string::npos) throw; }
		}
		if (r.data.size() < sizeof(npu_hello)) throw Error("short HELLO reply: the NPU firmware and this library are of different releases");
		npu_hello h; memcpy(&h, r.data.data(), sizeof h);
		if (h.proto != NPU_PROTO_VERSION)
			throw Error("the NPU server speaks protocol " + std::to_string(h.proto) + ", this library " + std::to_string(NPU_PROTO_VERSION) +
				    ": the NPU firmware and the library are of different releases");
		NpuInfo i;
		i.proto = h.proto; i.chip_model = h.chip_model; i.chip_revision = h.chip_revision; i.chip_product = h.chip_product; i.chip_ok = h.chip_ok;
		i.mem_total_kb = h.mem_total_kb; i.mem_avail_kb = h.mem_avail_kb; i.models = h.models;
		i.driver.assign(h.driver, strnlen(h.driver, sizeof h.driver)); i.via = via;
		return i;
	}

private:
	int sock_ = -1, usb_ = -1;
	unsigned ep_in_ = 0, ep_out_ = 0, iface_ = 0;
	std::thread reader_;
	std::mutex mu_, wmu_;
	std::unordered_map<uint32_t, Cb> pending_;
	uint32_t tag_ = 0;
	volatile bool closing_ = false;
	bool dead_ = false;
	std::string dead_why_;

	Link()
	{
		const char *env = getenv("NPU_SOCKET");
		std::string path = env ? env : DEFAULT_SOCKET;
		const char *direct = getenv("NPU_DIRECT");
		if (!(direct && !strcmp(direct, "1")) && !path.empty() && exists(path)) {
			sock_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
			struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
			if (connect(sock_, (struct sockaddr *)&a, sizeof a) < 0) {
				int e = errno; close(sock_); sock_ = -1;
				if (e == EACCES) throw Error("no access to the NPU daemon socket " + path + ": the image gives it to the group plugdev; log in again after being added to it");
				throw Error("the NPU daemon does not answer on " + path + " (" + strerror(e) + "): systemctl status npu-daemon");
			}
			via = "daemon";
		} else {
			open_usb();
			via = "usb";
		}
		reader_ = std::thread([this] { read_loop(); });
	}

	void open_usb()
	{
		auto devs = find_devices();
		if (devs.empty()) throw Error("NPU (USB 2207:0019) not found; is the NPU firmware loaded (systemctl status npu-firmware)?");
		std::string raw = read_file("/sys/bus/usb/devices/" + devs[0].sysfs + "/descriptors", false);
		int iface = -1; std::vector<unsigned> eps; bool cur = false;
		for (size_t i = 18; i + 2 <= raw.size();) {
			unsigned len = (uint8_t)raw[i], typ = (uint8_t)raw[i + 1];
			if (typ == 4 && i + 8 <= raw.size()) { cur = (uint8_t)raw[i + 5] == 0xff && (uint8_t)raw[i + 6] == 0x4e && (uint8_t)raw[i + 7] == 0x50; if (cur) iface = (uint8_t)raw[i + 2]; }
			else if (typ == 5 && cur && i + 3 <= raw.size()) eps.push_back((uint8_t)raw[i + 2]);
			i += len ? len : 1;
		}
		if (iface < 0 || eps.size() != 2) throw Error("the NPU does not offer the open channel (interface 0xff/0x4e/0x50): its firmware is not ours");
		ep_out_ = (eps[0] & 0x80) ? eps[1] : eps[0]; ep_in_ = (eps[0] & 0x80) ? eps[0] : eps[1];
		char node[64]; snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", devs[0].bus, devs[0].dev);
		usb_ = open(node, O_RDWR | O_CLOEXEC);
		if (usb_ < 0) {
			if (errno == EACCES) throw Error(std::string("no access to ") + node + ": the image gives it to the group plugdev (udev rule 60-tinker-npu.rules); log in again after being added to the group, or run as root");
			throw Error(std::string("cannot open ") + node + ": " + strerror(errno));
		}
		unsigned ifc = iface;
		if (ioctl(usb_, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
			int e = errno; close(usb_); usb_ = -1;
			throw Error(std::string("cannot claim the USB interface (") + strerror(e) + "): another program talks to the NPU directly; use the daemon (systemctl start npu-daemon) to share it");
		}
		iface_ = ifc;
	}

	int bulk(unsigned ep, void *buf, unsigned len, unsigned timeout_ms)
	{
		struct usbdevfs_bulktransfer bt; bt.ep = ep; bt.len = len; bt.timeout = timeout_ms; bt.data = buf;
		return ioctl(usb_, USBDEVFS_BULK, &bt);
	}

	uint32_t next_tag() { std::lock_guard<std::mutex> lk(mu_); tag_ = (tag_ + 1) & 0x7fffffff; if (!tag_) tag_ = 1; return tag_; }

	void send(uint16_t type, const void *p, size_t n, uint32_t model, uint32_t tag)
	{
		bool pad = usb_ >= 0 && (sizeof(npu_hdr) + n) % 1024 == 0;   // one stream per message, never ending on a multiple of 1024 bytes (NPU_FLAG_PAD)
		npu_hdr h = { NPU_MAGIC, type, (uint16_t)(pad ? NPU_FLAG_PAD : 0), tag, model, (uint32_t)n };
		std::lock_guard<std::mutex> lk(wmu_);
		if (sock_ >= 0) {
			struct iovec_ { const uint8_t *p; size_t n; } parts[2] = { { (const uint8_t *)&h, sizeof h }, { (const uint8_t *)p, n } };
			for (auto &pt : parts) while (pt.n) {
				ssize_t w = ::send(sock_, pt.p, pt.n, MSG_NOSIGNAL);
				if (w < 0) { if (errno == EINTR) continue; throw Error(std::string("the connection to the NPU daemon is lost (") + strerror(errno) + ")"); }
				pt.p += w; pt.n -= w;
			}
			return;
		}
		std::vector<uint8_t> msg(sizeof h + n + (pad ? 1 : 0));
		memcpy(msg.data(), &h, sizeof h);
		if (n) memcpy(msg.data() + sizeof h, p, n);
		if (pad) msg.back() = 0;
		for (size_t sent = 0; sent < msg.size();) {
			unsigned c = msg.size() - sent < 256 * 1024 ? msg.size() - sent : 256 * 1024;
			int r = bulk(ep_out_, msg.data() + sent, c, 60000);
			if (r < 0 && errno == EINTR) continue;
			if (r <= 0) throw Error(std::string("USB write failed (") + strerror(errno) + ")");
			sent += r;
		}
	}

	void fail_all(const std::string &why)
	{
		std::unordered_map<uint32_t, Cb> lost;
		{ std::lock_guard<std::mutex> lk(mu_); dead_ = true; dead_why_ = why; lost.swap(pending_); }
		for (auto &kv : lost) { try { kv.second({}, 0, why); } catch (...) {} }
	}

	void read_loop()
	{
		std::vector<uint8_t> buf(256 * 1024), rx;
		size_t off = 0;
		while (!closing_) {
			ssize_t n;
			if (sock_ >= 0) {
				n = recv(sock_, buf.data(), buf.size(), 0);
				if (n < 0 && errno == EINTR) continue;
				if (n <= 0) { if (!closing_) fail_all("the NPU daemon closed the connection"); return; }
			} else {
				n = bulk(ep_in_, buf.data(), 64 * 1024, 0);   // no timeout: a cancelled transfer is a 'stop endpoint' command and the xHCI of this SoC can hang on a storm of them
				if (n < 0 && (errno == EINTR || errno == ETIMEDOUT)) continue;
				if (n < 0) { if (!closing_) fail_all(std::string("USB read failed (") + strerror(errno) + ")"); return; }
			}
			rx.insert(rx.end(), buf.begin(), buf.begin() + n);
			while (rx.size() - off >= sizeof(npu_hdr)) {
				npu_hdr h; memcpy(&h, rx.data() + off, sizeof h);
				if (h.magic != NPU_MAGIC) { off++; continue; }   // stray bytes of an earlier program: skip to the next header
				if (h.len > MAX_MSG) { off++; continue; }
				size_t total = sizeof h + h.len + ((h.flags & NPU_FLAG_PAD) ? 1 : 0);
				if (rx.size() - off < total) break;
				Cb cb;
				{ std::lock_guard<std::mutex> lk(mu_); auto it = pending_.find(h.tag); if (it != pending_.end()) { cb = std::move(it->second); pending_.erase(it); } }
				if (cb) {
					const uint8_t *p = rx.data() + off + sizeof h;
					try {
						if (h.flags & NPU_FLAG_ERROR) cb({}, h.model, std::string((const char *)p, h.len));
						else cb(std::vector<uint8_t>(p, p + h.len), h.model, std::string());
					} catch (...) {}
				}
				off += total;
			}
			if (off) { rx.erase(rx.begin(), rx.begin() + off); off = 0; }
		}
	}
};

// ---------------------------------------------------------------- module functions
static std::vector<ModelInfo> list_models(Link &l)
{
	auto r = l.call(NPU_LIST, nullptr, 0, 0, 10);
	std::vector<ModelInfo> v;
	if (r.data.size() < 4) return v;
	uint32_t n; memcpy(&n, r.data.data(), 4);
	for (uint32_t i = 0; i < n && 4 + (i + 1) * sizeof(npu_model_entry) <= r.data.size(); i++) {
		npu_model_entry e; memcpy(&e, r.data.data() + 4 + i * sizeof e, sizeof e);
		ModelInfo m; m.id = e.id; m.inputs = e.n_inputs; m.outputs = e.n_outputs; m.load_ms = e.load_ms; m.mem_used_kb = e.mem_used_kb;
		m.runs = e.runs; m.age_s = e.age_s; m.from_cache = e.from_nbg;
		m.key.assign(e.key, strnlen(e.key, sizeof e.key)); m.name.assign(e.name, strnlen(e.name, sizeof e.name)); m.plan.assign(e.plan, strnlen(e.plan, sizeof e.plan));
		v.push_back(m);
	}
	return v;
}

NpuInfo info() { return Link::shared()->hello(); }
std::vector<ModelInfo> models() { auto l = Link::shared(); return list_models(*l); }

int stop(uint32_t id)
{
	auto l = Link::shared();
	try { l->call(NPU_UNLOAD, nullptr, 0, id, 30); return 1; }
	catch (Error &e) { if (std::string(e.what()).find("no such model") != std::string::npos) return 0; throw; }
}

int stop(const std::string &which)
{
	auto l = Link::shared();
	auto ms = list_models(*l);
	if (which == "all") { l->call(NPU_UNLOAD, nullptr, 0, NPU_ALL_MODELS, 60); return (int)ms.size(); }
	int n = 0;
	for (auto &m : ms) if (m.name == which || m.key == which) n += stop(m.id);
	return n;
}

// ---------------------------------------------------------------- the model
struct Model::Slots {   // async_exec: at most max_inflight runs on their way
	std::mutex mu; std::condition_variable cv; int free = 2;
	std::atomic<uint32_t> last_us{0};   // last_invoke_us, shared with the callbacks of async_exec (they may outlive the Model)
	void acquire() { std::unique_lock<std::mutex> l(mu); cv.wait(l, [&] { return free > 0; }); free--; }
	void release() { std::lock_guard<std::mutex> l(mu); free++; cv.notify_one(); }
};

Model::Model(const std::string &path, const LoadOptions &opt)
{
	std::string data = read_file(path, false);
	if (data.empty() && !exists(path)) throw Error("cannot read " + path);
	size_t sl = path.find_last_of('/');
	init(std::vector<uint8_t>(data.begin(), data.end()), opt, sl == std::string::npos ? path : path.substr(sl + 1));
}

Model::Model(const std::vector<uint8_t> &tflite, const LoadOptions &opt) { init(tflite, opt, "model"); }

Model::~Model() { try { close(); } catch (...) {} }

// ---- the plan of a load: where the model runs ("npu", "npu-nosplit", "cpu", "mixed:<codes>[:nosplit]")
struct Plan {
	bool cpu = false, split = true;
	double ratio = 4;                 // split ratio of per-channel layers
	double err = 0;                   // > 0: the ratio per layer from this weight error (see split_per_channel.h)
	std::vector<int> deny;            // operator types on the CPU
	std::vector<std::pair<int, int>> nodes;   // ranges of nodes on the CPU (first, last; last -1: to the end)
	static std::string ranges_text(const std::vector<std::pair<int, int>> &r)
	{
		std::string s;
		for (auto &x : r) s += (s.empty() ? "" : ",") + std::to_string(x.first) + (x.second == x.first ? "" : "-" + (x.second < 0 ? std::string() : std::to_string(x.second)));
		return s;
	}
	std::string tag() const
	{
		if (cpu) return "cpu";
		char r[48] = ""; if (split && ratio != 4) snprintf(r, sizeof r, "r%g", ratio);
		if (split && err > 0) snprintf(r + strlen(r), sizeof r - strlen(r), "%se%g", r[0] ? "-" : "", err);
		if (deny.empty() && nodes.empty()) return !split ? "npu-nosplit" : r[0] ? std::string("npu-") + r : "npu";
		std::string s = "mixed:";
		for (size_t i = 0; i < deny.size(); i++) s += (i ? "," : "") + std::to_string(deny[i]);
		if (!split) s += ":nosplit"; else if (r[0]) s += std::string(":") + r;
		if (!nodes.empty()) s += ":cpu=" + ranges_text(nodes);
		return s;
	}
	static Plan parse(const std::string &t)
	{
		Plan p;
		if (t == "cpu") { p.cpu = true; return p; }
		if (t == "npu-nosplit") { p.split = false; return p; }
		if (t.compare(0, 4, "npu-") == 0) { p.parse_split(t.substr(4)); return p; }
		if (t.compare(0, 6, "mixed:") == 0) {
			std::string r = t.substr(6);
			size_t c = r.find(':');
			std::string codes = r.substr(0, c), rest = c == std::string::npos ? "" : r.substr(c + 1);
			for (const char *q = codes.c_str(); *q;) { char *e; long v = strtol(q, &e, 10); if (e == q) { q++; continue; } p.deny.push_back((int)v); q = e; }
			for (size_t a = 0; a < rest.size();) {
				size_t b = rest.find(':', a); if (b == std::string::npos) b = rest.size();
				std::string k = rest.substr(a, b - a);
				if (k == "nosplit") p.split = false;
				else if (k.size() > 1 && (k[0] == 'r' || k[0] == 'e')) p.parse_split(k);
				else if (k.compare(0, 5, "from=") == 0) p.nodes.push_back({ atoi(k.c_str() + 5), -1 });   // 0.5 pre-release plans
				else if (k.compare(0, 4, "cpu=") == 0)
					for (const char *q = k.c_str() + 4; *q;) {
						char *e; long lo = strtol(q, &e, 10); if (e == q) { q++; continue; }
						long hi = lo;
						if (*e == '-') { q = e + 1; hi = strtol(q, &e, 10); if (e == q) hi = -1; }
						p.nodes.push_back({ (int)lo, (int)hi }); q = e;
					}
				a = b + 1;
			}
		}
		if (!(p.ratio > 1)) p.ratio = 4;
		return p;
	}
	bool whole_npu() const { return !cpu && deny.empty() && nodes.empty(); }   // only then is there a compiled graph of the whole model to cache
	void parse_split(const std::string &k)   // "r2", "e0.01", "r2-e0.01"
	{
		for (size_t a = 0; a < k.size();) {
			size_t b = k.find('-', a); if (b == std::string::npos) b = k.size();
			if (k[a] == 'r') ratio = atof(k.c_str() + a + 1); else if (k[a] == 'e') err = atof(k.c_str() + a + 1);
			a = b + 1;
		}
		if (!(ratio > 1)) ratio = 4;
		if (!(err > 0)) err = 0;
	}
};

// ---- a few fields of the TFLite flatbuffer (schema: Model.subgraphs = field 2, Model.buffers = 4; SubGraph.tensors = 0; Tensor.type = 1, Tensor.buffer = 2;
// Buffer.data = 0, Buffer.size = 2), read without the TensorFlow headers
namespace {
struct Fb {
	const uint8_t *b; size_t n;
	bool ok(size_t o, size_t len) const { return o + len <= n && o + len >= o; }
	uint32_t u32(size_t o) const { uint32_t v = 0; if (ok(o, 4)) memcpy(&v, b + o, 4); return v; }
	uint64_t u64(size_t o) const { uint64_t v = 0; if (ok(o, 8)) memcpy(&v, b + o, 8); return v; }
	uint16_t u16(size_t o) const { uint16_t v = 0; if (ok(o, 2)) memcpy(&v, b + o, 2); return v; }
	size_t field(size_t table, int i) const   // absolute position of field i of a table, 0 if absent
	{
		if (!ok(table, 4)) return 0;
		int32_t so; memcpy(&so, b + table, 4);
		size_t vt = table - so;
		if (!ok(vt, 4) || 4 + 2 * (size_t)i >= u16(vt)) return 0;
		uint16_t fo = u16(vt + 4 + 2 * i);
		return fo ? table + fo : 0;
	}
	size_t ref(size_t pos) const { return pos ? pos + u32(pos) : 0; }        // follow an offset field
	size_t vec(size_t table, int i, uint32_t *len) const { size_t v = ref(field(table, i)); *len = v ? u32(v) : 0; return v ? v + 4 : 0; }
};
}  // namespace

// a tensor quantised per channel (more than one scale): Tensor.quantization = field 4, QuantizationParameters.scale = field 2
static bool has_per_channel(const std::vector<uint8_t> &d)
{
	Fb f{ d.data(), d.size() };
	size_t root = f.u32(0);
	uint32_t nsg, nt;
	size_t sgs = f.vec(root, 2, &nsg);
	if (!nsg) return false;
	size_t sg = f.ref(sgs), tens = f.vec(sg, 0, &nt);
	for (uint32_t i = 0; i < nt; i++) {
		size_t t = f.ref(tens + 4 * i), qt = f.ref(f.field(t, 4));
		uint32_t ns = 0;
		if (qt) f.vec(qt, 2, &ns);
		if (ns > 1) return true;
	}
	return false;
}

bool is_quantized(const std::vector<uint8_t> &d)
{
	Fb f{ d.data(), d.size() };
	size_t root = f.u32(0);
	uint32_t nsg, nbuf, nt;
	size_t sgs = f.vec(root, 2, &nsg), bufs = f.vec(root, 4, &nbuf);
	if (!nsg) return false;
	size_t sg = f.ref(sgs), tens = f.vec(sg, 0, &nt);
	uint64_t q = 0, fl = 0;
	for (uint32_t i = 0; i < nt; i++) {
		size_t t = f.ref(tens + 4 * i);
		size_t tf = f.field(t, 1), bf = f.field(t, 2);
		int type = tf && f.ok(tf, 1) ? d[tf] : 0;   // TensorType: 0 FLOAT32, 1 FLOAT16, 3 UINT8, 9 INT8
		uint32_t bi = bf ? f.u32(bf) : 0;
		if (!bi || bi >= nbuf) continue;
		size_t bt = f.ref(bufs + 4 * bi);
		uint32_t dl; f.vec(bt, 0, &dl);
		uint64_t sz = dl ? dl : (f.field(bt, 2) ? f.u64(f.field(bt, 2)) : 0);
		if (type == 3 || type == 9) q += sz; else if (type == 0 || type == 1) fl += sz;
	}
	return q > fl;
}

void Model::init(const std::vector<uint8_t> &data, const LoadOptions &opt, const std::string &default_name)
{
	name_ = (opt.name.empty() ? default_name : opt.name).substr(0, sizeof(((npu_load_req *)0)->name) - 1);
	key_ = sha256_hex(data.data(), data.size());
	data_ = data; opt_ = opt;
	gap_us_ = opt.async_gap_us;
	slots_ = std::make_shared<Slots>();
	slots_->free = opt.max_inflight > 0 ? opt.max_inflight : 1;
	link_ = Link::shared();
	try {
		hello_ = link_->hello();
		if (!hello_.chip_ok) {
			char b[160]; snprintf(b, sizeof b, "unexpected NPU chip %x/%x/%x: this software is made for 8000/7100/45080001 only", hello_.chip_model, hello_.chip_revision, hello_.chip_product);
			throw Error(b);
		}
		std::vector<uint8_t> desc;
		for (auto &m : list_models(*link_)) if (m.key == key_ + opt.key_suffix) {
			auto r = link_->call(NPU_DESCRIBE, nullptr, 0, m.id, 10);
			id_ = r.model; desc = std::move(r.data); attached_ = true; plan_ = m.plan;
			break;
		}
		if (!attached_) desc = load(data, opt);
		parse_desc(desc);
	} catch (...) {
		link_.reset();
		throw;
	}
}

void Model::parse_desc(const std::vector<uint8_t> &r)
{
	if (r.size() < sizeof(npu_load_reply)) throw Error("short reply of the NPU");
	npu_load_reply lr; memcpy(&lr, r.data(), sizeof lr);
	if (r.size() < sizeof lr + (size_t)(lr.n_inputs + lr.n_outputs) * sizeof(npu_tensor)) throw Error("short reply of the NPU");
	load_ms_ = lr.load_ms; mem_kb_ = lr.mem_used_kb; attached_ = attached_ || lr.attached;
	inputs_.clear(); outputs_.clear();
	for (uint32_t i = 0; i < lr.n_inputs + lr.n_outputs; i++) {
		npu_tensor t; memcpy(&t, r.data() + sizeof lr + i * sizeof t, sizeof t);
		TensorInfo ti; ti.name.assign(t.name, strnlen(t.name, sizeof t.name)); ti.type = t.type;
		for (int k = 0; k < t.ndims && k < 6; k++) ti.shape.push_back(t.dims[k]);
		ti.scale = t.scale; ti.zero_point = t.zero_point; ti.bytes = t.bytes;
		(i < lr.n_inputs ? inputs_ : outputs_).push_back(ti);
	}
}

static std::string cache_dir_of(const LoadOptions &opt)
{
	if (!opt.use_cache) return "";
	if (!opt.cache_dir.empty()) return opt.cache_dir;
	if (const char *e = getenv("NPU_CACHE")) return e;
	const char *home = getenv("HOME");
	if (!home) { struct passwd *pw = getpwuid(getuid()); home = pw ? pw->pw_dir : "/tmp"; }
	return std::string(home) + "/.cache/npu-models";
}
static void mkdirs(const std::string &d) { for (size_t p = 1; (p = d.find('/', p)) != std::string::npos; p++) mkdir(d.substr(0, p).c_str(), 0755); mkdir(d.c_str(), 0755); }
static bool write_atomic(const std::string &path, const void *p, size_t n)
{
	std::string tmp = path + ".tmp" + std::to_string(getpid());
	FILE *f = fopen(tmp.c_str(), "wb"); if (!f) return false;
	bool ok = fwrite(p, 1, n, f) == n; ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
	return true;
}
// the meta file next to the cached graphs: "<field> <value>" lines. plan: the plan chosen by autotune; <plan>.tflite_mem_kb, <plan>.nbg_mem_kb:
// memory measured at earlier loads with that plan
using Meta = std::map<std::string, std::string>;
static Meta read_meta(const std::string &path)
{
	Meta m; std::string s = read_file(path);
	size_t p = 0;
	while (p < s.size()) {
		size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size();
		std::string l = s.substr(p, e - p); size_t sp = l.find(' ');
		if (sp != std::string::npos) m[l.substr(0, sp)] = l.substr(sp + 1);
		p = e + 1;
	}
	return m;
}
static void write_meta(const std::string &path, const Meta &m)
{
	std::string s; for (auto &kv : m) s += kv.first + " " + kv.second + "\n";
	write_atomic(path, s.data(), s.size());
}

std::string Model::cache_key() const
{
	std::string drv = hello_.driver;
	for (auto &c : drv) if (c == '/') c = '_';
	char chip[40]; snprintf(chip, sizeof chip, "%x_%x_%x", hello_.chip_model, hello_.chip_revision, hello_.chip_product);
	return key_ + "." + drv + "." + chip;
}

std::vector<uint8_t> Model::load(const std::vector<uint8_t> &data, const LoadOptions &opt)
{
	std::string cache = cache_dir_of(opt), ck = cache_key();
	std::string meta_path = cache.empty() ? "" : cache + "/" + ck + ".meta";
	Meta meta = meta_path.empty() ? Meta() : read_meta(meta_path);
	Plan plan; bool strict = false;
	if (opt.device == "cpu") plan.cpu = true;
	else if (opt.device == "npu") {
		if (!(opt.split_ratio > 1 && opt.split_ratio <= 100)) throw Error("split_ratio must be above 1 (and at most 100)");
		plan.split = opt.split; plan.ratio = opt.split_ratio; plan.err = opt.split_max_error; plan.deny = opt.cpu_ops; plan.nodes = opt.cpu_nodes;
		strict = opt.strict && opt.cpu_ops.empty() && opt.cpu_nodes.empty();
	}
	else if (opt.device == "auto" || opt.device.empty()) {
		if (meta.count("plan")) plan = Plan::parse(meta["plan"]);   // found by autotune
		else plan.cpu = !is_quantized(data);                        // float models: the CPU of the NPU die beats the float16 path of this NPU
	} else throw Error("device must be \"auto\", \"npu\" or \"cpu\", not \"" + opt.device + "\"");
	plan_ = plan.tag();
	std::string pt = plan_;
	for (auto &c : pt) if (c == ':' || c == ',') c = '_';
	std::string nbg_path = cache.empty() || !plan.whole_npu() ? "" : cache + "/" + ck + "." + pt + ".nbg";
	std::string nbg = nbg_path.empty() ? "" : read_file(nbg_path, false);
	auto num = [&](const std::string &k) { return meta.count(k) ? atof(meta[k].c_str()) : -1.0; };
	// memory: what this model took last time with this plan (+10 %, +16 MB), or the estimate of the server
	double m_nbg = num(plan_ + ".nbg_mem_kb"), m_tfl = num(plan_ + ".tflite_mem_kb");
	double need = !nbg.empty() ? (m_nbg >= 0 ? m_nbg * 1.1 + 16 * 1024 : nbg.size() / 1024.0 * 2 + 32 * 1024)
				   : (m_tfl >= 0 ? m_tfl * 1.1 + 16 * 1024 : data.size() / 1024.0 * 4 + 64 * 1024);
	if (need > hello_.mem_avail_kb) {
		std::string others;
		for (auto &m : list_models(*link_)) { char b[120]; snprintf(b, sizeof b, "%s%u %s (%u MB)", others.empty() ? "" : ", ", m.id, m.name.c_str(), m.mem_used_kb / 1024); others += b; }
		char b[200]; snprintf(b, sizeof b, "not enough memory on the NPU for %s: it needs about %.0f MB, %u MB are free. Loaded models: ", name_.c_str(), need / 1024, hello_.mem_avail_kb / 1024);
		throw Error(b + (others.empty() ? std::string("none") : others) + "; stop one with stop(id or name)");
	}
	npu_load_req lq; memset(&lq, 0, sizeof lq);
	snprintf(lq.key, sizeof lq.key, "%s", (key_ + opt.key_suffix).c_str()); snprintf(lq.name, sizeof lq.name, "%s", name_.c_str());
	lq.flags = (plan.cpu ? NPU_LOAD_CPU_ONLY : 0) | (plan.split ? 0 : NPU_LOAD_NO_SPLIT) | (strict ? NPU_LOAD_STRICT : 0);
	for (size_t i = 0; i < plan.deny.size() && i < NPU_MAX_DENY; i++) lq.deny_ops[i] = (uint16_t)plan.deny[i];
	lq.split_ratio_x100 = (uint16_t)std::lround(plan.ratio * 100); lq.split_max_err = (float)plan.err;
	if (plan.nodes.size() > NPU_MAX_RANGES) throw Error("at most " + std::to_string(NPU_MAX_RANGES) + " ranges of nodes on the CPU");
	lq.n_cpu_ranges = (uint16_t)plan.nodes.size();
	for (size_t i = 0; i < plan.nodes.size(); i++) { lq.cpu_ranges[2 * i] = (uint16_t)plan.nodes[i].first; lq.cpu_ranges[2 * i + 1] = plan.nodes[i].second < 0 ? 0xffff : (uint16_t)plan.nodes[i].second; }
	if (opt.profile) lq.flags |= NPU_LOAD_PROFILE;
	snprintf(lq.plan, sizeof lq.plan, "%s", plan_.c_str());
	Link::Reply r; bool have = false; std::string mem_field;
	if (!nbg.empty()) {
		std::vector<uint8_t> p(sizeof lq + nbg.size());
		memcpy(p.data(), &lq, sizeof lq); memcpy(p.data() + sizeof lq, nbg.data(), nbg.size());
		try { r = link_->call(NPU_LOAD_NBG, p.data(), p.size(), 0, opt.load_timeout_s); have = true; from_cache_ = true; mem_field = plan_ + ".nbg_mem_kb"; }
		catch (Error &e) {
			if (std::string(e.what()).find("not enough memory") != std::string::npos) throw;
			fprintf(stderr, "the cached graph was refused (%s): building it again\n", e.what());
		}
	}
	if (!have) {
		std::vector<uint8_t> p(sizeof lq + data.size());
		memcpy(p.data(), &lq, sizeof lq); memcpy(p.data() + sizeof lq, data.data(), data.size());
		r = link_->call(NPU_LOAD, p.data(), p.size(), 0, opt.load_timeout_s); mem_field = plan_ + ".tflite_mem_kb";
	}
	id_ = r.model;
	if (r.data.size() < sizeof(npu_load_reply)) throw Error("short reply of the NPU");
	npu_load_reply lr; memcpy(&lr, r.data.data(), sizeof lr);
	size_t tail = sizeof lr + (size_t)(lr.n_inputs + lr.n_outputs) * sizeof(npu_tensor);
	if (lr.attached || r.data.size() < tail + 4) return r.data;   // another program loaded it meanwhile
	uint32_t nbg_len; memcpy(&nbg_len, r.data.data() + tail, 4);
	if (!cache.empty()) {
		mkdirs(cache);
		if (nbg_len && !from_cache_ && !nbg_path.empty() && r.data.size() >= tail + 4 + nbg_len) {
			std::vector<uint8_t> f(8 + (tail - sizeof lr) + nbg_len);   // u32 n_in, u32 n_out, the tensors, the graph: the payload of LOAD_NBG
			memcpy(f.data(), &lr.n_inputs, 4); memcpy(f.data() + 4, &lr.n_outputs, 4);
			memcpy(f.data() + 8, r.data.data() + sizeof lr, tail - sizeof lr);
			memcpy(f.data() + 8 + (tail - sizeof lr), r.data.data() + tail + 4, nbg_len);
			write_atomic(nbg_path, f.data(), f.size());
		}
		meta = read_meta(meta_path);   // another program may have written it meanwhile
		meta[mem_field] = std::to_string(lr.mem_used_kb);
		write_meta(meta_path, meta);
	}
	r.data.resize(tail);
	return r.data;
}

std::vector<OpStat> Model::ops()
{
	if (!link_) throw Error("this Model is closed");
	auto r = link_->call(NPU_OPS, nullptr, 0, id_, 30);
	std::vector<OpStat> v;
	if (r.data.size() < 4) return v;
	uint32_t n; memcpy(&n, r.data.data(), 4);
	for (uint32_t i = 0; i < n && 4 + (i + 1) * sizeof(npu_op_stat) <= r.data.size(); i++) {
		npu_op_stat s; memcpy(&s, r.data.data() + 4 + i * sizeof s, sizeof s);
		OpStat o; o.code = s.code; o.name = builtin_op_name(s.code); o.total = s.total; o.on_npu = s.on_npu;
		v.push_back(o);
	}
	return v;
}

GraphInfo Model::graph()
{
	if (!link_) throw Error("this Model is closed");
	auto r = link_->call(NPU_GRAPH, nullptr, 0, id_, 30);
	GraphInfo g;
	if (r.data.size() < sizeof(npu_graph_head)) return g;
	npu_graph_head gh; memcpy(&gh, r.data.data(), sizeof gh);
	size_t need = sizeof gh + (size_t)gh.n_tensors * 4 + (size_t)gh.n_nodes * sizeof(npu_graph_node);
	if (r.data.size() < need) throw Error("short reply of the NPU");
	g.profile_us = gh.profile_us;
	g.tensor_bytes.resize(gh.n_tensors);
	if (gh.n_tensors) memcpy(g.tensor_bytes.data(), r.data.data() + sizeof gh, 4 * (size_t)gh.n_tensors);
	for (uint32_t i = 0; i < gh.n_nodes; i++) {
		npu_graph_node n; memcpy(&n, r.data.data() + sizeof gh + 4 * (size_t)gh.n_tensors + i * sizeof n, sizeof n);
		GraphNode x; x.code = n.code; x.on_npu = n.on_npu; x.cpu_us = n.cpu_us;
		for (int k = 0; k < n.n_in && k < NPU_GRAPH_MAX_IN; k++) x.in.push_back(n.in[k]);
		for (int k = 0; k < n.n_out && k < NPU_GRAPH_MAX_OUT; k++) x.out.push_back(n.out[k]);
		g.nodes.push_back(x);
	}
	return g;
}

// ---- the cost model of autotune
// The time of a plan is predicted from its placement (every node on the NPU or on the CPU, in execution order):
//   T = a_heavy * (profiled CPU time of the convolutions / FC / matmuls on the NPU) + a_light * (of the other nodes on the NPU)
//     + c_heavy * (... on the CPU) + c_light * (... on the CPU) + cut_us * (cuts between NPU and CPU parts) + byte_us * (bytes crossing the cuts)
// The profile (TensorFlow Lite's own kernels on the graph after our passes) gives the relative weight of every node; the six factors are fitted to
// every plan measured so far (least squares relative to the times, pulled towards priors so that a few measurements suffice). The cheapest
// placement under the model is found exactly by dynamic programming over the execution order (two states: NPU, CPU): that weighs the copies of
// every cut against what a node gains on the other side, and keeps the order of the layers.
namespace {
enum { A_HEAVY, A_LIGHT, C_HEAVY, C_LIGHT, CUT_US, BYTE_US, NPARAM };
bool heavy_op(int c) { return c == 3 || c == 4 || c == 9 || c == 67 || c == 126; }   // CONV_2D, DEPTHWISE_CONV_2D, FULLY_CONNECTED, TRANSPOSE_CONV, BATCH_MATMUL
struct CostModel {
	const GraphInfo &g;
	std::vector<double> live;         // live[b]: bytes of the activations crossing the boundary before node b
	std::vector<char> npu_ok;         // the NPU takes the node (in the whole-NPU plan of this graph)
	double th[NPARAM], prior[NPARAM];
	std::vector<std::pair<std::array<double, NPARAM>, double>> meas;
	explicit CostModel(const GraphInfo &gi) : g(gi)
	{
		size_t n = g.nodes.size();
		std::vector<int> prod(g.tensor_bytes.size(), -1), last(g.tensor_bytes.size(), -1);
		for (size_t i = 0; i < n; i++) {
			for (int t : g.nodes[i].out) if (t >= 0 && (size_t)t < prod.size()) prod[t] = (int)i;
			for (int t : g.nodes[i].in) if (t >= 0 && (size_t)t < last.size()) last[t] = (int)i;
		}
		std::vector<double> diff(n + 2, 0);
		for (size_t t = 0; t < prod.size(); t++)
			if (prod[t] >= 0 && last[t] > prod[t]) { diff[prod[t] + 1] += g.tensor_bytes[t]; diff[last[t] + 1] -= g.tensor_bytes[t]; }
		live.assign(n + 1, 0);
		double acc = 0;
		for (size_t b = 0; b <= n; b++) { acc += diff[b]; live[b] = acc; }
		for (auto &x : g.nodes) npu_ok.push_back(x.on_npu);
		for (int k = 0; k < NPARAM; k++) th[k] = prior[k] = 1;
	}
	std::array<double, NPARAM> features(const std::vector<char> &on_cpu) const
	{
		std::array<double, NPARAM> f{};
		for (size_t i = 0; i < g.nodes.size(); i++) {
			bool c = on_cpu[i] || !npu_ok[i], h = heavy_op(g.nodes[i].code);
			f[c ? (h ? C_HEAVY : C_LIGHT) : (h ? A_HEAVY : A_LIGHT)] += g.nodes[i].cpu_us;
			bool pc = i ? (on_cpu[i - 1] || !npu_ok[i - 1]) : c;
			if (i && pc != c) { f[CUT_US] += 1; f[BYTE_US] += live[i]; }
		}
		return f;
	}
	double predict(const std::array<double, NPARAM> &f) const { double t = 0; for (int k = 0; k < NPARAM; k++) t += th[k] * f[k]; return t; }
	void add(const std::vector<char> &on_cpu, double ms) { if (ms > 0) meas.push_back({ features(on_cpu), ms * 1000 }); }
	// ridge least squares, relative to the measured times, towards the priors
	void fit(double lambda = 0.3)
	{
		double A[NPARAM][NPARAM + 1] = {};
		for (int j = 0; j < NPARAM; j++) { A[j][j] += lambda / (prior[j] * prior[j]); A[j][NPARAM] += lambda / prior[j]; }
		for (auto &m : meas) {
			double w = 1 / (m.second * m.second);
			for (int j = 0; j < NPARAM; j++) {
				for (int k = 0; k < NPARAM; k++) A[j][k] += w * m.first[j] * m.first[k];
				A[j][NPARAM] += w * m.first[j] * m.second;
			}
		}
		for (int c = 0; c < NPARAM; c++) {   // Gauss-Jordan with partial pivoting
			int pv = c;
			for (int r = c + 1; r < NPARAM; r++) if (std::fabs(A[r][c]) > std::fabs(A[pv][c])) pv = r;
			if (std::fabs(A[pv][c]) < 1e-30) return;
			for (int k = 0; k <= NPARAM; k++) std::swap(A[c][k], A[pv][k]);
			for (int r = 0; r < NPARAM; r++) {
				if (r == c) continue;
				double q = A[r][c] / A[c][c];
				for (int k = c; k <= NPARAM; k++) A[r][k] -= q * A[c][k];
			}
		}
		for (int j = 0; j < NPARAM; j++) th[j] = std::max(A[j][NPARAM] / A[j][j], 0.02 * prior[j]);
	}
	// the cheapest placement; forced: nodes that must stay on the CPU (for the accuracy); cut_extra: a penalty per cut (to limit the number of parts)
	std::vector<char> best(const std::vector<char> &forced, double cut_extra = 0) const
	{
		size_t n = g.nodes.size();
		const double INF = 1e300;
		std::vector<std::array<double, 2>> cost(n);
		std::vector<std::array<char, 2>> from(n);
		for (size_t i = 0; i < n; i++) {
			bool h = heavy_op(g.nodes[i].code);
			double t[2] = { npu_ok[i] && !forced[i] ? th[h ? A_HEAVY : A_LIGHT] * g.nodes[i].cpu_us : INF, th[h ? C_HEAVY : C_LIGHT] * g.nodes[i].cpu_us };
			for (int s2 = 0; s2 < 2; s2++) {
				if (!i) { cost[i][s2] = t[s2]; from[i][s2] = (char)s2; continue; }
				double cut = th[CUT_US] + cut_extra + th[BYTE_US] * live[i];
				double stay = cost[i - 1][s2], sw = cost[i - 1][1 - s2] + cut;
				cost[i][s2] = std::min(stay, sw) + t[s2]; from[i][s2] = (char)(stay <= sw ? s2 : 1 - s2);
			}
		}
		std::vector<char> on_cpu(n, 0);
		if (!n) return on_cpu;
		int s2 = cost[n - 1][0] <= cost[n - 1][1] ? 0 : 1;
		for (size_t i = n; i-- > 0;) { on_cpu[i] = (char)s2; s2 = from[i][s2]; }
		return on_cpu;
	}
};
// the ranges of nodes a placement puts on the CPU (nodes the NPU does not take anyway are left out where that saves a range)
std::vector<std::pair<int, int>> cpu_ranges(const std::vector<char> &on_cpu, const std::vector<char> &npu_ok)
{
	std::vector<std::pair<int, int>> r;
	int n = (int)on_cpu.size();
	for (int i = 0; i < n;) {
		if (!on_cpu[i] || !npu_ok[i]) { i++; continue; }
		int j = i;
		while (j + 1 < n && (on_cpu[j + 1] || !npu_ok[j + 1])) j++;
		while (j > i && !npu_ok[j]) j--;
		r.push_back({ i, j + 1 == n ? -1 : j });
		i = j + 1;
	}
	return r;
}
std::vector<char> placement_of(const std::vector<std::pair<int, int>> &ranges, size_t n)
{
	std::vector<char> c(n, 0);
	for (auto &x : ranges) for (int i = std::max(x.first, 0); i < (int)n && (x.second < 0 || i <= x.second); i++) c[i] = 1;
	return c;
}
}  // namespace

// ---- autotune
static double half_to_float(uint16_t h)
{
	int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
	double v = e == 0 ? std::ldexp((double)m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp((double)(m | 1024), e - 25);
	return s ? -v : v;
}
static std::vector<double> as_real(const TensorInfo &t, const uint8_t *p)
{
	size_t n; std::vector<double> v;
	auto deq = [&](double x) { return t.scale ? (x - t.zero_point) * t.scale : x; };
	switch (t.type) {
	case 1: n = t.bytes / 4; for (size_t i = 0; i < n; i++) { float f; memcpy(&f, p + 4 * i, 4); v.push_back(f); } break;
	case 10: n = t.bytes / 2; for (size_t i = 0; i < n; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); v.push_back(half_to_float(h)); } break;
	case 3: for (size_t i = 0; i < t.bytes; i++) v.push_back(deq(p[i])); break;
	case 9: for (size_t i = 0; i < t.bytes; i++) v.push_back(deq((int8_t)p[i])); break;
	case 7: n = t.bytes / 2; for (size_t i = 0; i < n; i++) { int16_t x; memcpy(&x, p + 2 * i, 2); v.push_back(deq(x)); } break;
	case 2: n = t.bytes / 4; for (size_t i = 0; i < n; i++) { int32_t x; memcpy(&x, p + 4 * i, 4); v.push_back(deq(x)); } break;
	default: for (size_t i = 0; i < t.bytes; i++) v.push_back(p[i]);
	}
	return v;
}
static double cosine(const std::vector<double> &a, const std::vector<double> &b)
{
	double ab = 0, aa = 0, bb = 0;
	for (size_t i = 0; i < a.size() && i < b.size(); i++) {
		if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return 0;
		ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i];
	}
	if (aa == 0 && bb == 0) return 1;
	if (aa == 0 || bb == 0) return 0;
	return ab / std::sqrt(aa * bb);
}

std::vector<TuneResult> Model::autotune(const std::vector<std::vector<uint8_t>> &sample_in, double tolerance, int runs, int max_trials)
{
	if (!link_) throw Error("this Model is closed");
	if (runs < 1) runs = 1;
	// the input: the sample, or random data in the range of each input
	std::vector<std::vector<uint8_t>> x = sample_in;
	if (x.empty()) {
		uint64_t seed = 0x9e3779b97f4a7c15ull;
		auto rnd = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
		for (auto &t : inputs_) {
			std::vector<uint8_t> b(t.bytes);
			if (t.type == 1) for (size_t i = 0; i + 4 <= b.size(); i += 4) { float f = (rnd() >> 40) / 16777216.0f; memcpy(&b[i], &f, 4); }
			else if (t.type == 10) for (size_t i = 0; i + 2 <= b.size(); i += 2) { uint16_t h = 0x3800 | (rnd() & 0x3ff); memcpy(&b[i], &h, 2); }   // 0.5 .. 1
			else if (t.type == 3 || t.type == 9) for (auto &c : b) c = (uint8_t)rnd();
			x.push_back(b);
		}
	}
	if (x.size() != inputs_.size()) throw Error("autotune: the model has " + std::to_string(inputs_.size()) + " input(s), " + std::to_string(x.size()) + " given");
	LoadOptions base = opt_;
	// the loaded copy of this model is unloaded while the candidates are compiled: a big model and a candidate together may not fit into the 2 GB of the
	// NPU die (seen with EfficientNet: about 550 MB each plus the compiler). This object is loaded again with the chosen plan at the end.
	try { link_->call(NPU_UNLOAD, nullptr, 0, id_, 30); } catch (Error &) {}
	// one candidate: loaded, run once (the result), then `runs` times (the time); g: the graph of this load (with the CPU profile when `profile`)
	auto trial = [&](const Plan &p, const char *suffix, std::vector<std::vector<uint8_t>> *outs, GraphInfo *g, bool profile) {
		TuneResult r; r.plan = p.tag();
		try {
			LoadOptions o = base; o.key_suffix = suffix; o.name = "autotune"; o.strict = false; o.profile = profile;
			if (g) o.use_cache = false;   // the graph needs a load through TensorFlow Lite, not the compiled graph
			if (p.cpu) o.device = "cpu"; else { o.device = "npu"; o.split = p.split; o.split_ratio = p.ratio; o.split_max_error = p.err; o.cpu_ops = p.deny; o.cpu_nodes = p.nodes; }
			Model m(data_, o);
			std::vector<std::vector<uint8_t>> y = m.exec(x, 600);
			auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < runs; i++) m.exec(x, 600);
			r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / runs;
			if (g) *g = m.graph();
			if (outs) *outs = y;
			m.stop();
		} catch (Error &e) { r.error = e.what(); }
		return r;
	};
	std::vector<TuneResult> res;
	std::vector<std::vector<uint8_t>> ref;
	Plan cpu; cpu.cpu = true;
	TuneResult rc = trial(cpu, "#tune-ref", &ref, nullptr, false);
	if (!rc.error.empty()) throw Error("autotune: the model does not run on the CPU of the NPU die: " + rc.error);
	rc.cosine = 1; rc.ok = true;
	res.push_back(rc);
	auto score = [&](TuneResult &r, const std::vector<std::vector<uint8_t>> &y) {
		if (!r.error.empty() || y.size() != ref.size()) return;
		double c = 1;
		for (size_t i = 0; i < y.size(); i++) c = std::min(c, cosine(as_real(outputs_[i], y[i].data()), as_real(outputs_[i], ref[i].data())));
		r.cosine = c; r.ok = c >= tolerance;
	};
	// 1. the whole model on the NPU. Per-channel int8 layers are split into groups whose scales differ at most `ratio` times (the fast path of the NPU);
	// a group requantises its channels to its largest scale, which costs accuracy (MobileNet V3: ratio 4 68 ms / cosine 0.966, 2 90 ms / 0.982, no split
	// 173 ms / 0.984). The server can choose the ratio per layer from a bound on the weight error (split_per_channel.h): only the layers that need it
	// get a finer split or none. So: ratio 4; if that is not right, the bound by bisection on a grid (a smaller bound: more exact, slower); if even the
	// smallest bound is not right, no split. Every one of these loads also times every node of its graph on the CPU (the cost model below).
	struct Whole { Plan p; TuneResult r; GraphInfo g; };
	std::vector<Whole> measured;
	auto whole_trial = [&](const Plan &p) {
		std::vector<std::vector<uint8_t>> y; GraphInfo gi;
		TuneResult r = trial(p, "#tune", &y, &gi, true);
		score(r, y);
		res.push_back(r);
		if (r.error.empty()) measured.push_back({ p, r, gi });
		return r;
	};
	{
		TuneResult r4 = whole_trial(Plan());
		if (has_per_channel(data_) && !(r4.error.empty() && r4.ok)) {
			// the bound between the last wrong and the first right one, geometric bisection: the time can jump between two bounds (ResNet18: 0.02
			// 23 ms, 0.01 243 ms, when some layers keep their per-channel weights), so the search narrows to a factor of 1.25, at most 5 loads
			double wrong = 0, right = 0;   // 0: none yet (wrong 0: only ratio 4 is known to be wrong)
			bool slow = false;
			for (int k = 0; k < 5; k++) {
				double e = !wrong ? 0.02 : !right ? wrong / 2 : std::sqrt(wrong * right);
				if (e < 0.0005 || (wrong && right && wrong / right < 1.25)) break;
				Plan p; p.err = e;
				TuneResult r = whole_trial(p);
				if (r.error.empty() && r.ok) right = e;
				else { wrong = e; if (r.error.empty() && r.ms > rc.ms) { slow = true; break; } }   // wrong and slower than the CPU: finer is slower still
			}
			if (!right && !slow) { Plan n; n.split = false; whole_trial(n); }
		}
	}
	// the base of steps 2 and 3: the fastest right whole-NPU plan, else the fastest one
	Plan best_npu; double best_npu_ms = 1e30; bool best_npu_ok = false; GraphInfo g;
	{
		const Whole *fast = nullptr, *right = nullptr;
		for (auto &w : measured) {
			if (!fast || w.r.ms < fast->r.ms) fast = &w;
			if (w.r.ok && (!right || w.r.ms < right->r.ms)) right = &w;
		}
		const Whole *b = right ? right : fast;   // the parts on the CPU are tried for the accuracy only when no whole-NPU plan is right (the bound on the
		                                         // weight error handles a loss spread over the network, measured with SqueezeNet and ResNet18)
		if (b) { best_npu = b->p; best_npu_ms = b->r.ms; best_npu_ok = b->r.ok; g = b->g; }
	}
	const size_t n = g.nodes.size();
	if (best_npu_ms >= 1e30 || n < 2 || g.profile_us <= 0) goto choose;   // no NPU plan, or no profile: nothing to place
	{
		CostModel cm(g);
		// priors: the CPU factors from the measured CPU plan, the NPU factors from the whole-NPU plan with the light nodes 8 times less accelerated
		// than the convolutions (ResNet18: 36 times faster on the NPU than on the CPU, element-wise operators a few times)
		std::vector<char> none(n, 0), all(n, 1);
		auto fc = cm.features(all), fn = cm.features(none);
		double c = rc.ms * 1000 / std::max(fc[C_HEAVY] + fc[C_LIGHT], 1.0);
		cm.prior[C_HEAVY] = cm.prior[C_LIGHT] = c;
		cm.prior[CUT_US] = 300; cm.prior[BYTE_US] = 0.002;   // a switch between NPU and CPU parts; about 500 MB/s for the copy and the conversion
		double rest = best_npu_ms * 1000 - c * (fn[C_HEAVY] + fn[C_LIGHT]) - cm.prior[CUT_US] * fn[CUT_US] - cm.prior[BYTE_US] * fn[BYTE_US];
		double a = std::max(rest, 0.05 * best_npu_ms * 1000) / std::max(fn[A_HEAVY] + 8 * fn[A_LIGHT], 1.0);
		cm.prior[A_HEAVY] = a; cm.prior[A_LIGHT] = 8 * a;
		for (int k = 0; k < NPARAM; k++) cm.th[k] = cm.prior[k];
		cm.add(none, best_npu_ms);   // the CPU plan runs the original graph, not this one (no split, no rewrites): a prior only, not a measurement of it
		int tries = 0;
		std::vector<char> forced(n, 0);
		std::map<std::string, bool> tried;
		tried[best_npu.tag()] = true;
		auto measure = [&](const Plan &p, std::vector<char> on_cpu) {
			std::vector<std::vector<uint8_t>> y;
			double pred = cm.predict(cm.features(on_cpu)) / 1000;
			TuneResult r = trial(p, "#tune", &y, nullptr, false);
			r.predicted_ms = pred;
			score(r, y);
			res.push_back(r); tried[p.tag()] = true; tries++;
			if (r.error.empty()) { cm.add(on_cpu, r.ms); cm.fit(); }
			return r;
		};
		// 2. accuracy: no whole-NPU plan is right. Find where the result goes wrong: the latest node from which on the CPU the result is right, by
		// bisection (about log2(nodes) compilations); then whether the damage is local: only a window of nodes there on the CPU, 1, 2, 4, ... wide.
		// Those nodes must stay on the CPU in every later plan.
		if (!best_npu_ok && best_npu_ms < rc.ms) {
			int lo = 0, hi = (int)n;   // nodes from lo on, on the CPU: right; from hi on: wrong
			while (hi - lo > 1 && tries < max_trials) {
				int mid = (lo + hi) / 2;
				Plan p = best_npu; p.nodes = { { mid, -1 } };
				TuneResult r = measure(p, placement_of(p.nodes, n));
				if (r.error.empty() && r.ok) lo = mid; else hi = mid;
			}
			for (int i = lo; i < (int)n; i++) forced[i] = 1;
			for (int w = 1; lo + w < (int)n && tries < max_trials; w *= 2) {   // lo 0: the first node (SqueezeNet: all the loss is there)
				Plan p = best_npu; p.nodes = { { lo, lo + w - 1 } };
				TuneResult r = measure(p, placement_of(p.nodes, n));
				if (r.error.empty() && r.ok) { forced.assign(n, 0); for (int i = lo; i < lo + w; i++) forced[i] = 1; break; }
			}
		}
		// 3. speed: the cheapest placement under the model (with the forced nodes on the CPU), measured; the model is fitted again after every
		// measurement, until it proposes nothing new that it expects to be 3 % faster than the best right plan so far
		while (tries < max_trials) {
			double best_ok = 1e30;
			for (auto &r : res) if (r.ok && r.error.empty()) best_ok = std::min(best_ok, r.ms);
			std::vector<char> on_cpu;
			std::vector<std::pair<int, int>> rg;
			for (double extra = 0;; extra = extra ? extra * 2 : cm.th[CUT_US]) {   // at most NPU_MAX_RANGES parts on the CPU
				on_cpu = cm.best(forced, extra);
				rg = cpu_ranges(on_cpu, cm.npu_ok);
				if (rg.size() <= NPU_MAX_RANGES || extra > 1e9) break;
			}
			if (rg.size() > NPU_MAX_RANGES) break;
			Plan p = best_npu; p.nodes = rg;
			bool all_cpu = true; for (size_t i = 0; i < n; i++) all_cpu = all_cpu && (on_cpu[i] || !cm.npu_ok[i]);
			if (all_cpu) break;   // the CPU plan is measured already
			double pred = cm.predict(cm.features(on_cpu)) / 1000;
			if (tried.count(p.tag()) || pred > best_ok * 0.97) break;
			measure(p, on_cpu);
		}
	}
choose:
	// the choice: the fastest candidate with a right result (the CPU is always right). A plan without a compiled graph to cache (mixed, cpu) is loaded
	// through TensorFlow Lite every time: it must beat the whole-NPU plan by 5 % (and 0.3 ms) to be taken, a difference in the noise is not worth it
	size_t bi = 0;
	for (size_t i = 0; i < res.size(); i++) if (res[i].ok && res[i].error.empty() && res[i].ms < res[bi].ms) bi = i;
	for (size_t i = 0; i < res.size(); i++) {
		if (i == bi || !res[i].ok || !res[i].error.empty() || !Plan::parse(res[i].plan).whole_npu() || Plan::parse(res[bi].plan).whole_npu()) continue;
		bool clearly_faster = res[bi].ms < res[i].ms * 0.95 && res[i].ms - res[bi].ms > 0.3;
		if (!clearly_faster) bi = i;
	}
	std::swap(res[0], res[bi]);
	// keep the plan, switch this object to it
	std::string cache = cache_dir_of(opt_);
	if (!cache.empty()) {
		mkdirs(cache);
		std::string mp = cache + "/" + cache_key() + ".meta";
		Meta meta = read_meta(mp);
		meta["plan"] = res[0].plan;
		char b[32]; snprintf(b, sizeof b, "%.3f", res[0].ms); meta["plan.ms"] = b;
		snprintf(b, sizeof b, "%.5f", res[0].cosine); meta["plan.cosine"] = b;
		write_meta(mp, meta);
	}
	{
		LoadOptions o = opt_;
		Plan p = Plan::parse(res[0].plan);
		if (p.cpu) o.device = "cpu"; else { o.device = "npu"; o.split = p.split; o.split_ratio = p.ratio; o.split_max_error = p.err; o.cpu_ops = p.deny; o.cpu_nodes = p.nodes; o.strict = false; }
		std::vector<uint8_t> d = data_;
		link_.reset();
		attached_ = from_cache_ = false;
		init(d, o, name_);
		opt_.device = "auto";
	}
	return res;
}

std::vector<uint8_t> Model::pack(const std::vector<const void *> &in) const
{
	if (in.size() != inputs_.size()) throw Error("the model has " + std::to_string(inputs_.size()) + " input(s), " + std::to_string(in.size()) + " given");
	size_t total = 0; for (auto &t : inputs_) total += t.bytes;
	std::vector<uint8_t> p(total); size_t off = 0;
	for (size_t i = 0; i < in.size(); i++) { memcpy(p.data() + off, in[i], inputs_[i].bytes); off += inputs_[i].bytes; }
	return p;
}

void Model::exec(const std::vector<const void *> &in, const std::vector<void *> &out, int timeout_s)
{
	if (!link_) throw Error("this Model is closed");
	if (out.size() != outputs_.size()) throw Error("the model has " + std::to_string(outputs_.size()) + " output(s), " + std::to_string(out.size()) + " buffers given");
	std::vector<uint8_t> p = pack(in);
	auto r = link_->call(NPU_RUN, p.data(), p.size(), id_, timeout_s);
	size_t need = 4; for (auto &t : outputs_) need += t.bytes;
	if (r.data.size() < need) throw Error("short reply of the NPU");
	uint32_t us; memcpy(&us, r.data.data(), 4); slots_->last_us = us;
	size_t off = 4;
	for (size_t i = 0; i < out.size(); i++) { memcpy(out[i], r.data.data() + off, outputs_[i].bytes); off += outputs_[i].bytes; }
}

std::vector<std::vector<uint8_t>> Model::exec(const std::vector<std::vector<uint8_t>> &in, int timeout_s)
{
	std::vector<const void *> ip;
	for (size_t i = 0; i < in.size(); i++) {
		if (i < inputs_.size() && in[i].size() != inputs_[i].bytes)
			throw Error("input " + inputs_[i].name + ": " + std::to_string(in[i].size()) + " bytes, the model wants " + std::to_string(inputs_[i].bytes));
		ip.push_back(in[i].data());
	}
	std::vector<std::vector<uint8_t>> o(outputs_.size());
	std::vector<void *> op;
	for (size_t i = 0; i < o.size(); i++) { o[i].resize(outputs_[i].bytes); op.push_back(o[i].data()); }
	exec(ip, op, timeout_s);
	return o;
}

void Model::async_exec(const std::vector<const void *> &in, Done done)
{
	if (!link_) throw Error("this Model is closed");
	std::vector<uint8_t> p = pack(in);
	auto slots = slots_;
	slots->acquire();
	if (gap_us_ > 0) usleep(gap_us_);
	std::vector<size_t> sizes; for (auto &t : outputs_) sizes.push_back(t.bytes);
	try {
		link_->submit(NPU_RUN, p.data(), p.size(), id_, [slots, sizes, done](std::vector<uint8_t> &&d, uint32_t, const std::string &err) {
			slots->release();
			std::vector<std::vector<uint8_t>> o;
			std::string e = err;
			if (e.empty()) {
				size_t need = 4; for (size_t s : sizes) need += s;
				if (d.size() < need) e = "short reply of the NPU";
				else { uint32_t us; memcpy(&us, d.data(), 4); slots->last_us = us; size_t off = 4; for (size_t s : sizes) { o.emplace_back(d.begin() + off, d.begin() + off + s); off += s; } }
			}
			done(std::move(o), e);
		});
	} catch (...) { slots->release(); throw; }
}

void Model::close() { link_.reset(); }

uint32_t Model::last_invoke_us() const { return slots_ ? slots_->last_us.load() : 0; }

void Model::stop()
{
	if (!link_) throw Error("this Model is closed");
	auto l = link_;
	link_.reset();
	try { l->call(NPU_UNLOAD, nullptr, 0, id_, 30); }
	catch (Error &e) { if (std::string(e.what()).find("no such model") == std::string::npos) throw; }   // gone already (stopped by another program, or its engine died)
}

}  // namespace rk3399pro_npu
