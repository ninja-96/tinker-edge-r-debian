/* Per-channel int8 weights -> per-tensor groups.
 *
 * The NPU runs convolutions with per-tensor weights on its fast path (MobileNet V2: 7 ms) and per-channel weights on a slow one (134 ms), but one scale for a
 * whole layer destroys the accuracy of networks whose channel scales differ by tens of times (depthwise layers of MobileNet V2: cos 0.02).
 * A middle way that stays exact: the channels of a layer are sorted by scale and grouped so that the scales inside a group differ by at most `ratio`.
 * The layer becomes K convolutions of the same input, one per group, each with per-tensor weights (the scale of the group; the channels of other groups are
 * zero, their bias is zero) and the K outputs are summed with ADD. A channel outside the group gives exactly the zero point, so the sum loses nothing,
 * and the fused ReLU / ReLU6 stays valid because 0 is inside its range. MobileNet V2 needs 2.5 groups per layer on average at ratio 4.
 * What a group costs in accuracy: its channels are requantised to the largest scale of the group, a channel whose scale is `ratio` times smaller loses up to
 * log2(ratio) bits of its weights. With max_err > 0 the ratio is chosen per layer: the largest of `ratio`, 2, 1.5, 1.25 whose weight error (relative RMS
 * per channel, against the int8 weights of the model, averaged as RMS over the channels) is at most max_err; a layer that needs less keeps its per-channel
 * weights (the slow, exact path of the NPU). One number then trades speed for accuracy over the whole network (autotune searches it).
 */
#ifndef NPU_SPLIT_PER_CHANNEL_H
#define NPU_SPLIT_PER_CHANNEL_H

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include "flatbuffers/flatbuffers.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model_buffers.h"

namespace npu_split {

using namespace tflite;

struct Stats { int layers = 0, split = 0, convs_added = 0, adds = 0, kept = 0; double max_err = 0; };

/* TensorT has no copy constructor in TensorFlow 2.6 */
static inline std::unique_ptr<TensorT> clone_tensor(const TensorT &t)
{
	auto c = std::make_unique<TensorT>();
	c->shape = t.shape; c->type = t.type; c->buffer = t.buffer; c->name = t.name; c->is_variable = t.is_variable; c->shape_signature = t.shape_signature;
	if (t.quantization) {
		auto q = std::make_unique<QuantizationParametersT>();
		q->min = t.quantization->min; q->max = t.quantization->max; q->scale = t.quantization->scale; q->zero_point = t.quantization->zero_point;
		q->quantized_dimension = t.quantization->quantized_dimension;
		c->quantization = std::move(q);
	}
	return c;
}

static inline BuiltinOperator code_of(const ModelT &m, const OperatorT &op)
{
	const OperatorCodeT &c = *m.operator_codes[op.opcode_index];
	return (BuiltinOperator)std::max<int>((int)c.builtin_code, (int)c.deprecated_builtin_code);
}

static inline bool fused_act(const OperatorT &op, BuiltinOperator code, ActivationFunctionType *act)
{
	switch (code) {
	case BuiltinOperator_CONV_2D: if (auto o = op.builtin_options.AsConv2DOptions()) { *act = o->fused_activation_function; return true; } break;
	case BuiltinOperator_DEPTHWISE_CONV_2D: if (auto o = op.builtin_options.AsDepthwiseConv2DOptions()) { *act = o->fused_activation_function; return true; } break;
	case BuiltinOperator_FULLY_CONNECTED: if (auto o = op.builtin_options.AsFullyConnectedOptions()) { *act = o->fused_activation_function; return true; } break;
	default: break;
	}
	return false;
}

/* bytes: a TFLite model; on success replaced with the converted model. Returns "" or an error text; a model without per-channel layers is left as it is. */
static inline std::string run(std::vector<uint8_t> &bytes, double ratio, Stats *st, double max_err = 0)
{
	flatbuffers::Verifier v(bytes.data(), bytes.size());
	if (!VerifyModelBuffer(v)) return "not a TFLite model";
	std::unique_ptr<ModelT> model = UnPackModel(bytes.data());
	if (!model || model->subgraphs.empty()) return "empty model";
	{ std::string e = npu_model::inline_buffers(*model, bytes); if (!e.empty()) return e; }
	SubGraphT &sg = *model->subgraphs[0];

	int add_code = -1;
	auto add_opcode = [&]() {
		if (add_code >= 0) return add_code;
		for (size_t i = 0; i < model->operator_codes.size(); i++)
			if (std::max<int>(model->operator_codes[i]->builtin_code, model->operator_codes[i]->deprecated_builtin_code) == BuiltinOperator_ADD) return add_code = (int)i;
		auto c = std::make_unique<OperatorCodeT>();
		c->builtin_code = BuiltinOperator_ADD; c->deprecated_builtin_code = (int8_t)BuiltinOperator_ADD; c->version = 2;
		model->operator_codes.push_back(std::move(c));
		return add_code = (int)model->operator_codes.size() - 1;
	};
	auto new_tensor = [&](const TensorT &like, const std::string &suffix) {
		auto t = clone_tensor(like);
		t->name = like.name + suffix; t->buffer = 0;
		sg.tensors.push_back(std::move(t));
		return (int)sg.tensors.size() - 1;
	};
	auto new_buffer = [&](std::vector<uint8_t> &&data) {
		auto b = std::make_unique<BufferT>(); b->data = std::move(data);
		model->buffers.push_back(std::move(b));
		return (uint32_t)model->buffers.size() - 1;
	};

	std::vector<std::unique_ptr<OperatorT>> out_ops;
	Stats s;
	bool changed = false;
	for (auto &op : sg.operators) {
		BuiltinOperator code = code_of(*model, *op);
		ActivationFunctionType act;
		bool ok = (code == BuiltinOperator_CONV_2D || code == BuiltinOperator_DEPTHWISE_CONV_2D || code == BuiltinOperator_FULLY_CONNECTED) &&
			  op->inputs.size() >= 3 && op->inputs[1] >= 0 && op->inputs[2] >= 0 && op->outputs.size() == 1 && fused_act(*op, code, &act) &&
			  (act == ActivationFunctionType_NONE || act == ActivationFunctionType_RELU || act == ActivationFunctionType_RELU6 || act == ActivationFunctionType_RELU_N1_TO_1);
		if (ok) {
			const TensorT &w = *sg.tensors[op->inputs[1]], &b = *sg.tensors[op->inputs[2]], &x = *sg.tensors[op->inputs[0]];
			ok = w.type == TensorType_INT8 && w.quantization && w.quantization->scale.size() > 1 && b.type == TensorType_INT32 && b.quantization &&
			     b.quantization->scale.size() == w.quantization->scale.size() && x.quantization && !x.quantization->scale.empty() &&
			     w.buffer < model->buffers.size() && b.buffer < model->buffers.size();
		}
		if (!ok) { out_ops.push_back(std::move(op)); continue; }

		const TensorT &w = *sg.tensors[op->inputs[1]], &b = *sg.tensors[op->inputs[2]], &x = *sg.tensors[op->inputs[0]];
		const std::vector<float> sc = w.quantization->scale, bsc = b.quantization->scale;
		const size_t n = sc.size();
		const int qd = w.quantization->quantized_dimension;
		const std::vector<uint8_t> wdata = model->buffers[w.buffer]->data, bdata = model->buffers[b.buffer]->data;
		size_t total = 1, inner = 1;
		for (size_t i = 0; i < w.shape.size(); i++) total *= w.shape[i];
		for (size_t i = qd + 1; i < w.shape.size(); i++) inner *= w.shape[i];
		if (qd < 0 || (size_t)qd >= w.shape.size() || (size_t)w.shape[qd] != n || wdata.size() != total || bdata.size() != n * 4) { out_ops.push_back(std::move(op)); continue; }
		const float xs = x.quantization->scale[0];

		/* groups: sorted by scale, a group spans at most `r` */
		std::vector<size_t> order(n);
		std::iota(order.begin(), order.end(), 0);
		std::sort(order.begin(), order.end(), [&](size_t a, size_t c) { return sc[a] < sc[c]; });
		std::vector<int> group(n); std::vector<float> gmax;
		auto make_groups = [&](double r) {
			gmax.clear();
			for (size_t i = 0; i < n;) {
				double top = std::max<double>(sc[order[i]], 1e-12) * r; size_t j = i; float mx = 0;
				while (j < n && sc[order[j]] <= top) { group[order[j]] = (int)gmax.size(); mx = std::max(mx, sc[order[j]]); j++; }
				gmax.push_back(mx); i = j;
			}
		};
		auto weight_error = [&]() {   /* RMS over the channels of the relative RMS error of each channel after the requantisation */
			std::vector<double> e2(n, 0), w2(n, 0);
			for (size_t i = 0; i < total; i++) {
				size_t c = (i / inner) % n;
				double wv = (int8_t)wdata[i] * (double)sc[c], smax = std::max(gmax[group[c]], 1e-30f);
				double q = std::max(-127.0, std::min(127.0, std::round(wv / smax)));
				e2[c] += (q * smax - wv) * (q * smax - wv); w2[c] += wv * wv;
			}
			double acc = 0; int m = 0;
			for (size_t c = 0; c < n; c++) if (w2[c] > 0) { acc += e2[c] / w2[c]; m++; }
			return m ? std::sqrt(acc / m) : 0.0;
		};
		if (max_err > 0) {
			bool found = false;
			for (double r : { ratio, 2.0, 1.5, 1.25 }) {
				if (r > ratio) continue;
				make_groups(r);
				double e = weight_error();
				if (e <= max_err) { found = true; s.max_err = std::max(s.max_err, e); break; }
			}
			if (!found) { s.layers++; s.kept++; out_ops.push_back(std::move(op)); continue; }   /* exact only per channel */
		} else make_groups(ratio);
		const int K = (int)gmax.size();
		s.layers++; if (K > 1) { s.split++; s.convs_added += K - 1; s.adds += K - 1; }
		changed = true;

		const int in_x = op->inputs[0], out_y = op->outputs[0];
		const TensorT &y = *sg.tensors[out_y];
		std::vector<int> partial;
		for (int g = 0; g < K; g++) {
			std::vector<uint8_t> wd(total); std::vector<uint8_t> bd(n * 4);
			const float smax = std::max(gmax[g], 1e-30f), bs = xs * smax;
			for (size_t i = 0; i < total; i++) {
				size_t c = (i / inner) % n;
				double q = group[c] == g ? std::round((int8_t)wdata[i] * (double)sc[c] / smax) : 0;
				wd[i] = (uint8_t)(int8_t)std::max(-127.0, std::min(127.0, q));
			}
			for (size_t c = 0; c < n; c++) {
				int32_t bi; memcpy(&bi, &bdata[c * 4], 4);
				int32_t nb = group[c] == g ? (int32_t)std::llround(bi * (double)bsc[c] / bs) : 0;
				memcpy(&bd[c * 4], &nb, 4);
			}
			int wi = new_tensor(w, "_g" + std::to_string(g)), bi = new_tensor(b, "_g" + std::to_string(g));
			sg.tensors[wi]->buffer = new_buffer(std::move(wd)); sg.tensors[bi]->buffer = new_buffer(std::move(bd));
			sg.tensors[wi]->quantization->scale = { smax }; sg.tensors[wi]->quantization->zero_point = { 0 };
			sg.tensors[bi]->quantization->scale = { bs }; sg.tensors[bi]->quantization->zero_point = { 0 };
			int oi = (K == 1) ? out_y : new_tensor(y, "_p" + std::to_string(g));
			auto c = std::make_unique<OperatorT>();
			c->opcode_index = op->opcode_index; c->builtin_options = op->builtin_options; c->custom_options = op->custom_options;
			c->inputs = { in_x, wi, bi }; c->outputs = { oi };
			out_ops.push_back(std::move(c)); partial.push_back(oi);
		}
		if (K > 1) {
			int acc = partial[0];
			for (int g = 1; g < K; g++) {
				int oi = (g == K - 1) ? out_y : new_tensor(y, "_s" + std::to_string(g));
				auto a = std::make_unique<OperatorT>();
				a->opcode_index = add_opcode();
				a->builtin_options.type = BuiltinOptions_AddOptions;
				auto *ao = new AddOptionsT(); ao->fused_activation_function = ActivationFunctionType_NONE; a->builtin_options.value = ao;
				a->inputs = { acc, partial[g] }; a->outputs = { oi };
				out_ops.push_back(std::move(a)); acc = oi;
			}
		}
	}
	if (!changed) { if (st) *st = s; return ""; }
	sg.operators = std::move(out_ops);
	flatbuffers::FlatBufferBuilder fbb(bytes.size() * 2);
	fbb.Finish(Model::Pack(fbb, model.get()), ModelIdentifier());
	bytes.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
	if (st) *st = s;
	return "";
}

}  // namespace npu_split
#endif
