// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the tinker-edge-r project contributors
// rk3399pro_npu._core: the Python binding (pybind11) of the C++ library. The Python package rk3399pro_npu wraps it (dtype conversion,
// quantisation, docstrings); use the package, not this module.
#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "rk3399pro_npu.hpp"

namespace py = pybind11;
using namespace rk3399pro_npu;

static py::dtype dtype_of(int t)
{
	switch (t) {
	case 1: return py::dtype("float32"); case 2: return py::dtype("int32"); case 3: return py::dtype("uint8"); case 4: return py::dtype("int64");
	case 6: return py::dtype("bool"); case 7: return py::dtype("int16"); case 9: return py::dtype("int8"); case 10: return py::dtype("float16");
	case 11: return py::dtype("float64"); default: return py::dtype("uint8");
	}
}

static py::array new_output(const TensorInfo &t)
{
	std::vector<py::ssize_t> shape(t.shape.begin(), t.shape.end());
	py::array a(dtype_of(t.type), shape);
	if ((size_t)a.nbytes() != t.bytes) a = py::array(py::dtype("uint8"), std::vector<py::ssize_t>{ (py::ssize_t)t.bytes });   // an odd tensor: raw bytes
	return a;
}

/* the inputs as C-contiguous buffers of exactly the size the model wants (the Python package converts dtypes before) */
static std::vector<py::buffer_info> input_buffers(const Model &m, const py::list &xs)
{
	if (xs.size() != m.inputs().size())
		throw Error("the model has " + std::to_string(m.inputs().size()) + " input(s), " + std::to_string(xs.size()) + " given");
	std::vector<py::buffer_info> bi;
	for (size_t i = 0; i < xs.size(); i++) {
		py::buffer b = xs[i].cast<py::buffer>();
		py::buffer_info info = b.request();
		size_t n = (size_t)info.size * info.itemsize;
		if (n != m.inputs()[i].bytes)
			throw Error("input " + m.inputs()[i].name + ": " + std::to_string(n) + " bytes, the model wants " + std::to_string(m.inputs()[i].bytes));
		bool contiguous = true; py::ssize_t stride = info.itemsize;
		for (int d = (int)info.ndim - 1; d >= 0; d--) { if (info.shape[d] > 1 && info.strides[d] != stride) contiguous = false; stride *= info.shape[d]; }
		if (!contiguous) throw Error("input " + m.inputs()[i].name + " is not C-contiguous");
		bi.push_back(std::move(info));
	}
	return bi;
}

PYBIND11_MODULE(_core, mod)
{
	mod.doc() = "C++ core of rk3399pro_npu (use the package rk3399pro_npu)";
	py::register_exception<Error>(mod, "NpuError", PyExc_RuntimeError);

	py::class_<TensorInfo>(mod, "TensorInfo")
		.def_readonly("name", &TensorInfo::name).def_readonly("type", &TensorInfo::type)
		.def_property_readonly("shape", [](const TensorInfo &t) { return py::tuple(py::cast(t.shape)); })
		.def_readonly("scale", &TensorInfo::scale).def_readonly("zero_point", &TensorInfo::zero_point).def_readonly("bytes", &TensorInfo::bytes)
		.def_property_readonly("dtype", [](const TensorInfo &t) { return dtype_of(t.type); });

	py::class_<ModelInfo>(mod, "ModelInfo")
		.def_readonly("id", &ModelInfo::id).def_readonly("name", &ModelInfo::name).def_readonly("key", &ModelInfo::key)
		.def_readonly("inputs", &ModelInfo::inputs).def_readonly("outputs", &ModelInfo::outputs).def_readonly("load_ms", &ModelInfo::load_ms)
		.def_readonly("mem_used_kb", &ModelInfo::mem_used_kb).def_readonly("runs", &ModelInfo::runs).def_readonly("age_s", &ModelInfo::age_s)
		.def_readonly("from_cache", &ModelInfo::from_cache).def_readonly("plan", &ModelInfo::plan);

	py::class_<OpStat>(mod, "OpStat")
		.def_readonly("code", &OpStat::code).def_readonly("name", &OpStat::name).def_readonly("total", &OpStat::total).def_readonly("on_npu", &OpStat::on_npu);

	py::class_<Model>(mod, "Model")
		.def(py::init([](py::object src, const std::string &name, int load_timeout, bool use_cache, const std::string &cache_dir, int max_inflight, int gap,
				 const std::string &device, bool split, const std::vector<int> &cpu_ops) {
			LoadOptions o; o.name = name; o.load_timeout_s = load_timeout; o.use_cache = use_cache; o.cache_dir = cache_dir; o.max_inflight = max_inflight; o.async_gap_us = gap;
			o.device = device; o.split = split; o.cpu_ops = cpu_ops;
			if (py::isinstance<py::bytes>(src)) {
				std::string s = src.cast<std::string>();
				std::vector<uint8_t> d(s.begin(), s.end());
				py::gil_scoped_release nogil;
				return new Model(d, o);
			}
			std::string path = src.cast<std::string>();
			py::gil_scoped_release nogil;   // a load takes seconds to minutes: other Python threads go on
			return new Model(path, o);
		}), py::arg("src"), py::arg("name") = "", py::arg("load_timeout") = 600, py::arg("use_cache") = true, py::arg("cache_dir") = "",
		    py::arg("max_inflight") = 2, py::arg("async_gap_us") = 300, py::arg("device") = "auto", py::arg("split") = true, py::arg("cpu_ops") = std::vector<int>())
		.def_property_readonly("id", &Model::id).def_property_readonly("name", &Model::name).def_property_readonly("key", &Model::key)
		.def_property_readonly("attached", &Model::attached).def_property_readonly("from_cache", &Model::from_cache)
		.def_property_readonly("load_ms", &Model::load_ms).def_property_readonly("mem_used_kb", &Model::mem_used_kb)
		.def_property_readonly("last_invoke_us", &Model::last_invoke_us).def_property_readonly("closed", &Model::closed)
		.def_property_readonly("inputs", &Model::inputs).def_property_readonly("outputs", &Model::outputs)
		.def_property_readonly("plan", &Model::plan)
		.def("ops", [](Model &m) { py::gil_scoped_release nogil; return m.ops(); })
		.def("autotune", [](Model &m, py::object sample, double tolerance, int runs, int max_trials) {
			std::vector<std::vector<uint8_t>> x;
			if (!sample.is_none()) {
				auto bi = input_buffers(m, sample.cast<py::list>());
				for (auto &b : bi) { const uint8_t *p = (const uint8_t *)b.ptr; x.emplace_back(p, p + (size_t)b.size * b.itemsize); }
			}
			std::vector<TuneResult> r;
			{ py::gil_scoped_release nogil; r = m.autotune(x, tolerance, runs, max_trials); }   // minutes: every candidate is compiled once
			py::list out;
			for (auto &t : r) { py::dict d; d["plan"] = t.plan; d["ms"] = t.ms; d["cosine"] = t.cosine; d["ok"] = t.ok; d["error"] = t.error; out.append(d); }
			return out;
		}, py::arg("sample") = py::none(), py::arg("tolerance") = 0.98, py::arg("runs") = 5, py::arg("max_trials") = 8)
		.def("exec", [](Model &m, const py::list &xs, int timeout) {
			auto bi = input_buffers(m, xs);
			std::vector<const void *> in; for (auto &b : bi) in.push_back(b.ptr);
			py::list res; std::vector<void *> out;
			for (auto &t : m.outputs()) { py::array a = new_output(t); out.push_back(a.mutable_data()); res.append(a); }
			{ py::gil_scoped_release nogil; m.exec(in, out, timeout); }   // the outputs are written straight into the numpy arrays
			return res;
		}, py::arg("inputs"), py::arg("timeout") = 30)
		.def("async_exec", [](Model &m, const py::list &xs, py::function cb) {
			auto bi = input_buffers(m, xs);
			std::vector<const void *> in; for (auto &b : bi) in.push_back(b.ptr);
			std::vector<TensorInfo> outs = m.outputs();
			auto keep = std::make_shared<py::function>(cb);
			auto done = [keep, outs](std::vector<std::vector<uint8_t>> &&o, const std::string &err) {
				py::gil_scoped_acquire gil;
				py::function f = std::move(*keep);   // released here, under the GIL (the C++ side drops its copy of this lambda without it)
				if (!f) return;
				try {
					if (!err.empty()) { f(py::none(), err); return; }
					py::list res;
					for (size_t i = 0; i < outs.size(); i++) { py::array a = new_output(outs[i]); memcpy(a.mutable_data(), o[i].data(), outs[i].bytes); res.append(a); }
					f(res, py::none());
				} catch (py::error_already_set &e) { py::print("callback raised", e.what()); }
			};
			py::gil_scoped_release nogil;   // waits while max_inflight runs are on their way
			m.async_exec(in, done);
		}, py::arg("inputs"), py::arg("callback"))
		.def("close", &Model::close)
		.def("stop", [](Model &m) { py::gil_scoped_release nogil; m.stop(); });

	mod.def("info", [] {
		NpuInfo i; { py::gil_scoped_release nogil; i = info(); }
		py::dict d;
		d["proto"] = i.proto; d["chip"] = py::make_tuple(i.chip_model, i.chip_revision, i.chip_product); d["chip_ok"] = i.chip_ok;
		d["mem_total_kb"] = i.mem_total_kb; d["mem_avail_kb"] = i.mem_avail_kb; d["models"] = i.models; d["driver"] = i.driver; d["via"] = i.via;
		return d;
	});
	mod.def("models", [] { py::gil_scoped_release nogil; return models(); });
	mod.def("stop_id", [](uint32_t id) { py::gil_scoped_release nogil; return stop(id); });
	mod.def("stop_name", [](const std::string &w) { py::gil_scoped_release nogil; return stop(w); });
	mod.def("find_devices", [] {
		py::list l;
		for (auto &d : find_devices()) { py::dict x; x["bus"] = d.bus; x["dev"] = d.dev; x["serial"] = d.serial; x["sysfs"] = d.sysfs; l.append(x); }
		return l;
	});
	mod.def("is_quantized", [](py::bytes b) { std::string s = b; return is_quantized(std::vector<uint8_t>(s.begin(), s.end())); });
	mod.def("sha256_hex", [](py::bytes b) { std::string s = b; return sha256_hex(s.data(), s.size()); });
}
