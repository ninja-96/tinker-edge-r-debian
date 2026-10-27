// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the tinker-edge-r project contributors
//
// rk3399pro_npu: TensorFlow Lite models on the NPU of the RK3399Pro (ASUS Tinker Edge R), C++ API.
//
// The NPU die runs our server (npu_server: TIM-VX, the TFLite VX delegate, TensorFlow Lite 2.21). Several models can be loaded at once and
// used by several programs: a model stays on the NPU until it is stopped, and opening the same .tflite file again attaches to the loaded
// model (it is identified by the sha256 of the file). Programs reach the NPU through the daemon npu-daemon (/run/npu/npu.sock, group plugdev);
// without the daemon (or with NPU_DIRECT=1) one program at a time talks to the NPU over USB itself.
//
//     #include <rk3399pro_npu.hpp>
//     rk3399pro_npu::Model net("mobilenet_v2_quant.tflite");          // loads it, or attaches to it if it is loaded
//     std::vector<uint8_t> x(net.inputs()[0].bytes);
//     auto out = net.exec({x});                                        // the output tensors, as bytes
//     net.close();                                                     // the model stays loaded; net.stop() unloads it
//
// Link with -lrk3399pro_npu. The Python module rk3399pro_npu is built on this library.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rk3399pro_npu {

// The NPU does not answer, refuses a model, cannot be reached, or a call is wrong.
struct Error : std::runtime_error {
	explicit Error(const std::string &what) : std::runtime_error(what) {}
};

// A model input or output. type: the TfLiteType (1 float32, 2 int32, 3 uint8, 7 int16, 9 int8, 10 float16 ...); scale 0 for a float tensor.
struct TensorInfo {
	std::string name;
	int type = 0;
	std::vector<int> shape;
	float scale = 0;
	int zero_point = 0;
	size_t bytes = 0;
};

// A model loaded on the NPU, as models() lists it.
struct ModelInfo {
	uint32_t id = 0;
	std::string name, key;            // key: sha256 of the .tflite file (hex)
	uint32_t inputs = 0, outputs = 0;
	uint32_t load_ms = 0, mem_used_kb = 0, runs = 0, age_s = 0;
	bool from_cache = false;          // loaded from a compiled graph
	std::string plan;                 // where it runs: "npu", "npu-nosplit", "cpu", "mixed:<operator codes on the CPU>[:nosplit]"
};

// An operator type of a model and where it runs (Model::ops()).
struct OpStat {
	int code = 0;                     // TfLiteBuiltinOperator (0xffff: a custom operator)
	std::string name;                 // CONV_2D, ADD, ...
	int total = 0, on_npu = 0;        // operators of this type, and how many of them the NPU runs (the rest: TensorFlow Lite with XNNPACK on the CPU of the NPU die)
};

// One candidate of Model::autotune().
struct TuneResult {
	std::string plan;                 // "npu", "npu-nosplit", "cpu", "mixed:<operator codes on the CPU>[:nosplit]"
	double ms = 0;                    // time of one exec(), with the USB transfer
	double cosine = 0;                // the worst cosine similarity of the outputs to the reference (the model on the CPU)
	bool ok = false;                  // cosine >= the tolerance
	std::string error;                // the candidate could not be loaded or run
};

// The state of the NPU (info()).
struct NpuInfo {
	uint32_t proto = 0;
	uint32_t chip_model = 0, chip_revision = 0, chip_product = 0;
	bool chip_ok = false;             // the chip this software is made for: 8000/7100/45080001
	uint32_t mem_total_kb = 0, mem_avail_kb = 0;
	uint32_t models = 0;              // number of loaded models
	std::string driver;               // galcore version of the NPU die
	std::string via;                  // "daemon" or "usb"
};

// An NPU die on the USB bus.
struct Device {
	int bus = 0, dev = 0;
	std::string serial, sysfs;
};

struct LoadOptions {
	std::string name;                 // the name in models() (default: the file name)
	// where the model runs: "auto" (NPU and CPU together: the plan found by autotune() if there is one, else integer models on the NPU with what it cannot
	// do on the CPU, and float models on the CPU of the NPU die, which is faster for them than the float16 path of this NPU); "npu" (strictly the whole model
	// on the NPU: an Error naming the operators otherwise); "cpu" (strictly the whole model on the CPU of the NPU die: TensorFlow Lite with XNNPACK on its two A35)
	std::string device = "auto";
	bool split = true;                // device "npu": split per-channel int8 layers into per-tensor groups (the fast path of the NPU)
	std::vector<int> cpu_ops;         // internal (autotune, plans): operator codes (TfLiteBuiltinOperator) to leave to the CPU
	std::string key_suffix;           // internal (autotune): load a second copy next to the loaded one
	bool strict = true;               // internal: false lets device "npu" leave what the NPU cannot do to the CPU (the "auto" plans)
	int load_timeout_s = 600;         // the first load of a big int8 model takes a minute or more
	bool use_cache = true;            // keep the compiled graph (NBG) in cache_dir and load from it next time
	std::string cache_dir;            // default: $NPU_CACHE or ~/.cache/npu-models
	int max_inflight = 2;             // async_exec: at most this many runs of this model on their way
	int async_gap_us = 300;           // async_exec: pause before each request (a USB hang of this SoC, see the documentation)
};

class Link;

// A TFLite model on the NPU.
//
// The constructor attaches to the model if it is loaded already (attached() is true: nothing is loaded or checked); otherwise it checks that
// the model fits into the free memory of the NPU die (the memory measured at an earlier load of the same model, kept next to the cache, or an
// estimate from the size) and loads it: from the cached compiled graph if there is one (a fraction of a second), else from the .tflite file
// (13 s for MobileNet V2 uint8, a minute or more for big int8 models). Throws Error.
//
// Several threads may use one Model; runs of one model are served one after another by the NPU server.
class Model {
public:
	explicit Model(const std::string &path, const LoadOptions &opt = LoadOptions());
	Model(const std::vector<uint8_t> &tflite, const LoadOptions &opt);
	~Model();                         // close()
	Model(const Model &) = delete;
	Model &operator=(const Model &) = delete;

	const std::vector<TensorInfo> &inputs() const { return inputs_; }
	const std::vector<TensorInfo> &outputs() const { return outputs_; }
	uint32_t id() const { return id_; }
	const std::string &name() const { return name_; }
	const std::string &key() const { return key_; }
	bool attached() const { return attached_; }
	bool from_cache() const { return from_cache_; }
	uint32_t load_ms() const { return load_ms_; }
	uint32_t mem_used_kb() const { return mem_kb_; }
	uint32_t last_invoke_us() const;  // time of the last run on the NPU die
	const std::string &plan() const { return plan_; }   // where it runs: "npu", "npu-nosplit", "cpu", "mixed:..." ("?" when attached to the model of another program)
	std::vector<OpStat> ops();        // the operator types of the model and where they run (empty for a model loaded from its compiled graph: all on the NPU)

	// Find the fastest way to run this model whose results match the model on the CPU: the whole model on the NPU, without the split of per-channel
	// layers, operator types moved to the CPU one by one (a mixed plan), or the CPU only. sample: inputs to compare with (empty: random data; real data
	// is better); tolerance: the lowest cosine similarity of the outputs to the reference accepted; runs: exec() calls timed per candidate; max_trials: mixed
	// plans tried. Every candidate is loaded once (for big models: minutes); the loaded copy of this model is unloaded meanwhile (programs attached to it
	// lose it). The best plan is kept in the cache (later loads with device "auto" use it) and this object is loaded with it. Returns the candidates, the chosen one first.
	std::vector<TuneResult> autotune(const std::vector<std::vector<uint8_t>> &sample = {}, double tolerance = 0.98, int runs = 5, int max_trials = 8);
	bool closed() const { return !link_; }

	// One run. in[i] points to the bytes of input i (inputs()[i].bytes long, in the dtype of the model); out[i] receives output i
	// (outputs()[i].bytes). Throws Error (for example when the model was stopped by another program).
	void exec(const std::vector<const void *> &in, const std::vector<void *> &out, int timeout_s = 30);
	// The same with vectors: returns the outputs.
	std::vector<std::vector<uint8_t>> exec(const std::vector<std::vector<uint8_t>> &in, int timeout_s = 30);

	// EXPERIMENTAL. Queue a run and return at once; done(outputs, error) is called from the reader thread (error empty on success).
	// Waits while max_inflight runs of this model are on their way.
	using Done = std::function<void(std::vector<std::vector<uint8_t>> &&outputs, const std::string &error)>;
	void async_exec(const std::vector<const void *> &in, Done done);

	void close();                     // let go of the model: it stays loaded on the NPU (for this or other programs)
	void stop();                      // unload it from the NPU (for every program) and close

private:
	void init(const std::vector<uint8_t> &data, const LoadOptions &opt, const std::string &default_name);
	std::string cache_key() const;
	std::vector<uint8_t> load(const std::vector<uint8_t> &data, const LoadOptions &opt);
	std::vector<uint8_t> pack(const std::vector<const void *> &in) const;
	void parse_desc(const std::vector<uint8_t> &reply);
	std::shared_ptr<Link> link_;
	std::vector<TensorInfo> inputs_, outputs_;
	uint32_t id_ = 0, load_ms_ = 0, mem_kb_ = 0;
	std::string name_, key_, plan_;
	std::vector<uint8_t> data_;       // the .tflite file (for autotune)
	LoadOptions opt_;
	bool attached_ = false, from_cache_ = false;
	NpuInfo hello_;
	struct Slots;
	std::shared_ptr<Slots> slots_;
	int gap_us_ = 300;
};

NpuInfo info();                                   // the state of the NPU, without loading anything
std::vector<ModelInfo> models();                  // the loaded models (of every program), by id
int stop(uint32_t id);                            // unload one model; returns 1, or 0 if there is no such model
int stop(const std::string &name_key_or_all);     // by name or sha256 key, or "all"; returns the number unloaded
std::vector<Device> find_devices();               // the NPU dies on the USB bus (empty if its firmware is not loaded)
std::string sha256_hex(const void *data, size_t n);
bool is_quantized(const std::vector<uint8_t> &tflite);   // most of the constant data of the model is int8 / uint8 (it belongs on the NPU)

}  // namespace rk3399pro_npu
