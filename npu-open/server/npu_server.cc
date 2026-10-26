/* Thin server on the NPU die (research, rnd/verisilicon): takes over the FunctionFS function "ntb" (see transport/npu_echo.c),
 * loads one TFLite model, runs it on the NPU through the TFLite VX delegate (TIM-VX), frees it on request or when the host goes away.
 * Protocol: protocol.h. One model at a time. A reader thread takes requests from the USB endpoint while the worker runs the previous one. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <linux/aio_abi.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "tensorflow/lite/delegates/external/external_delegate.h"
#include "tim/vx/context.h"
#include "tim/vx/graph.h"
#include "tim/vx/ops/nbg.h"
#include "tim/vx/tensor.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"

#define FFS "/dev/usb-ffs/ntb"
#define NPU_SYSFS "/sys/devices/platform/ffbc0000.npu"
#define DELEGATE_SO "/usr/lib/npu/libvx_delegate.so"
#define RDCHUNK (64 * 1024)
#define WRCHUNK (256 * 1024)
#define MAX_MSG (96u << 20)
#ifndef REPLY_GAP_US
#define REPLY_GAP_US 0   /* a pause before the reply did not help (see README of rnd/verisilicon) */
#endif

static volatile const char *g_rstate = "start", *g_wstate = "start";
static volatile unsigned long g_rx, g_tx;
static FILE *g_log;   /* our own log: stderr belongs to the TFLite delegate, which prints on every run */
static void logf_(const char *fmt, ...)
{
	if (!g_log) return;
	va_list a; va_start(a, fmt); fprintf(g_log, "npu_server: "); vfprintf(g_log, fmt, a); fprintf(g_log, "\n"); fflush(g_log); va_end(a);
}
static uint64_t now_us() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

/* ---------------- USB FunctionFS (descriptors: see transport/npu_echo.c) ---------------- */
struct ep_pair { struct usb_endpoint_descriptor_no_audio bulk_out, bulk_in; } __attribute__((packed));
struct ss_ep { struct usb_endpoint_descriptor_no_audio d; struct usb_ss_ep_comp_descriptor c; } __attribute__((packed));
static const struct {
	struct usb_functionfs_descs_head_v2 header;
	__le32 fs_count, hs_count, ss_count;
	struct { struct usb_interface_descriptor intf; struct ep_pair ep; } __attribute__((packed)) fs, hs;
	struct { struct usb_interface_descriptor intf; struct ss_ep out, in; } __attribute__((packed)) ss;
} __attribute__((packed)) descs = {
	.header = { .magic = __constant_cpu_to_le32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2), .length = __constant_cpu_to_le32(sizeof(descs)),
		    .flags = __constant_cpu_to_le32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC) },
	.fs_count = 3, .hs_count = 3, .ss_count = 5,
#define INTF { .bLength = sizeof(struct usb_interface_descriptor), .bDescriptorType = USB_DT_INTERFACE, .bNumEndpoints = 2, \
	       .bInterfaceClass = 0xff, .bInterfaceSubClass = 0x4e, .bInterfaceProtocol = 0x50, .iInterface = 1 }
#define EP(addr, mps) { .bLength = sizeof(struct usb_endpoint_descriptor_no_audio), .bDescriptorType = USB_DT_ENDPOINT, \
			.bEndpointAddress = addr, .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = __constant_cpu_to_le16(mps) }
#define COMP { .bLength = sizeof(struct usb_ss_ep_comp_descriptor), .bDescriptorType = USB_DT_SS_ENDPOINT_COMP, .bMaxBurst = 0 }
	.fs = { .intf = INTF, .ep = { .bulk_out = EP(1 | USB_DIR_OUT, 64), .bulk_in = EP(2 | USB_DIR_IN, 64) } },
	.hs = { .intf = INTF, .ep = { .bulk_out = EP(1 | USB_DIR_OUT, 512), .bulk_in = EP(2 | USB_DIR_IN, 512) } },
	.ss = { .intf = INTF, .out = { .d = EP(1 | USB_DIR_OUT, 1024), .c = COMP }, .in = { .d = EP(2 | USB_DIR_IN, 1024), .c = COMP } },
};
static const struct {
	struct usb_functionfs_strings_head header;
	struct { __le16 code; const char str1[8]; } __attribute__((packed)) lang0;
} __attribute__((packed)) strs = {
	.header = { .magic = __constant_cpu_to_le32(FUNCTIONFS_STRINGS_MAGIC), .length = __constant_cpu_to_le32(sizeof(strs)),
		    .str_count = __constant_cpu_to_le32(1), .lang_count = __constant_cpu_to_le32(1) },
	.lang0 = { __constant_cpu_to_le16(0x0409), "npu-ctl" },
};

static int open_retry(const char *path, int tries) { for (int i = 0; i < tries; i++) { int fd = open(path, O_RDWR); if (fd >= 0) return fd; usleep(100000); } return -1; }
static bool write_all(int fd, const void *b, size_t n)
{
	const uint8_t *p = (const uint8_t *)b;
	while (n) { size_t c = n < WRCHUNK ? n : WRCHUNK; ssize_t w = write(fd, p, c); if (w < 0) { if (errno == EINTR) continue; return false; } p += w; n -= w; }
	return true;
}

/* ---------------- the model ---------------- */
struct Model {
	std::vector<uint8_t> bytes;
	std::unique_ptr<tflite::FlatBufferModel> fb;
	std::unique_ptr<tflite::Interpreter> interp;
	TfLiteDelegate *delegate = nullptr;
	std::vector<npu_tensor> info;   /* inputs, then outputs */
	/* NBG mode: the compiled graph is loaded with TIM-VX directly, TFLite is not used */
	bool nbg_mode = false;
	size_t n_in = 0, n_out = 0;
	std::vector<char> nbg;
	std::shared_ptr<tim::vx::Context> ctx;
	std::shared_ptr<tim::vx::Graph> graph;
	std::vector<std::shared_ptr<tim::vx::Tensor>> tin, tout;
};
static std::unique_ptr<Model> g_model;
static std::mutex g_model_mu;

static std::string read_file(const char *path) { std::string s; FILE *f = fopen(path, "r"); if (!f) return s; char b[512]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }
static uint32_t meminfo_kb(const char *key) { std::string m = read_file("/proc/meminfo"); size_t p = m.find(key); return p == std::string::npos ? 0 : (uint32_t)strtoul(m.c_str() + p + strlen(key) + 1, 0, 10); }

static void chip_identity(uint32_t *model, uint32_t *rev, uint32_t *prod, char *driver)
{
	*model = *rev = *prod = 0; driver[0] = 0;
	std::string s = read_file(NPU_SYSFS "/info");
	size_t p;
	if ((p = s.find("model")) != std::string::npos) *model = strtoul(s.c_str() + s.find(':', p) + 1, 0, 16);
	if ((p = s.find("revision")) != std::string::npos) *rev = strtoul(s.c_str() + s.find(':', p) + 1, 0, 16);
	if ((p = s.find("product")) != std::string::npos) *prod = strtoul(s.c_str() + s.find(':', p) + 1, 0, 16);
	std::string v = read_file(NPU_SYSFS "/version");
	snprintf(driver, 32, "%.*s", (int)(v.size() && v.back() == '\n' ? v.size() - 1 : v.size()), v.c_str());
}

static void describe(Model &m)
{
	auto add = [&](int idx) {
		const TfLiteTensor *t = m.interp->tensor(idx);
		npu_tensor d; memset(&d, 0, sizeof d);
		d.type = t->type; d.ndims = t->dims ? t->dims->size : 0;
		for (int i = 0; i < d.ndims && i < 6; i++) d.dims[i] = t->dims->data[i];
		d.scale = t->params.scale; d.zero_point = t->params.zero_point; d.bytes = t->bytes;
		snprintf(d.name, sizeof d.name, "%s", t->name ? t->name : "");
		m.info.push_back(d);
	};
	for (int i : m.interp->inputs()) add(i);
	for (int i : m.interp->outputs()) add(i);
}

/* returns an error text, or "" and fills `reply` */
static std::string do_load(std::vector<uint8_t> &&bytes, std::vector<uint8_t> &reply)
{
	std::lock_guard<std::mutex> lk(g_model_mu);
	if (g_model) return "a model is already loaded (one model at a time): unload it first";
	uint32_t mo, re, pr; char drv[32]; chip_identity(&mo, &re, &pr, drv);
	if (mo != 0x8000 || re != 0x7100 || pr != 0x45080001) {
		char b[160]; snprintf(b, sizeof b, "unknown NPU chip %x/%x/%x: this build is made for 8000/7100/45080001 only", mo, re, pr); return b;
	}
	uint32_t avail = meminfo_kb("MemAvailable:");
	/* the model is kept in memory, TFLite makes a copy of the weights for the NPU and the graph takes more: 4x is a first guess */
	if ((uint64_t)bytes.size() / 1024 * 4 + 64 * 1024 > avail) {
		char b[160]; snprintf(b, sizeof b, "not enough memory: the model needs about %zu MB, %u MB are free", bytes.size() / 1024 / 1024 * 4 + 64, avail / 1024); return b;
	}
	auto m = std::make_unique<Model>();
	m->bytes = std::move(bytes);
	uint64_t t0 = now_us();
	m->fb = tflite::FlatBufferModel::BuildFromBuffer((const char *)m->bytes.data(), m->bytes.size());
	if (!m->fb) return "not a TFLite model (or a version TFLite 2.6 cannot read)";
	tflite::ops::builtin::BuiltinOpResolver resolver;
	tflite::InterpreterBuilder(*m->fb, resolver)(&m->interp);
	if (!m->interp) return "cannot build the TFLite interpreter";
	setenv("VX_NBG_EXPORT", "/tmp/npu_model.nbg", 1);   /* the delegate writes the compiled graph here */
	unlink("/tmp/npu_model.nbg");
	auto opt = TfLiteExternalDelegateOptionsDefault(DELEGATE_SO);
	m->delegate = TfLiteExternalDelegateCreate(&opt);
	if (!m->delegate) return "cannot load the VX delegate " DELEGATE_SO;
	if (m->interp->ModifyGraphWithDelegate(m->delegate) != kTfLiteOk) return "the VX delegate refused the model";
	if (m->interp->AllocateTensors() != kTfLiteOk) return "cannot allocate the tensors";
	if (m->interp->Invoke() != kTfLiteOk) return "the first run failed";   /* the Vivante compiler builds the graph here (seconds) */
	describe(*m);
	npu_load_reply lr = { (uint32_t)m->interp->inputs().size(), (uint32_t)m->interp->outputs().size(), (uint32_t)((now_us() - t0) / 1000),
			      avail > meminfo_kb("MemAvailable:") ? avail - meminfo_kb("MemAvailable:") : 0 };
	unsetenv("VX_NBG_EXPORT");
	std::string nbg = read_file("/tmp/npu_model.nbg");
	unlink("/tmp/npu_model.nbg");
	uint32_t nbg_len = nbg.size();
	size_t tl = m->info.size() * sizeof(npu_tensor);
	reply.resize(sizeof lr + tl + 4 + nbg_len);
	memcpy(reply.data(), &lr, sizeof lr);
	memcpy(reply.data() + sizeof lr, m->info.data(), tl);
	memcpy(reply.data() + sizeof lr + tl, &nbg_len, 4);
	if (nbg_len) memcpy(reply.data() + sizeof lr + tl + 4, nbg.data(), nbg_len);
	logf_("compiled graph: %u bytes", nbg_len);
	g_model = std::move(m);
	logf_("model loaded: %u inputs, %u outputs, %u ms, %u MB", lr.n_inputs, lr.n_outputs, lr.load_ms, lr.mem_used_kb / 1024);
	return "";
}

static tim::vx::DataType vx_type(int t)
{
	switch (t) { case 1: return tim::vx::DataType::FLOAT32; case 2: return tim::vx::DataType::INT32; case 3: return tim::vx::DataType::UINT8;
		     case 7: return tim::vx::DataType::INT16; case 9: return tim::vx::DataType::INT8; case 10: return tim::vx::DataType::FLOAT16;
		     default: return tim::vx::DataType::UNKNOWN; }
}

/* a model from its compiled graph: tensors as in the LOAD reply, then the NBG */
static std::string do_load_nbg(std::vector<uint8_t> &&p, std::vector<uint8_t> &reply)
{
	std::lock_guard<std::mutex> lk(g_model_mu);
	if (g_model) return "a model is already loaded (one model at a time): unload it first";
	uint32_t mo, re, pr; char drv[32]; chip_identity(&mo, &re, &pr, drv);
	if (mo != 0x8000 || re != 0x7100 || pr != 0x45080001) { char b[160]; snprintf(b, sizeof b, "unknown NPU chip %x/%x/%x", mo, re, pr); return b; }
	if (p.size() < 8) return "short request";
	uint32_t n_in, n_out; memcpy(&n_in, p.data(), 4); memcpy(&n_out, p.data() + 4, 4);
	size_t tl = (size_t)(n_in + n_out) * sizeof(npu_tensor);
	if (n_in == 0 || n_out == 0 || n_in > 16 || n_out > 16 || p.size() <= 8 + tl) return "bad tensor description";
	uint32_t avail = meminfo_kb("MemAvailable:");
	size_t nbg_len = p.size() - 8 - tl;
	if (nbg_len / 1024 * 2 + 32 * 1024 > avail) return "not enough memory for the compiled graph";
	auto m = std::make_unique<Model>();
	m->nbg_mode = true; m->n_in = n_in; m->n_out = n_out;
	m->info.resize(n_in + n_out);
	memcpy(m->info.data(), p.data() + 8, tl);
	m->nbg.assign((const char *)p.data() + 8 + tl, (const char *)p.data() + p.size());
	uint64_t t0 = now_us();
	m->ctx = tim::vx::Context::Create();
	m->graph = m->ctx->CreateGraph();
	auto make = [&](const npu_tensor &t, tim::vx::TensorAttribute attr) {
		tim::vx::ShapeType shape;
		for (int i = t.ndims - 1; i >= 0; i--) shape.push_back(t.dims[i]);   /* TFLite NHWC dims -> TIM-VX WHCN */
		tim::vx::TensorSpec spec(vx_type(t.type), shape, attr);
		if (t.scale != 0 && (t.type == 3 || t.type == 9 || t.type == 7)) spec.SetQuantization(*new tim::vx::Quantization(tim::vx::QuantType::ASYMMETRIC, t.scale, t.zero_point));
		return m->graph->CreateTensor(spec);
	};
	for (uint32_t i = 0; i < n_in; i++) m->tin.push_back(make(m->info[i], tim::vx::TensorAttribute::INPUT));
	for (uint32_t i = 0; i < n_out; i++) m->tout.push_back(make(m->info[n_in + i], tim::vx::TensorAttribute::OUTPUT));
	auto op = m->graph->CreateOperation<tim::vx::ops::NBG>(m->nbg.data(), n_in, n_out);
	op->BindInputs(m->tin).BindOutputs(m->tout);
	if (!m->graph->Compile()) return "the NPU did not accept the compiled graph (made by another driver version or chip?)";
	npu_load_reply lr = { n_in, n_out, (uint32_t)((now_us() - t0) / 1000), avail > meminfo_kb("MemAvailable:") ? avail - meminfo_kb("MemAvailable:") : 0 };
	uint32_t zero = 0;
	reply.resize(sizeof lr + tl + 4);
	memcpy(reply.data(), &lr, sizeof lr);
	memcpy(reply.data() + sizeof lr, m->info.data(), tl);
	memcpy(reply.data() + sizeof lr + tl, &zero, 4);
	g_model = std::move(m);
	logf_("model loaded from the compiled graph: %u inputs, %u outputs, %u ms", n_in, n_out, lr.load_ms);
	return "";
}
static void do_unload()
{
	std::lock_guard<std::mutex> lk(g_model_mu);
	if (!g_model) return;
	if (g_model->interp) g_model->interp.reset();
	g_model->graph.reset(); g_model->tin.clear(); g_model->tout.clear(); g_model->ctx.reset();
	if (g_model->delegate) TfLiteExternalDelegateDelete(g_model->delegate);
	g_model.reset();
	logf_("model unloaded");
}
static std::string do_run(const std::vector<uint8_t> &in, std::vector<uint8_t> &out)
{
	std::lock_guard<std::mutex> lk(g_model_mu);
	if (!g_model) return "no model loaded";
	Model &m = *g_model;
	if (m.nbg_mode) {
		size_t need = 0, off = 0;
		for (size_t i = 0; i < m.n_in; i++) need += m.info[i].bytes;
		if (in.size() != need) { char b[100]; snprintf(b, sizeof b, "input size %zu, the model wants %zu bytes", in.size(), need); return b; }
		for (size_t i = 0; i < m.n_in; i++) { if (!m.tin[i]->CopyDataToTensor(in.data() + off, m.info[i].bytes)) return "cannot copy the input"; off += m.info[i].bytes; }
		uint64_t t0 = now_us();
		if (!m.graph->Run()) return "Run failed";
		uint32_t us = (uint32_t)(now_us() - t0);
		size_t total = 4;
		for (size_t i = 0; i < m.n_out; i++) total += m.info[m.n_in + i].bytes;
		out.resize(total); memcpy(out.data(), &us, 4); off = 4;
		for (size_t i = 0; i < m.n_out; i++) { if (!m.tout[i]->CopyDataFromTensor(out.data() + off)) return "cannot copy the output"; off += m.info[m.n_in + i].bytes; }
		return "";
	}
	size_t ni = m.interp->inputs().size(), need = 0;
	for (size_t i = 0; i < ni; i++) need += m.info[i].bytes;
	if (in.size() != need) { char b[100]; snprintf(b, sizeof b, "input size %zu, the model wants %zu bytes", in.size(), need); return b; }
	size_t off = 0;
	for (size_t i = 0; i < ni; i++) { memcpy(m.interp->tensor(m.interp->inputs()[i])->data.raw, in.data() + off, m.info[i].bytes); off += m.info[i].bytes; }
	uint64_t t0 = now_us();
	if (m.interp->Invoke() != kTfLiteOk) return "Invoke failed";
	uint32_t us = (uint32_t)(now_us() - t0);
	size_t no = m.interp->outputs().size(), total = 4;
	for (size_t i = 0; i < no; i++) total += m.info[ni + i].bytes;
	out.resize(total);
	memcpy(out.data(), &us, 4);
	off = 4;
	for (size_t i = 0; i < no; i++) { memcpy(out.data() + off, m.interp->tensor(m.interp->outputs()[i])->data.raw, m.info[ni + i].bytes); off += m.info[ni + i].bytes; }
	return "";
}

/* ---------------- messages ---------------- */
struct Msg { npu_hdr h; std::vector<uint8_t> payload; };
static std::mutex q_mu; static std::condition_variable q_cv; static std::deque<Msg> q; static bool q_closed = false;
static const size_t QMAX = 3;   /* RUN requests waiting while one runs: enough to hide the USB transfer */

static bool reply(int in_fd, const npu_hdr &req, bool err, const void *p, size_t n)
{
	static std::mutex wmu; std::lock_guard<std::mutex> lk(wmu);
	bool pad = ((sizeof(npu_hdr) + n) % 1024) == 0;
	npu_hdr h = { NPU_MAGIC, req.type, (uint16_t)((err ? NPU_FLAG_ERROR : 0) | (pad ? NPU_FLAG_PAD : 0)), req.tag, (uint32_t)n };
	static std::vector<uint8_t> buf; buf.resize(sizeof h + n + (pad ? 1 : 0));
	memcpy(buf.data(), &h, sizeof h);
	if (n) memcpy(buf.data() + sizeof h, p, n);
	if (pad) buf[sizeof h + n] = 0;
	static const int gap = getenv("NPU_REPLY_GAP_US") ? atoi(getenv("NPU_REPLY_GAP_US")) : REPLY_GAP_US;   /* experiment */
	if (gap) usleep(gap);
	g_wstate = "writing a reply to USB";
	return write_all(in_fd, buf.data(), buf.size());
}
static bool reply_err(int in_fd, const npu_hdr &req, const std::string &t) { logf_("error reply: %s", t.c_str()); return reply(in_fd, req, true, t.data(), t.size()); }

static void handle(int in_fd, Msg &m)
{
	std::vector<uint8_t> out;
	switch (m.h.type) {
	case NPU_HELLO: {
		npu_hello hl; memset(&hl, 0, sizeof hl);
		hl.proto = NPU_PROTO_VERSION;
		chip_identity(&hl.chip_model, &hl.chip_revision, &hl.chip_product, hl.driver);
		hl.chip_ok = hl.chip_model == 0x8000 && hl.chip_revision == 0x7100 && hl.chip_product == 0x45080001;
		hl.mem_total_kb = meminfo_kb("MemTotal:"); hl.mem_avail_kb = meminfo_kb("MemAvailable:");
		{ std::lock_guard<std::mutex> lk(g_model_mu); hl.model_loaded = g_model != nullptr; }
		reply(in_fd, m.h, false, &hl, sizeof hl); break; }
	case NPU_LOAD: { std::string e = do_load(std::move(m.payload), out); if (e.empty()) reply(in_fd, m.h, false, out.data(), out.size()); else reply_err(in_fd, m.h, e); break; }
	case NPU_LOAD_NBG: { std::string e = do_load_nbg(std::move(m.payload), out); if (e.empty()) reply(in_fd, m.h, false, out.data(), out.size()); else reply_err(in_fd, m.h, e); break; }
	case NPU_RUN: { std::string e = do_run(m.payload, out); if (e.empty()) reply(in_fd, m.h, false, out.data(), out.size()); else reply_err(in_fd, m.h, e); break; }
	case NPU_UNLOAD: do_unload(); reply(in_fd, m.h, false, nullptr, 0); break;
	case NPU_PING: reply(in_fd, m.h, false, m.payload.data(), m.payload.size()); break;
	default: reply_err(in_fd, m.h, "unknown request type");
	}
}

static void worker(int in_fd)
{
	for (;;) {
		Msg m;
		g_wstate = "waiting for a request";
		{ std::unique_lock<std::mutex> lk(q_mu); q_cv.wait(lk, [] { return !q.empty() || q_closed; }); if (q.empty()) return; m = std::move(q.front()); q.pop_front(); q_cv.notify_all(); }
		g_wstate = "handling a request";
		handle(in_fd, m);
		g_tx++;
	}
}

static volatile bool g_rebind;

/* Reads from the OUT endpoint are done with asynchronous I/O, several at once: with a plain blocking read() there is a moment between the end of one
 * read and the start of the next in which no request is queued on the endpoint, and if the host sends in that moment the (old) dwc3 device controller
 * loses the 'ready' notification and the transfer hangs for ever (seen: the host sends the next request right after it got a reply). */
static int sys_io_setup(unsigned n, aio_context_t *c) { return syscall(SYS_io_setup, n, c); }
static int sys_io_submit(aio_context_t c, long n, struct iocb **p) { return syscall(SYS_io_submit, c, n, p); }
static int sys_io_getevents(aio_context_t c, long mn, long mx, struct io_event *e, struct timespec *t) { return syscall(SYS_io_getevents, c, mn, mx, e, t); }
static int sys_io_destroy(aio_context_t c) { return syscall(SYS_io_destroy, c); }


/* events of the function on ep0: the gadget of this firmware is unbound and bound again at every USB disconnect (bus reset, host controller
 * reset, a host program that goes away...). After UNBIND the function wants new descriptors: close everything and start from the top. */
static volatile bool g_ev_stop;
static void ep0_events(int ep0)
{
	for (;;) {
		struct usb_functionfs_event ev;
		ssize_t r = read(ep0, &ev, sizeof ev);   /* ep0 is non-blocking: closing a descriptor does not wake a blocked read */
		if (r == (ssize_t)sizeof ev) { if (ev.type == FUNCTIONFS_UNBIND) { logf_("event: unbind"); break; } continue; }
		if (r < 0 && errno == EAGAIN) { if (g_ev_stop) return; usleep(50000); continue; }
		break;
	}
	g_rebind = true;
}

int main(void)
{
	g_log = fopen("/tmp/npu_server.log", "a");
	std::thread([] {
		unsigned long lrx = 0, ltx = 0; int quiet = 0;
		for (;;) {
			sleep(1);
			if (g_rx == lrx && g_tx == ltx) { if (++quiet == 3 || (quiet > 3 && quiet % 10 == 0)) logf_("quiet for %d s: rx %lu tx %lu, reader: %s, worker: %s", quiet, g_rx, g_tx, g_rstate, g_wstate); }
			else { quiet = 0; lrx = g_rx; ltx = g_tx; }
		}
	}).detach();
	std::vector<uint8_t> chunk(RDCHUNK);
	bool first = true;
	for (;;) {
		g_rebind = false;
		int ep0 = open_retry(FFS "/ep0", 100);
		if (ep0 < 0) { logf_("open ep0 failed: errno %d", errno); sleep(2); continue; }
		if (write(ep0, &descs, sizeof descs) < 0 || write(ep0, &strs, sizeof strs) < 0) { logf_("writing the descriptors failed: errno %d", errno); close(ep0); sleep(1); continue; }
		logf_("descriptors written");
		fcntl(ep0, F_SETFL, fcntl(ep0, F_GETFL) | O_NONBLOCK);
		g_ev_stop = false;
		if (first) { close(open("/tmp/npu.ready", O_CREAT | O_WRONLY, 0644)); first = false; }   /* the init script binds the USB controller after this */
		std::thread ev(ep0_events, ep0);
		int out = open_retry(FFS "/ep1", 300), in = open_retry(FFS "/ep2", 300);
		if (out < 0 || in < 0 || g_rebind) {
			logf_("endpoints did not appear (or the function was unbound meanwhile)");
			if (out >= 0) close(out);
			if (in >= 0) close(in);
			g_ev_stop = true; ev.join(); close(ep0); sleep(1); continue;
		}
		logf_("endpoints open");
		q_closed = false; q.clear();
		std::thread w(worker, in);
		std::vector<uint8_t> stream; stream.reserve(1 << 20);
		Msg cur; bool have_hdr = false; size_t got = 0; size_t skip_pad = 0;
		const int NB = 4;
		std::vector<std::vector<uint8_t>> bufs(NB, std::vector<uint8_t>(RDCHUNK));
		aio_context_t actx = 0;
		struct iocb cbs[NB]; struct io_event evs[NB];
		uint64_t next_submit = 0, next_consume = 0;
		struct Done { int idx; long res; }; std::vector<Done> done(NB * 4); std::vector<bool> have(NB * 4, false);
		auto submit = [&](int idx) {
			memset(&cbs[idx], 0, sizeof cbs[idx]);
			cbs[idx].aio_data = (uint64_t)idx | (next_submit << 8);
			cbs[idx].aio_lio_opcode = IOCB_CMD_PREAD; cbs[idx].aio_fildes = out;
			cbs[idx].aio_buf = (uint64_t)(uintptr_t)bufs[idx].data(); cbs[idx].aio_nbytes = RDCHUNK;
			struct iocb *pcb = &cbs[idx]; next_submit++;
			return sys_io_submit(actx, 1, &pcb) == 1;
		};
		if (sys_io_setup(NB * 2, &actx) < 0) { logf_("io_setup failed: errno %d", errno); goto lost; }
		for (int i = 0; i < NB; i++) if (!submit(i)) { logf_("io_submit failed: errno %d", errno); goto lost; }
		for (;;) {
			/* take the completions, handle them in the order of their submission */
			g_rstate = "reading USB";
			int ne = sys_io_getevents(actx, 1, NB, evs, NULL);
			if (ne < 0) { if (errno == EINTR) continue; break; }
			for (int i = 0; i < ne; i++) { uint64_t d = evs[i].data; uint64_t seq = d >> 8; done[seq % done.size()] = { (int)(d & 255), (long)evs[i].res }; have[seq % done.size()] = true; }
			bool bad = false;
			while (have[next_consume % done.size()]) {
				Done dn = done[next_consume % done.size()]; have[next_consume % done.size()] = false; next_consume++;
				if (dn.res <= 0) { bad = true; errno = (int)-dn.res; break; }
				const uint8_t *p = bufs[dn.idx].data(); size_t n = dn.res;
				g_rstate = "parsing";
				while (n) {
					if (skip_pad) { size_t c = n < skip_pad ? n : skip_pad; p += c; n -= c; skip_pad -= c; continue; }
					if (!have_hdr) {
						size_t need = sizeof(npu_hdr) - stream.size(), c = n < need ? n : need;
						stream.insert(stream.end(), p, p + c); p += c; n -= c;
						if (stream.size() < sizeof(npu_hdr)) continue;
						memcpy(&cur.h, stream.data(), sizeof(npu_hdr)); stream.clear();
						if (cur.h.magic != NPU_MAGIC || cur.h.len > MAX_MSG) { logf_("bad header (magic %08x, len %u): dropping the connection", cur.h.magic, cur.h.len); bad = true; break; }
						cur.payload.assign(cur.h.len, 0); got = 0; have_hdr = true;
						if (cur.h.len) continue;
					} else {
						size_t c = cur.h.len - got < n ? cur.h.len - got : n;
						memcpy(cur.payload.data() + got, p, c); got += c; p += c; n -= c;
					}
					if (have_hdr && got == cur.h.len) {
						have_hdr = false;
						skip_pad = (cur.h.flags & NPU_FLAG_PAD) ? 1 : 0;
						g_rx++;
						g_rstate = "waiting for room in the queue";
						std::unique_lock<std::mutex> lk(q_mu);
						q_cv.wait(lk, [] { return q.size() < QMAX; });
						q.push_back(std::move(cur)); cur = Msg(); q_cv.notify_all();
					}
				}
				if (bad) break;
				if (!submit(dn.idx)) { bad = true; break; }   /* the buffer is free again: arm it at once */
			}
			if (bad) break;
		}
lost:
		if (actx) sys_io_destroy(actx);
		logf_("host disconnected (errno %d): the model is freed", errno);
		{ std::lock_guard<std::mutex> lk(q_mu); q_closed = true; q_cv.notify_all(); }
		close(out); close(in);
		w.join();
		do_unload();
		/* ep0: wait for the UNBIND event (the function is rebuilt from the descriptors again), but not for ever */
		for (int i = 0; i < 30 && !g_rebind; i++) usleep(100000);
		g_ev_stop = true;
		ev.join();
		close(ep0);
		sleep(1);
	}
}
