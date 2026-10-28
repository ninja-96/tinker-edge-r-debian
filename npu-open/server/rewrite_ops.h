/* Operators the VX delegate does not take, rewritten into ones it does, before the graph is built (so that they run on the NPU instead of the CPU
 * of the NPU die, and the graph is not cut):
 *   SLICE(x, begin, size)      -> STRIDED_SLICE(x, begin, begin + size, strides 1)   (size -1: to the end, through end_mask)
 *   TILE(x, multiples)         -> CONCATENATION of `m` copies along every axis with a multiple m > 1
 *   LOG_SOFTMAX(x)             -> SOFTMAX(x, beta 1) -> LOG
 * Only with constant begin / size / multiples and known shapes; anything else is left as it is. The data type does not matter (float, int8, uint8:
 * the intermediate tensors take the quantisation of their input, the result keeps the tensor of the original output).
 */
#ifndef NPU_REWRITE_OPS_H
#define NPU_REWRITE_OPS_H

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "flatbuffers/flatbuffers.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace npu_rewrite {

using namespace tflite;

struct Stats { int slice = 0, tile = 0, log_softmax = 0; };

static inline BuiltinOperator code_of(const ModelT &m, const OperatorT &op)
{
	const OperatorCodeT &c = *m.operator_codes[op.opcode_index];
	return (BuiltinOperator)std::max<int>((int)c.builtin_code, (int)c.deprecated_builtin_code);
}

/* bytes: a TFLite model; on success replaced with the rewritten model. Returns "" or an error text; a model without such operators is left as it is. */
static inline std::string run(std::vector<uint8_t> &bytes, Stats *st)
{
	flatbuffers::Verifier v(bytes.data(), bytes.size());
	if (!VerifyModelBuffer(v)) return "not a TFLite model";
	std::unique_ptr<ModelT> model = UnPackModel(bytes.data());
	if (!model || model->subgraphs.empty()) return "empty model";
	SubGraphT &sg = *model->subgraphs[0];
	Stats s;

	auto opcode = [&](BuiltinOperator c, int version) {
		for (size_t i = 0; i < model->operator_codes.size(); i++)
			if (std::max<int>(model->operator_codes[i]->builtin_code, model->operator_codes[i]->deprecated_builtin_code) == c) return (int)i;
		auto oc = std::make_unique<OperatorCodeT>();
		oc->builtin_code = c; oc->deprecated_builtin_code = (int8_t)std::min<int>(c, 127); oc->version = version;
		model->operator_codes.push_back(std::move(oc));
		return (int)model->operator_codes.size() - 1;
	};
	auto const_i32 = [&](const TensorT &t, std::vector<int32_t> *out) {   /* the values of a constant int32 / int64 tensor */
		if (t.buffer == 0 || t.buffer >= model->buffers.size()) return false;
		const auto &d = model->buffers[t.buffer]->data;
		if (d.empty()) return false;
		out->clear();
		if (t.type == TensorType_INT32) { for (size_t i = 0; i + 4 <= d.size(); i += 4) { int32_t x; memcpy(&x, &d[i], 4); out->push_back(x); } return true; }
		if (t.type == TensorType_INT64) { for (size_t i = 0; i + 8 <= d.size(); i += 8) { int64_t x; memcpy(&x, &d[i], 8); out->push_back((int32_t)x); } return true; }
		return false;
	};
	auto new_const_i32 = [&](const std::vector<int32_t> &vals, const std::string &name) {
		auto b = std::make_unique<BufferT>();
		b->data.resize(vals.size() * 4); memcpy(b->data.data(), vals.data(), b->data.size());
		model->buffers.push_back(std::move(b));
		auto t = std::make_unique<TensorT>();
		t->name = name; t->type = TensorType_INT32; t->shape = { (int)vals.size() }; t->buffer = (uint32_t)model->buffers.size() - 1;
		sg.tensors.push_back(std::move(t));
		return (int)sg.tensors.size() - 1;
	};
	auto new_like = [&](const TensorT &like, const std::vector<int> &shape, const std::string &name) {   /* an activation tensor with the type and quantisation of `like` */
		auto t = std::make_unique<TensorT>();
		t->name = name; t->type = like.type; t->shape = shape; t->buffer = 0;
		if (like.quantization) {
			auto q = std::make_unique<QuantizationParametersT>();
			q->scale = like.quantization->scale; q->zero_point = like.quantization->zero_point; q->quantized_dimension = like.quantization->quantized_dimension;
			q->min = like.quantization->min; q->max = like.quantization->max;
			t->quantization = std::move(q);
		}
		sg.tensors.push_back(std::move(t));
		return (int)sg.tensors.size() - 1;
	};
	auto known = [](const std::vector<int> &sh) { if (sh.empty()) return false; for (int d : sh) if (d <= 0) return false; return true; };

	std::vector<std::unique_ptr<OperatorT>> out;
	bool changed = false;
	for (auto &op : sg.operators) {
		BuiltinOperator c = code_of(*model, *op);
		if (c == BuiltinOperator_SLICE && op->inputs.size() == 3 && op->outputs.size() == 1) {
			const TensorT &x = *sg.tensors[op->inputs[0]];
			std::vector<int32_t> begin, size;
			if (known(x.shape) && const_i32(*sg.tensors[op->inputs[1]], &begin) && const_i32(*sg.tensors[op->inputs[2]], &size) &&
			    begin.size() == x.shape.size() && size.size() == x.shape.size()) {
				std::vector<int32_t> end(begin.size()), strides(begin.size(), 1);
				int end_mask = 0;
				for (size_t i = 0; i < begin.size(); i++) {
					if (size[i] < 0) { end_mask |= 1 << i; end[i] = x.shape[i]; } else end[i] = begin[i] + size[i];
				}
				auto n = std::make_unique<OperatorT>();
				n->opcode_index = opcode(BuiltinOperator_STRIDED_SLICE, 1);
				n->inputs = { op->inputs[0], new_const_i32(begin, x.name + "_ss_begin"), new_const_i32(end, x.name + "_ss_end"), new_const_i32(strides, x.name + "_ss_strides") };
				n->outputs = op->outputs;
				n->builtin_options.type = BuiltinOptions_StridedSliceOptions;
				auto *o = new StridedSliceOptionsT(); o->end_mask = end_mask; n->builtin_options.value = o;
				out.push_back(std::move(n)); s.slice++; changed = true; continue;
			}
		}
		if (c == BuiltinOperator_TILE && op->inputs.size() == 2 && op->outputs.size() == 1) {
			const TensorT &x = *sg.tensors[op->inputs[0]];
			std::vector<int32_t> mult;
			if (known(x.shape) && const_i32(*sg.tensors[op->inputs[1]], &mult) && mult.size() == x.shape.size()) {
				bool ok = true;
				for (int32_t m : mult) ok = ok && m >= 1 && m <= 64;
				if (ok) {
					int cur = op->inputs[0]; std::vector<int> shape = x.shape;
					std::vector<int> axes;
					for (size_t i = 0; i < mult.size(); i++) if (mult[i] > 1) axes.push_back((int)i);
					for (size_t k = 0; k < axes.size(); k++) {
						int a = axes[k];
						shape[a] *= mult[a];
						int dst = (k + 1 == axes.size()) ? op->outputs[0] : new_like(x, shape, x.name + "_tile" + std::to_string(a));
						auto n = std::make_unique<OperatorT>();
						n->opcode_index = opcode(BuiltinOperator_CONCATENATION, 1);
						n->inputs.assign(mult[a], cur);
						n->outputs = { dst };
						n->builtin_options.type = BuiltinOptions_ConcatenationOptions;
						auto *o = new ConcatenationOptionsT(); o->axis = a; n->builtin_options.value = o;
						out.push_back(std::move(n));
						cur = dst;
					}
					if (axes.empty()) {   /* all multiples 1: a copy */
						auto n = std::make_unique<OperatorT>();
						n->opcode_index = opcode(BuiltinOperator_CONCATENATION, 1);
						n->inputs = { op->inputs[0] }; n->outputs = op->outputs;
						n->builtin_options.type = BuiltinOptions_ConcatenationOptions;
						auto *o = new ConcatenationOptionsT(); o->axis = 0; n->builtin_options.value = o;
						out.push_back(std::move(n));
					}
					s.tile++; changed = true; continue;
				}
			}
		}
		if (c == BuiltinOperator_LOG_SOFTMAX && op->inputs.size() == 1 && op->outputs.size() == 1) {
			const TensorT &x = *sg.tensors[op->inputs[0]], &y = *sg.tensors[op->outputs[0]];
			if (known(x.shape)) {
				int mid = new_like(x, x.shape, x.name + "_softmax");
				TensorT &m = *sg.tensors[mid];
				if (m.quantization && (m.type == TensorType_INT8 || m.type == TensorType_UINT8)) {   /* the usual quantisation of a softmax output */
					m.quantization->scale = { 1.0f / 256 };
					m.quantization->zero_point = { m.type == TensorType_INT8 ? -128 : 0 };
					m.quantization->min.clear(); m.quantization->max.clear();
				}
				(void)y;
				auto a = std::make_unique<OperatorT>();
				a->opcode_index = opcode(BuiltinOperator_SOFTMAX, 1);
				a->inputs = { op->inputs[0] }; a->outputs = { mid };
				a->builtin_options.type = BuiltinOptions_SoftmaxOptions;
				auto *o = new SoftmaxOptionsT(); o->beta = 1.0f; a->builtin_options.value = o;
				out.push_back(std::move(a));
				auto b = std::make_unique<OperatorT>();
				b->opcode_index = opcode(BuiltinOperator_LOG, 1);
				b->inputs = { mid }; b->outputs = op->outputs;
				out.push_back(std::move(b));
				s.log_softmax++; changed = true; continue;
			}
		}
		out.push_back(std::move(op));
	}
	if (!changed) { if (st) *st = s; return ""; }
	sg.operators = std::move(out);
	flatbuffers::FlatBufferBuilder fbb(bytes.size() + 4096);
	fbb.Finish(Model::Pack(fbb, model.get()), ModelIdentifier());
	bytes.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
	if (st) *st = s;
	return "";
}

}  // namespace npu_rewrite
#endif
