/* Thin server on the NPU die: takes over the FunctionFS function "ntb", loads TFLite models (several at once, each in its own engine process),
 * runs them on the NPU through the TFLite VX delegate (TIM-VX) and frees them on request. The models stay loaded when the host goes away
 * (a program on the board can attach to a model that another one loaded). Protocol: protocol.h. A reader thread takes requests from the USB
 * endpoint while the worker runs the previous one; a LOAD runs in a thread of its own, so that the loaded models keep running meanwhile. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <linux/aio_abi.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "fix_dynamic_reshape.h"
#include "rewrite_ops.h"
#include "split_per_channel.h"
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
#define RUN_TIMEOUT_S 300   /* a float model on the slow float16 path of the NPU takes seconds per run; a hung engine is killed after this */
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
	std::vector<npu_op_stat> ops;   /* the operators and where they run (NPU_OPS) */
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
static std::string do_load(std::vector<uint8_t> &&req, std::vector<uint8_t> &reply)
{
	if (req.size() < sizeof(npu_load_req)) return "short request";
	npu_load_req lq; memcpy(&lq, req.data(), sizeof lq);
	std::vector<uint8_t> bytes(req.begin() + sizeof lq, req.end());
	req.clear(); req.shrink_to_fit();
	const bool cpu_only = lq.flags & NPU_LOAD_CPU_ONLY, no_split = lq.flags & NPU_LOAD_NO_SPLIT;
	std::lock_guard<std::mutex> lk(g_model_mu);
	if (g_model) return "this engine has a model already";
	uint32_t mo, re, pr; char drv[32]; chip_identity(&mo, &re, &pr, drv);
	if (mo != 0x8000 || re != 0x7100 || pr != 0x45080001) {
		char b[160]; snprintf(b, sizeof b, "unknown NPU chip %x/%x/%x: this build is made for 8000/7100/45080001 only", mo, re, pr); return b;
	}
	uint32_t avail = meminfo_kb("MemAvailable:");
	/* the model is kept in memory, TFLite makes a copy of the weights for the NPU and the graph takes more: 4x is a first guess */
	if ((uint64_t)bytes.size() / 1024 * 4 + 64 * 1024 > avail) {
		char b[200]; snprintf(b, sizeof b, "not enough memory on the NPU: the model needs about %zu MB, %u MB are free (stop a loaded model first)", bytes.size() / 1024 / 1024 * 4 + 64, avail / 1024); return b;
	}
	auto m = std::make_unique<Model>();
	m->bytes = std::move(bytes);
	uint64_t t0 = now_us();
	{   /* Keras tails (SHAPE -> STRIDED_SLICE -> PACK -> RESHAPE) -> a constant shape, see fix_dynamic_reshape.h */
		npu_reshape::Stats st; std::string e = npu_reshape::run(m->bytes, &st);
		if (!e.empty()) return e;
		if (st.fixed) logf_("graph-computed reshape shapes made constant: %d (%d operators removed)", st.fixed, st.removed);
	}
	if (!cpu_only && (!getenv("NPU_REWRITE") || getenv("NPU_REWRITE")[0] != '0')) {   /* SLICE, TILE, LOG_SOFTMAX into operators the NPU runs, see rewrite_ops.h */
		npu_rewrite::Stats st; std::string e = npu_rewrite::run(m->bytes, &st);
		if (!e.empty()) return e;
		if (st.slice + st.tile + st.log_softmax) logf_("operators rewritten for the NPU: SLICE %d, TILE %d, LOG_SOFTMAX %d", st.slice, st.tile, st.log_softmax);
	}
	if (!cpu_only && !no_split && (!getenv("NPU_SPLIT") || getenv("NPU_SPLIT")[0] != '0')) {   /* per-channel int8 layers -> per-tensor groups (fast path of the NPU, exact), see split_per_channel.h */
		npu_split::Stats st; std::string e = npu_split::run(m->bytes, 4.0, &st);
		if (!e.empty()) return e;
		if (st.layers) logf_("per-channel layers: %d, split into groups: %d (+%d convolutions, +%d additions)", st.layers, st.split, st.convs_added, st.adds);
	}
	m->fb = tflite::FlatBufferModel::BuildFromBuffer((const char *)m->bytes.data(), m->bytes.size());
	if (!m->fb) return "not a TFLite model (or a version this TFLite cannot read)";
	tflite::ops::builtin::BuiltinOpResolver resolver;
	tflite::InterpreterBuilder(*m->fb, resolver)(&m->interp);
	if (!m->interp) return "cannot build the TFLite interpreter";
	m->interp->SetNumThreads(2);   /* both Cortex-A35 for what stays on the CPU (XNNPACK, applied by TFLite after the VX delegate) */
	std::map<int, npu_op_stat> stat;   /* the operators before the delegate takes its part */
	for (int idx : m->interp->execution_plan()) {
		const auto *nr = m->interp->node_and_registration(idx);
		int c = nr->second.custom_name ? 0xffff : nr->second.builtin_code;
		auto &st = stat[c]; st.code = c; st.total++; st.on_npu++;
	}
	char nbg_file[64]; snprintf(nbg_file, sizeof nbg_file, "/tmp/npu_model.%d.nbg", (int)getpid());   /* engines of several models load at once */
	bool whole = false;
	if (!cpu_only) {
		std::string deny;
		for (int i = 0; i < NPU_MAX_DENY && lq.deny_ops[i]; i++) deny += (deny.empty() ? "" : ",") + std::to_string(lq.deny_ops[i]);
		if (!deny.empty()) setenv("VX_DENY_OPS", deny.c_str(), 1);
		setenv("VX_NBG_EXPORT", nbg_file, 1);   /* the delegate writes the compiled graph here */
		unlink(nbg_file);
		auto opt = TfLiteExternalDelegateOptionsDefault(DELEGATE_SO);
		m->delegate = TfLiteExternalDelegateCreate(&opt);
		if (!m->delegate) return "cannot load the VX delegate " DELEGATE_SO;
		if (m->interp->ModifyGraphWithDelegate(m->delegate) != kTfLiteOk) return "the VX delegate refused the model";
		int parts = 0, left = 0;
		for (int idx : m->interp->execution_plan()) {
			const auto *nr = m->interp->node_and_registration(idx);
			if (nr->first.delegate) { parts++; continue; }
			left++;
			int c = nr->second.custom_name ? 0xffff : nr->second.builtin_code;
			if (stat.count(c) && stat[c].on_npu) stat[c].on_npu--;
		}
		whole = parts == 1 && left == 0;   /* only then is the exported graph the whole model */
	} else {
		for (auto &kv : stat) kv.second.on_npu = 0;
	}
	for (auto &kv : stat) m->ops.push_back(kv.second);
	if (!cpu_only && ((lq.flags & NPU_LOAD_STRICT) || (getenv("NPU_STRICT") && getenv("NPU_STRICT")[0] == '1'))) {   /* the whole model on the NPU or an error naming what stays on the CPU */
		std::string cpu;
		for (int idx : m->interp->execution_plan()) {
			const auto *nr = m->interp->node_and_registration(idx);
			if (nr->first.delegate) continue;
			const char *nm = nr->second.custom_name ? nr->second.custom_name : tflite::EnumNameBuiltinOperator((tflite::BuiltinOperator)nr->second.builtin_code);
			if (cpu.find(nm) == std::string::npos) cpu += std::string(cpu.empty() ? "" : ",") + nm;
		}
		if (!cpu.empty()) return "not the whole model runs on the NPU: " + cpu + " would run on the CPU of the NPU die; use device=\"auto\" (NPU and CPU together) or device=\"cpu\"";
	}
	if (m->interp->AllocateTensors() != kTfLiteOk) return "cannot allocate the tensors";
	if (m->interp->Invoke() != kTfLiteOk) return "the first run failed";   /* the Vivante compiler builds the graph here (seconds) */
	describe(*m);
	npu_load_reply lr = { (uint32_t)m->interp->inputs().size(), (uint32_t)m->interp->outputs().size(), (uint32_t)((now_us() - t0) / 1000),
			      avail > meminfo_kb("MemAvailable:") ? avail - meminfo_kb("MemAvailable:") : 0, 0 };
	unsetenv("VX_NBG_EXPORT");
	std::string nbg = whole ? read_file(nbg_file) : std::string();
	unlink(nbg_file);
	/* a float input/output is converted by the delegate (the graph itself is float16): the cached graph would not match the tensor description */
	for (auto &d : m->info) if (d.type == 1) nbg.clear();
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
static std::string do_load_nbg(std::vector<uint8_t> &&req, std::vector<uint8_t> &reply)
{
	if (req.size() < sizeof(npu_load_req)) return "short request";
	std::vector<uint8_t> p(req.begin() + sizeof(npu_load_req), req.end());
	req.clear(); req.shrink_to_fit();
	std::lock_guard<std::mutex> lk(g_model_mu);
	if (g_model) return "this engine has a model already";
	uint32_t mo, re, pr; char drv[32]; chip_identity(&mo, &re, &pr, drv);
	if (mo != 0x8000 || re != 0x7100 || pr != 0x45080001) { char b[160]; snprintf(b, sizeof b, "unknown NPU chip %x/%x/%x", mo, re, pr); return b; }
	if (p.size() < 8) return "short request";
	uint32_t n_in, n_out; memcpy(&n_in, p.data(), 4); memcpy(&n_out, p.data() + 4, 4);
	size_t tl = (size_t)(n_in + n_out) * sizeof(npu_tensor);
	if (n_in == 0 || n_out == 0 || n_in > 16 || n_out > 16 || p.size() <= 8 + tl) return "bad tensor description";
	uint32_t avail = meminfo_kb("MemAvailable:");
	size_t nbg_len = p.size() - 8 - tl;
	if (nbg_len / 1024 * 2 + 32 * 1024 > avail) { char b[200]; snprintf(b, sizeof b, "not enough memory on the NPU: the compiled graph needs about %zu MB, %u MB are free (stop a loaded model first)", nbg_len / 1024 / 1024 * 2 + 32, avail / 1024); return b; }
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
		/* the delegate runs int8 tensors as uint8 (data ^ 0x80, zero point + 128): the compiled graph has uint8 inputs and outputs */
		const bool i8 = t.type == 9;
		tim::vx::TensorSpec spec(i8 ? tim::vx::DataType::UINT8 : vx_type(t.type), shape, attr);
		if (t.scale != 0 && (t.type == 3 || t.type == 9 || t.type == 7)) spec.SetQuantization(*new tim::vx::Quantization(tim::vx::QuantType::ASYMMETRIC, t.scale, t.zero_point + (i8 ? 128 : 0)));
		return m->graph->CreateTensor(spec);
	};
	for (uint32_t i = 0; i < n_in; i++) m->tin.push_back(make(m->info[i], tim::vx::TensorAttribute::INPUT));
	for (uint32_t i = 0; i < n_out; i++) m->tout.push_back(make(m->info[n_in + i], tim::vx::TensorAttribute::OUTPUT));
	auto op = m->graph->CreateOperation<tim::vx::ops::NBG>(m->nbg.data(), n_in, n_out);
	op->BindInputs(m->tin).BindOutputs(m->tout);
	if (!m->graph->Compile()) return "the NPU did not accept the compiled graph (made by another driver version or chip?)";
	npu_load_reply lr = { n_in, n_out, (uint32_t)((now_us() - t0) / 1000), avail > meminfo_kb("MemAvailable:") ? avail - meminfo_kb("MemAvailable:") : 0, 0 };
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
		for (size_t i = 0; i < m.n_in; i++) {
			const uint8_t *src = in.data() + off; std::vector<uint8_t> x;
			if (m.info[i].type == 9) { x.assign(src, src + m.info[i].bytes); for (auto &c : x) c ^= 0x80; src = x.data(); }   /* int8 -> uint8 */
			if (!m.tin[i]->CopyDataToTensor(src, m.info[i].bytes)) return "cannot copy the input";
			off += m.info[i].bytes;
		}
		uint64_t t0 = now_us();
		if (!m.graph->Run()) return "Run failed";
		uint32_t us = (uint32_t)(now_us() - t0);
		size_t total = 4;
		for (size_t i = 0; i < m.n_out; i++) total += m.info[m.n_in + i].bytes;
		out.resize(total); memcpy(out.data(), &us, 4); off = 4;
		for (size_t i = 0; i < m.n_out; i++) {
			if (!m.tout[i]->CopyDataFromTensor(out.data() + off)) return "cannot copy the output";
			if (m.info[m.n_in + i].type == 9) for (size_t k = 0; k < m.info[m.n_in + i].bytes; k++) out[off + k] ^= 0x80;   /* uint8 -> int8 */
			off += m.info[m.n_in + i].bytes;
		}
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

/* usb = true: a message for the host (padded so that it does not end on a multiple of 1024, see protocol.h); false: a message between the processes */
static bool send_msg(int fd, const npu_hdr &req, bool err, const void *p, size_t n, bool usb)
{
	static std::mutex wmu; std::lock_guard<std::mutex> lk(wmu);
	bool pad = usb && ((sizeof(npu_hdr) + n) % 1024) == 0;
	npu_hdr h = { NPU_MAGIC, req.type, (uint16_t)((err ? NPU_FLAG_ERROR : 0) | (pad ? NPU_FLAG_PAD : 0)), req.tag, req.model, (uint32_t)n };
	static std::vector<uint8_t> buf; buf.resize(sizeof h + n + (pad ? 1 : 0));
	memcpy(buf.data(), &h, sizeof h);
	if (n) memcpy(buf.data() + sizeof h, p, n);
	if (pad) buf[sizeof h + n] = 0;
	if (usb) {
		static const int gap = getenv("NPU_REPLY_GAP_US") ? atoi(getenv("NPU_REPLY_GAP_US")) : REPLY_GAP_US;   /* experiment */
		if (gap) usleep(gap);
		g_wstate = "writing a reply to USB";
	}
	return write_all(fd, buf.data(), buf.size());
}
static bool reply(int in_fd, const npu_hdr &req, bool err, const void *p, size_t n) { return send_msg(in_fd, req, err, p, n, true); }
static bool reply_err(int in_fd, const npu_hdr &req, const std::string &t) { logf_("error reply: %s", t.c_str()); return reply(in_fd, req, true, t.data(), t.size()); }

/* ================= the engine: a child process that owns the model =================
 * The parent keeps the USB function and the protocol and never touches TFLite or TIM-VX. A model that crashes the delegate or the driver
 * (segmentation fault, abort) or hangs kills only this child; the parent answers the host with the reason and stays on the bus. */
static bool read_exact(int fd, void *b, size_t n)
{
	uint8_t *p = (uint8_t *)b;
	while (n) { ssize_t r = read(fd, p, n); if (r < 0) { if (errno == EINTR) continue; return false; } if (r == 0) return false; p += r; n -= r; }
	return true;
}
static int engine_main(int fd)
{
	g_log = fopen("/tmp/npu_server.log", "a");
	for (;;) {
		npu_hdr h;
		if (!read_exact(fd, &h, sizeof h) || h.magic != NPU_MAGIC || h.len > MAX_MSG) return 0;
		std::vector<uint8_t> payload(h.len);
		if (h.len && !read_exact(fd, payload.data(), h.len)) return 0;
		std::vector<uint8_t> out; std::string e;
		switch (h.type) {
		case NPU_LOAD: e = do_load(std::move(payload), out); break;
		case NPU_LOAD_NBG: e = do_load_nbg(std::move(payload), out); break;
		case NPU_RUN: e = do_run(payload, out); break;
		case NPU_OPS: {
			std::lock_guard<std::mutex> lk(g_model_mu);
			if (!g_model) { e = "no model loaded"; break; }
			uint32_t n = g_model->ops.size(); out.resize(4 + n * sizeof(npu_op_stat)); memcpy(out.data(), &n, 4);
			if (n) memcpy(out.data() + 4, g_model->ops.data(), n * sizeof(npu_op_stat));
			break; }
		default: e = "the engine does not know this request";
		}
		if ((h.type == NPU_LOAD || h.type == NPU_LOAD_NBG) && e.empty()) { if (!freopen("/dev/null", "w", stderr)) {} }   /* the delegate prints on every run: only the load is logged */
		bool ok = e.empty() ? send_msg(fd, h, false, out.data(), out.size(), false) : send_msg(fd, h, true, e.data(), e.size(), false);
		if (!ok) return 0;
	}
}

/* one engine per model; the table is guarded by g_eng_mu, every engine by its own mutex (one request at a time on its socket) */
struct Engine {
	uint32_t id = 0;
	pid_t pid = -1;
	int fd = -1;
	std::mutex mu;
	std::string key, name, plan;
	std::vector<uint8_t> desc;   /* the LOAD reply without the compiled graph: npu_load_reply + tensors + u32 0 (for DESCRIBE and attaching) */
	npu_load_reply lr {};
	bool from_nbg = false;
	uint64_t loaded_us = 0;
	uint32_t runs = 0;
	std::string log;
};
static std::map<uint32_t, std::shared_ptr<Engine>> g_engines;
static std::mutex g_eng_mu, g_load_mu;
static uint32_t g_next_id = 1;

static std::shared_ptr<Engine> engine_get(uint32_t id) { std::lock_guard<std::mutex> lk(g_eng_mu); auto it = g_engines.find(id); return it == g_engines.end() ? nullptr : it->second; }
static size_t engine_count() { std::lock_guard<std::mutex> lk(g_eng_mu); return g_engines.size(); }

static void engine_kill(Engine &e)
{
	if (e.pid > 0) { kill(e.pid, SIGKILL); int st; waitpid(e.pid, &st, 0); logf_("engine %d of model %u stopped", (int)e.pid, e.id); }
	if (e.fd >= 0) close(e.fd);
	e.pid = -1; e.fd = -1;
}
static bool engine_spawn(Engine &e)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return false;
	int lg = open(e.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	pid_t pid = fork();
	if (pid < 0) { close(sv[0]); close(sv[1]); if (lg >= 0) close(lg); return false; }
	if (pid == 0) {
		close(sv[0]);
		if (sv[1] != 3) { dup2(sv[1], 3); close(sv[1]); }
		if (lg >= 0) { dup2(lg, 2); if (lg != 2) close(lg); }
		for (int i = 4; i < 256; i++) close(i);
		signal(SIGPIPE, SIG_DFL);
		execl("/proc/self/exe", "npu_server", "--engine", "3", (char *)NULL);
		_exit(127);
	}
	close(sv[1]); if (lg >= 0) close(lg);
	fcntl(sv[0], F_SETFD, FD_CLOEXEC);
	e.pid = pid; e.fd = sv[0];
	return true;
}
static std::string engine_log_tail(const Engine &e)
{
	std::string s = read_file(e.log.c_str()), keep;
	size_t pos = 0; int lines = 0;
	std::vector<std::string> v;
	while (pos < s.size()) { size_t q = s.find('\n', pos); if (q == std::string::npos) q = s.size(); v.push_back(s.substr(pos, q - pos)); pos = q + 1; }
	for (size_t i = v.size(); i > 0 && lines < 4; i--) { if (v[i - 1].empty() || v[i - 1].find("Tensor has not beed inserted") != std::string::npos) continue; keep = v[i - 1] + (keep.empty() ? "" : " | ") + keep; lines++; }
	return keep.size() > 400 ? keep.substr(keep.size() - 400) : keep;
}
/* forward a request to an engine (its mutex held) and wait for the reply; false = the engine is gone or silent (err_text says why, the engine is stopped) */
static bool engine_call(Engine &g, const npu_hdr &h0, const uint8_t *p, size_t n, int timeout_s, npu_hdr &rh, std::vector<uint8_t> &rp, std::string &err_text)
{
	npu_hdr hh = h0; hh.flags = 0; hh.len = n;
	bool wrote = write_all(g.fd, &hh, sizeof hh) && (!n || write_all(g.fd, p, n));
	auto dead = [&](const char *what) {
		int st = 0; pid_t r = waitpid(g.pid, &st, WNOHANG);
		char b[160];
		if (r == g.pid && WIFSIGNALED(st)) snprintf(b, sizeof b, "the engine process %s: killed by signal %d (%s)", what, WTERMSIG(st), strsignal(WTERMSIG(st)));
		else if (r == g.pid) snprintf(b, sizeof b, "the engine process %s: exit status %d", what, WEXITSTATUS(st));
		else snprintf(b, sizeof b, "the engine process %s", what);
		err_text = b; std::string t = engine_log_tail(g); if (!t.empty()) err_text += ". Last lines of its log: " + t;
		if (r == g.pid) g.pid = -1;   /* already reaped */
		engine_kill(g);
	};
	if (!wrote) { dead("went away"); return false; }
	auto wait_read = [&](void *b, size_t len) {
		uint8_t *q = (uint8_t *)b;
		while (len) {
			struct pollfd pf = { g.fd, POLLIN, 0 };
			int pr = poll(&pf, 1, timeout_s * 1000);
			if (pr == 0) return 2;
			if (pr < 0) { if (errno == EINTR) continue; return 1; }
			ssize_t r = read(g.fd, q, len);
			if (r <= 0) { if (r < 0 && errno == EINTR) continue; return 1; }
			q += r; len -= r;
		}
		return 0;
	};
	int rc = wait_read(&rh, sizeof rh);
	if (rc == 0 && (rh.magic != NPU_MAGIC || rh.len > MAX_MSG)) rc = 1;
	if (rc == 0) { rp.assign(rh.len, 0); if (rh.len) rc = wait_read(rp.data(), rh.len); }
	if (rc == 2) { char b[40]; snprintf(b, sizeof b, "did not answer in %d s (killed)", timeout_s); dead(b); return false; }
	if (rc != 0) { dead("crashed"); return false; }
	return true;
}
static void engine_remove(uint32_t id)
{
	std::shared_ptr<Engine> e;
	{ std::lock_guard<std::mutex> lk(g_eng_mu); auto it = g_engines.find(id); if (it == g_engines.end()) return; e = it->second; g_engines.erase(it); }
	std::lock_guard<std::mutex> lk(e->mu);   /* waits for a RUN in progress */
	engine_kill(*e);
	unlink(e->log.c_str());
	logf_("model %u (%s) unloaded", id, e->name.c_str());
}

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
		hl.models = engine_count();
		reply(in_fd, m.h, false, &hl, sizeof hl); break; }
	case NPU_LOAD: case NPU_LOAD_NBG: {
		if (m.payload.size() < sizeof(npu_load_req)) { reply_err(in_fd, m.h, "short request (an older host library?)"); break; }
		npu_load_req lq; memcpy(&lq, m.payload.data(), sizeof lq);
		lq.key[sizeof lq.key - 1] = 0; lq.name[sizeof lq.name - 1] = 0;
		{   /* the same model is loaded already (another program on the board loaded it, or two loaded it at once): attach */
			std::lock_guard<std::mutex> lk(g_eng_mu);
			for (auto &kv : g_engines) if (lq.key[0] && kv.second->key == lq.key) {
				npu_hdr rh = m.h; rh.model = kv.first;
				std::vector<uint8_t> d = kv.second->desc; uint32_t one = 1; memcpy(d.data() + offsetof(npu_load_reply, attached), &one, 4);
				reply(in_fd, rh, false, d.data(), d.size());
				return;
			}
		}
		auto e = std::make_shared<Engine>();
		{ std::lock_guard<std::mutex> lk(g_eng_mu); e->id = g_next_id++; }
		e->key = lq.key; e->name = lq.name; e->from_nbg = m.h.type == NPU_LOAD_NBG;
		if (e->from_nbg) e->plan = "npu";
		else if (lq.flags & NPU_LOAD_CPU_ONLY) e->plan = "cpu";
		else {
			std::string d; for (int i = 0; i < NPU_MAX_DENY && lq.deny_ops[i]; i++) d += (d.empty() ? "" : ",") + std::to_string(lq.deny_ops[i]);
			e->plan = d.empty() ? ((lq.flags & NPU_LOAD_NO_SPLIT) ? "npu-nosplit" : "npu") : "mixed:" + d + ((lq.flags & NPU_LOAD_NO_SPLIT) ? ":nosplit" : "");
		}
		e->log = "/tmp/npu_engine." + std::to_string(e->id) + ".log";
		if (!engine_spawn(*e)) { reply_err(in_fd, m.h, "cannot start the engine process"); break; }
		npu_hdr rh; std::string et;
		std::lock_guard<std::mutex> elk(e->mu);
		if (!engine_call(*e, m.h, m.payload.data(), m.payload.size(), 600, rh, out, et)) { unlink(e->log.c_str()); reply_err(in_fd, m.h, et); break; }
		if (rh.flags & NPU_FLAG_ERROR) { std::string msg((const char *)out.data(), out.size()); engine_kill(*e); unlink(e->log.c_str()); reply_err(in_fd, m.h, msg); break; }
		if (out.size() < sizeof(npu_load_reply)) { engine_kill(*e); reply_err(in_fd, m.h, "short reply of the engine"); break; }
		memcpy(&e->lr, out.data(), sizeof e->lr);
		size_t tl = (size_t)(e->lr.n_inputs + e->lr.n_outputs) * sizeof(npu_tensor);
		e->desc.assign(out.begin(), out.begin() + sizeof(npu_load_reply) + tl);
		e->desc.insert(e->desc.end(), 4, 0);
		e->loaded_us = now_us();
		{ std::lock_guard<std::mutex> lk(g_eng_mu); g_engines[e->id] = e; }
		logf_("model %u (%s) loaded: %u ms, %u MB; %zu model(s) now", e->id, e->name.c_str(), e->lr.load_ms, e->lr.mem_used_kb / 1024, engine_count());
		npu_hdr oh = m.h; oh.model = e->id;
		reply(in_fd, oh, false, out.data(), out.size()); break; }
	case NPU_RUN: {
		auto e = engine_get(m.h.model);
		if (!e) { reply_err(in_fd, m.h, "no such model on the NPU (stopped by another program?)"); break; }
		npu_hdr rh; std::string et; bool ok;
		{ std::lock_guard<std::mutex> lk(e->mu);
		  if (e->pid <= 0) { reply_err(in_fd, m.h, "the model was stopped"); break; }
		  ok = engine_call(*e, m.h, m.payload.data(), m.payload.size(), RUN_TIMEOUT_S, rh, out, et); if (ok) e->runs++; }
		if (!ok) { engine_remove(m.h.model); reply_err(in_fd, m.h, et + " (the model is unloaded)"); break; }
		if (rh.flags & NPU_FLAG_ERROR) reply_err(in_fd, m.h, std::string((const char *)out.data(), out.size())); else reply(in_fd, m.h, false, out.data(), out.size());
		break; }
	case NPU_OPS: {
		auto e = engine_get(m.h.model);
		if (!e) { reply_err(in_fd, m.h, "no such model on the NPU"); break; }
		npu_hdr rh; std::string et; bool ok;
		{ std::lock_guard<std::mutex> lk(e->mu); if (e->pid <= 0) { reply_err(in_fd, m.h, "the model was stopped"); break; }
		  ok = engine_call(*e, m.h, nullptr, 0, 30, rh, out, et); }
		if (!ok) { engine_remove(m.h.model); reply_err(in_fd, m.h, et); break; }
		if (rh.flags & NPU_FLAG_ERROR) reply_err(in_fd, m.h, std::string((const char *)out.data(), out.size())); else reply(in_fd, m.h, false, out.data(), out.size());
		break; }
	case NPU_UNLOAD: {
		if (m.h.model == NPU_ALL_MODELS) {
			std::vector<uint32_t> ids; { std::lock_guard<std::mutex> lk(g_eng_mu); for (auto &kv : g_engines) ids.push_back(kv.first); }
			for (uint32_t id : ids) engine_remove(id);
		} else if (!engine_get(m.h.model)) { reply_err(in_fd, m.h, "no such model on the NPU"); break; }
		else engine_remove(m.h.model);
		reply(in_fd, m.h, false, nullptr, 0); break; }
	case NPU_LIST: {
		std::lock_guard<std::mutex> lk(g_eng_mu);
		uint32_t n = g_engines.size();
		out.resize(4 + n * sizeof(npu_model_entry)); memcpy(out.data(), &n, 4);
		size_t off = 4;
		for (auto &kv : g_engines) {
			const Engine &e = *kv.second;
			npu_model_entry me; memset(&me, 0, sizeof me);
			me.id = e.id; me.n_inputs = e.lr.n_inputs; me.n_outputs = e.lr.n_outputs; me.load_ms = e.lr.load_ms; me.mem_used_kb = e.lr.mem_used_kb;
			me.runs = e.runs; me.age_s = (uint32_t)((now_us() - e.loaded_us) / 1000000); me.from_nbg = e.from_nbg;
			snprintf(me.key, sizeof me.key, "%s", e.key.c_str()); snprintf(me.name, sizeof me.name, "%s", e.name.c_str()); snprintf(me.plan, sizeof me.plan, "%s", e.plan.c_str());
			memcpy(out.data() + off, &me, sizeof me); off += sizeof me;
		}
		reply(in_fd, m.h, false, out.data(), out.size()); break; }
	case NPU_DESCRIBE: {
		auto e = engine_get(m.h.model);
		if (!e) { reply_err(in_fd, m.h, "no such model on the NPU"); break; }
		std::vector<uint8_t> d = e->desc; uint32_t one = 1; memcpy(d.data() + offsetof(npu_load_reply, attached), &one, 4);
		reply(in_fd, m.h, false, d.data(), d.size()); break; }
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
		if (m.h.type == NPU_LOAD || m.h.type == NPU_LOAD_NBG) {   /* minutes for a big model: the loaded models keep running meanwhile */
			std::thread([in_fd](Msg mm) { std::lock_guard<std::mutex> lk(g_load_mu); handle(in_fd, mm); g_tx++; }, std::move(m)).detach();
			continue;
		}
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

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	if (argc >= 3 && strcmp(argv[1], "--engine") == 0) return engine_main(atoi(argv[2]));
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
		logf_("host disconnected (errno %d): the models stay loaded", errno);
		{ std::lock_guard<std::mutex> lk(q_mu); q_closed = true; q_cv.notify_all(); }
		close(out); close(in);
		w.join();
		/* ep0: wait for the UNBIND event (the function is rebuilt from the descriptors again), but not for ever */
		for (int i = 0; i < 30 && !g_rebind; i++) usleep(100000);
		g_ev_stop = true;
		ev.join();
		close(ep0);
		sleep(1);
	}
}
