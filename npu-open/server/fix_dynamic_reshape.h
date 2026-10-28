/* Graph-computed RESHAPE shapes -> constants.
 *
 * Keras models end with SHAPE -> STRIDED_SLICE -> PACK -> RESHAPE (the batch size is read from the tensor at run time). TensorFlow Lite knows the
 * output shape of such a RESHAPE anyway (the model stores it, with -1 only in shape_signature), but the VX delegate cannot take a shape that is
 * computed in the graph: it leaves the RESHAPE to the CPU, between two NPU parts, and the shape tensor it gets from the NPU part is broken.
 * Here the shape input of such a RESHAPE becomes a constant with the stored output shape, and the operators whose results nobody reads any more
 * (the SHAPE -> ... -> PACK chain) are removed. The model input keeps its stored shape (batch 1), so the constant is right.
 */
#ifndef NPU_FIX_DYNAMIC_RESHAPE_H
#define NPU_FIX_DYNAMIC_RESHAPE_H

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "flatbuffers/flatbuffers.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model_buffers.h"

namespace npu_reshape {

using namespace tflite;

struct Stats { int fixed = 0, removed = 0; };

static inline bool is_const(const ModelT &m, const TensorT &t)
{
	return t.buffer > 0 && t.buffer < m.buffers.size() && !m.buffers[t.buffer]->data.empty();
}

/* bytes: a TFLite model; on success replaced with the converted model. Returns "" or an error text; a model without such a RESHAPE is left as it is. */
static inline std::string run(std::vector<uint8_t> &bytes, Stats *st)
{
	flatbuffers::Verifier v(bytes.data(), bytes.size());
	if (!VerifyModelBuffer(v)) return "not a TFLite model";
	std::unique_ptr<ModelT> model = UnPackModel(bytes.data());
	if (!model || model->subgraphs.empty()) return "empty model";
	{ std::string e = npu_model::inline_buffers(*model, bytes); if (!e.empty()) return e; }
	SubGraphT &sg = *model->subgraphs[0];
	Stats s;
	for (auto &op : sg.operators) {
		const OperatorCodeT &c = *model->operator_codes[op->opcode_index];
		if (std::max<int>((int)c.builtin_code, (int)c.deprecated_builtin_code) != BuiltinOperator_RESHAPE) continue;
		if (op->inputs.size() < 2 || op->inputs[1] < 0 || op->outputs.size() != 1) continue;
		const TensorT &shape = *sg.tensors[op->inputs[1]], &y = *sg.tensors[op->outputs[0]];
		if (is_const(*model, shape) || y.shape.empty()) continue;
		bool known = true;
		for (int d : y.shape) known = known && d > 0;
		if (!known) continue;
		auto b = std::make_unique<BufferT>();
		b->data.resize(y.shape.size() * 4);
		memcpy(b->data.data(), y.shape.data(), b->data.size());
		model->buffers.push_back(std::move(b));
		auto t = std::make_unique<TensorT>();
		t->name = y.name + "_shape"; t->type = TensorType_INT32; t->shape = { (int)y.shape.size() };
		t->buffer = (uint32_t)model->buffers.size() - 1;
		sg.tensors.push_back(std::move(t));
		op->inputs[1] = (int)sg.tensors.size() - 1;
		if (auto *o = op->builtin_options.AsReshapeOptions()) o->new_shape = y.shape;
		s.fixed++;
	}
	if (!s.fixed) { if (st) *st = s; return ""; }
	/* remove operators whose outputs nobody reads (repeat: the chain dies from its end) */
	for (bool again = true; again;) {
		again = false;
		std::set<int> used(sg.outputs.begin(), sg.outputs.end());
		for (auto &op : sg.operators) used.insert(op->inputs.begin(), op->inputs.end());
		std::vector<std::unique_ptr<OperatorT>> keep;
		for (auto &op : sg.operators) {
			bool live = false;
			for (int o : op->outputs) live = live || used.count(o);
			if (live) keep.push_back(std::move(op)); else { s.removed++; again = true; }
		}
		sg.operators = std::move(keep);
	}
	flatbuffers::FlatBufferBuilder fbb(bytes.size() + 1024);
	fbb.Finish(Model::Pack(fbb, model.get()), ModelIdentifier());
	bytes.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
	if (st) *st = s;
	return "";
}

}  // namespace npu_reshape
#endif
