/* Operators the VX delegate does not take, rewritten into ones it does, before the graph is built (so that they run on the NPU instead of the CPU
 * of the NPU die, and the graph is not cut):
 *   SLICE(x, begin, size)      -> STRIDED_SLICE(x, begin, begin + size, strides 1)   (size -1: to the end, through end_mask)
 *   TILE(x, multiples)         -> CONCATENATION of `m` copies along every axis with a multiple m > 1
 *   LOG_SOFTMAX(x)             -> SOFTMAX(x, beta 1) -> LOG
 * Only with constant begin / size / multiples and known shapes; anything else is left as it is. The data type does not matter (float, int8, uint8:
 * the intermediate tensors take the quantisation of their input, the result keeps the tensor of the original output).
 * Comparisons, logical operators, SELECT, CAST and CEIL: see rewrite_bool below.
 */
#ifndef NPU_REWRITE_OPS_H
#define NPU_REWRITE_OPS_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "flatbuffers/flatbuffers.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace npu_rewrite {

using namespace tflite;

struct Stats { int slice = 0, tile = 0, log_softmax = 0, boolean = 0, trunc = 0, ceil = 0, cos = 0, round = 0, prelu = 0; };

static inline BuiltinOperator code_of(const ModelT &m, const OperatorT &op)
{
	const OperatorCodeT &c = *m.operator_codes[op.opcode_index];
	return (BuiltinOperator)std::max<int>((int)c.builtin_code, (int)c.deprecated_builtin_code);
}

/* ---- comparisons, logical operators, SELECT and CAST without bool tensors ----
 * TIM-VX crashes on bool tensors with the driver of this NPU. A bool tensor becomes a tensor of the type of the compared values (float32, or uint8 /
 * int8 quantised to [0, 1]) holding 0 or 1, and the operators are made of arithmetic the NPU runs:
 *   GREATER(a, b) = float: MINIMUM(MAXIMUM((a - b) * K, 0), 1), K = 60000 (fits float16, the NPU's float type)
 *                   int8 / uint8: MUL(SUB(a, b), K) into a [0, 1] output: the saturation of the requantisation gives exactly 0 or 1
 *   LESS(a, b) = GREATER(b, a); GREATER_EQUAL(a, b) = 1 - GREATER(b, a); LESS_EQUAL(a, b) = 1 - GREATER(a, b)
 *   NOT_EQUAL = GREATER(a, b) + GREATER(b, a); EQUAL = 1 - NOT_EQUAL
 *   LOGICAL_AND = MUL, LOGICAL_OR = MAXIMUM, LOGICAL_NOT = 1 - x; SELECT(c, a, b) = b + c * (a - b), c first brought to the type of a, b
 *   (DEQUANTIZE / QUANTIZE) when they differ
 *   CAST(bool -> float32): float: RESHAPE (a copy); quantised: DEQUANTIZE; a constant bool input becomes a constant 0 / 1
 * and operators without bool:
 *   CAST(CAST(x float32 -> int32 / int64) -> float32), truncation toward 0 = MAXIMUM(FLOOR(x), 0) + MINIMUM(NEG(FLOOR(NEG(x))), 0)
 *   CEIL(x) = NEG(FLOOR(NEG(x))); COS(x) = SIN(x + pi / 2)
 *   ROUND(x) (half to even, as TFLite) = r - tie * (r - 2 * FLOOR(r / 2)), r = FLOOR(x + 0.5), tie = MAXIMUM(1 - (0.5 - (r - x)) * K, 0): 1 when x + 0.5 is whole
 *   CEIL, COS, ROUND of int8 / uint8: DEQUANTIZE, the float formula, QUANTIZE into the original output
 * Only a group whose every bool tensor is made and used by such operators (and is no model output) is rewritten. In float, |a - b| below
 * 1 / K (or below the float16 resolution) counts as equal. */
struct QRange { float lo, hi; };
static inline bool qrange(const TensorT &t, QRange *r)
{
	if (!t.quantization || t.quantization->scale.size() != 1) return false;
	float s = t.quantization->scale[0]; int zp = t.quantization->zero_point.empty() ? 0 : (int)t.quantization->zero_point[0];
	int lo = t.type == TensorType_INT8 ? -128 : 0, hi = t.type == TensorType_INT8 ? 127 : 255;
	r->lo = (lo - zp) * s; r->hi = (hi - zp) * s;
	return true;
}

static inline bool rewrite_bool(ModelT &model, Stats &s)
{
	SubGraphT &sg = *model.subgraphs[0];
	auto is_cmp = [](BuiltinOperator c) { return c == BuiltinOperator_GREATER || c == BuiltinOperator_GREATER_EQUAL || c == BuiltinOperator_LESS ||
						      c == BuiltinOperator_LESS_EQUAL || c == BuiltinOperator_EQUAL || c == BuiltinOperator_NOT_EQUAL; };
	auto is_logic = [](BuiltinOperator c) { return c == BuiltinOperator_LOGICAL_AND || c == BuiltinOperator_LOGICAL_OR || c == BuiltinOperator_LOGICAL_NOT; };
	auto numeric = [](TensorType t) { return t == TensorType_FLOAT32 || t == TensorType_UINT8 || t == TensorType_INT8; };
	auto const_bool = [&](int t) { const TensorT &x = *sg.tensors[t]; return x.type == TensorType_BOOL && x.buffer > 0 && x.buffer < model.buffers.size() && !model.buffers[x.buffer]->data.empty(); };
	const size_t nt = sg.tensors.size(), nops = sg.operators.size();
	std::vector<int> repr(nt, -1);   /* bool tensor -> the type that represents it (TensorType), -1: not rewritten */
	auto same_quant = [&](int a, int b) {   /* two int8/uint8 tensors of the same type, per-tensor quantised */
		QRange x, y; return sg.tensors[a]->type == sg.tensors[b]->type && qrange(*sg.tensors[a], &x) && qrange(*sg.tensors[b], &y); };
	/* 1. candidates, in graph order */
	for (auto &op : sg.operators) {
		BuiltinOperator c = code_of(model, *op);
		if (op->outputs.size() != 1 || sg.tensors[op->outputs[0]]->type != TensorType_BOOL) continue;
		int out = op->outputs[0];
		if (is_cmp(c) && op->inputs.size() == 2) {
			const TensorT &a = *sg.tensors[op->inputs[0]], &b = *sg.tensors[op->inputs[1]];
			if (a.type == b.type && numeric(a.type) && (a.type == TensorType_FLOAT32 || same_quant(op->inputs[0], op->inputs[1]))) repr[out] = a.type;
		} else if (is_logic(c)) {
			int r = -2;
			for (int in : op->inputs) { if (in >= 0 && const_bool(in)) continue; if (in < 0 || repr[in] < 0 || (r != -2 && repr[in] != r)) { r = -1; break; } r = repr[in]; }
			if (r < 0) continue;
			repr[out] = r;
			for (int in : op->inputs) if (const_bool(in)) repr[in] = r;
		}
	}
	/* 2. every use of a candidate must be rewritable, and it must not be a model output; repeat until nothing changes */
	for (bool again = true; again;) {
		again = false;
		for (size_t t = 0; t < nt; t++) {
			if (repr[t] < 0) continue;
			bool ok = std::find(sg.outputs.begin(), sg.outputs.end(), (int)t) == sg.outputs.end();
			for (auto &op : sg.operators) {
				for (size_t k = 0; ok && k < op->inputs.size(); k++) {
					if (op->inputs[k] != (int)t) continue;
					BuiltinOperator c = code_of(model, *op);
					if (is_logic(c)) ok = op->outputs.size() == 1 && repr[op->outputs[0]] == repr[t];
					else if ((c == BuiltinOperator_SELECT || c == BuiltinOperator_SELECT_V2) && k == 0 && op->inputs.size() == 3) {
						const TensorT &a = *sg.tensors[op->inputs[1]], &b = *sg.tensors[op->inputs[2]], &y = *sg.tensors[op->outputs[0]];
						ok = a.type == b.type && a.type == y.type && numeric(a.type) && (a.type == TensorType_FLOAT32 || (same_quant(op->inputs[1], op->inputs[2]) && same_quant(op->inputs[1], op->outputs[0])));
					} else if (c == BuiltinOperator_CAST) ok = sg.tensors[op->outputs[0]]->type == TensorType_FLOAT32;
					else ok = false;
				}
			}
			if (!ok) { repr[t] = -1; again = true; }
		}
		/* a logical operator needs all its inputs rewritten */
		for (auto &op : sg.operators) {
			BuiltinOperator c = code_of(model, *op);
			if (!is_logic(c) || op->outputs.size() != 1 || repr[op->outputs[0]] < 0) continue;
			for (int in : op->inputs) if (in < 0 || repr[in] < 0) { repr[op->outputs[0]] = -1; again = true; break; }
		}
	}
	/* truncation (a CAST pair through an integer used nowhere else, float32); CEIL, COS, ROUND */
	std::vector<int> uses(nt, 0), trunc_of(nops, -1);   /* trunc_of[second CAST] = x */
	std::vector<char> drop(nops, 0);
	std::vector<int> unary(nops, 0);   /* CEIL, COS, ROUND: their operator code */
	for (auto &op : sg.operators) for (int in : op->inputs) if (in >= 0) uses[in]++;
	for (int t : sg.outputs) uses[t]++;
	for (size_t i = 0; i < nops; i++) {
		const OperatorT &op = *sg.operators[i];
		BuiltinOperator c = code_of(model, op);
		if (op.inputs.size() != 1 || op.outputs.size() != 1) continue;
		const TensorT &x = *sg.tensors[op.inputs[0]], &y = *sg.tensors[op.outputs[0]];
		if ((c == BuiltinOperator_CEIL || c == BuiltinOperator_COS || c == BuiltinOperator_ROUND) && x.type == y.type &&
		    (x.type == TensorType_FLOAT32 || ((x.type == TensorType_INT8 || x.type == TensorType_UINT8) && same_quant(op.inputs[0], op.outputs[0])))) unary[i] = c;
		if (c != BuiltinOperator_CAST || x.type != TensorType_FLOAT32 || (y.type != TensorType_INT32 && y.type != TensorType_INT64) || uses[op.outputs[0]] != 1) continue;
		for (size_t j = i + 1; j < nops; j++) {
			const OperatorT &o2 = *sg.operators[j];
			if (o2.inputs.size() != 1 || o2.inputs[0] != op.outputs[0]) continue;
			if (code_of(model, o2) == BuiltinOperator_CAST && o2.outputs.size() == 1 && sg.tensors[o2.outputs[0]]->type == TensorType_FLOAT32) { drop[i] = 1; trunc_of[j] = op.inputs[0]; }
			break;
		}
	}
	bool any = false;
	for (size_t t = 0; t < nt; t++) any = any || repr[t] >= 0;
	for (size_t i = 0; i < nops; i++) any = any || drop[i] || unary[i];
	if (!any) return false;

	/* helpers to build the replacement */
	auto opcode = [&](BuiltinOperator c, int version) {
		for (size_t i = 0; i < model.operator_codes.size(); i++)
			if (std::max<int>(model.operator_codes[i]->builtin_code, model.operator_codes[i]->deprecated_builtin_code) == c) return (int)i;
		auto oc = std::make_unique<OperatorCodeT>();
		oc->builtin_code = c; oc->deprecated_builtin_code = (int8_t)std::min<int>(c, 127); oc->version = version;
		model.operator_codes.push_back(std::move(oc));
		return (int)model.operator_codes.size() - 1;
	};
	auto quant = [](TensorType t, float lo, float hi, std::unique_ptr<QuantizationParametersT> *q) {   /* per-tensor parameters covering [lo, hi] */
		lo = std::min(lo, 0.0f); hi = std::max(hi, 0.0f);
		float scale = (hi - lo) / 255.0f; if (scale <= 0) scale = 1.0f / 255;
		int qmin = t == TensorType_INT8 ? -128 : 0;
		int zp = qmin - (int)std::lround(lo / scale);
		*q = std::make_unique<QuantizationParametersT>();
		(*q)->scale = { scale }; (*q)->zero_point = { zp };
	};
	auto tensor = [&](TensorType type, const std::vector<int> &shape, float lo, float hi, const std::string &name) {
		auto t = std::make_unique<TensorT>();
		t->name = name; t->type = type; t->shape = shape; t->buffer = 0;
		if (type != TensorType_FLOAT32) quant(type, lo, hi, &t->quantization);
		sg.tensors.push_back(std::move(t));
		return (int)sg.tensors.size() - 1;
	};
	auto constant = [&](TensorType type, float value, const std::string &name) {   /* a [1] constant; quantised: the value sits at the end of the range */
		auto b = std::make_unique<BufferT>();
		auto t = std::make_unique<TensorT>();
		t->name = name; t->type = type; t->shape = { 1 };
		if (type == TensorType_FLOAT32) { b->data.resize(4); memcpy(b->data.data(), &value, 4); }
		else {
			int top = type == TensorType_INT8 ? 127 : 255;
			auto q = std::make_unique<QuantizationParametersT>(); q->scale = { value / top }; q->zero_point = { 0 };
			t->quantization = std::move(q);
			b->data = { (uint8_t)top };
		}
		model.buffers.push_back(std::move(b));
		t->buffer = (uint32_t)model.buffers.size() - 1;
		sg.tensors.push_back(std::move(t));
		return (int)sg.tensors.size() - 1;
	};
	auto set01 = [&](int t, TensorType type) {   /* a former bool tensor: type `type`, values 0 / 1 */
		TensorT &x = *sg.tensors[t];
		x.type = type; x.quantization.reset();
		if (type != TensorType_FLOAT32) {
			auto q = std::make_unique<QuantizationParametersT>(); q->scale = { 1.0f / 255 }; q->zero_point = { type == TensorType_INT8 ? -128 : 0 };
			x.quantization = std::move(q);
		}
	};
	std::vector<std::unique_ptr<OperatorT>> out;
	auto emit = [&](BuiltinOperator c, std::vector<int> in, int o) {
		auto n = std::make_unique<OperatorT>();
		n->opcode_index = opcode(c, 1); n->inputs = std::move(in); n->outputs = { o };
		if (c == BuiltinOperator_SUB) { n->builtin_options.type = BuiltinOptions_SubOptions; n->builtin_options.value = new SubOptionsT(); }
		if (c == BuiltinOperator_ADD) { n->builtin_options.type = BuiltinOptions_AddOptions; n->builtin_options.value = new AddOptionsT(); }
		if (c == BuiltinOperator_MUL) { n->builtin_options.type = BuiltinOptions_MulOptions; n->builtin_options.value = new MulOptionsT(); }
		if (c == BuiltinOperator_MAXIMUM || c == BuiltinOperator_MINIMUM) { n->builtin_options.type = BuiltinOptions_MaximumMinimumOptions; n->builtin_options.value = new MaximumMinimumOptionsT(); }
		if (c == BuiltinOperator_RESHAPE) { n->builtin_options.type = BuiltinOptions_ReshapeOptions; n->builtin_options.value = new ReshapeOptionsT(); }
		if (c == BuiltinOperator_NEG) { n->builtin_options.type = BuiltinOptions_NegOptions; n->builtin_options.value = new NegOptionsT(); }
		out.push_back(std::move(n));
	};
	int counter = 0;
	auto nm = [&](const char *what) { return std::string("npu_bool_") + what + "_" + std::to_string(counter++); };
	/* GREATER(a, b) into tensor `o` (already 0/1 of type `type`) */
	auto greater = [&](int a, int b, int o, TensorType type, const std::vector<int> &shape) {
		if (type == TensorType_FLOAT32) {
			int d = tensor(type, shape, 0, 0, nm("diff")), m = tensor(type, shape, 0, 0, nm("scaled")), x = tensor(type, shape, 0, 0, nm("pos"));
			emit(BuiltinOperator_SUB, { a, b }, d);
			emit(BuiltinOperator_MUL, { d, constant(type, 60000.0f, nm("k")) }, m);
			emit(BuiltinOperator_MAXIMUM, { m, constant(type, 0.0f, nm("zero")) }, x);
			emit(BuiltinOperator_MINIMUM, { x, constant(type, 1.0f, nm("one")) }, o);
		} else {
			/* only the sign of a - b matters: its step is 1/16 of the finer input step, so that close values still differ; large differences saturate */
			float step = std::min(sg.tensors[a]->quantization->scale[0], sg.tensors[b]->quantization->scale[0]) / 16;
			int d = tensor(type, shape, 0, 0, nm("diff"));
			sg.tensors[d]->quantization->scale = { step }; sg.tensors[d]->quantization->zero_point = { type == TensorType_INT8 ? 0 : 128 };
			emit(BuiltinOperator_SUB, { a, b }, d);
			/* one step of the difference gives 1 (k = 1 / step keeps the requantisation multiplier at 2: stock TFLite's NEON MUL wraps around at 4, ours is patched) */
			emit(BuiltinOperator_MUL, { d, constant(type, 1.0f / step, nm("k")) }, o);
		}
	};
	auto one_minus = [&](int x, int o, TensorType type) { emit(BuiltinOperator_SUB, { constant(type, 1.0f, nm("one")), x }, o); };
	auto ceil = [&](int x, int o, const std::vector<int> &shape) {   /* float32 */
		int n = tensor(TensorType_FLOAT32, shape, 0, 0, nm("neg")), f = tensor(TensorType_FLOAT32, shape, 0, 0, nm("floor"));
		emit(BuiltinOperator_NEG, { x }, n); emit(BuiltinOperator_FLOOR, { n }, f); emit(BuiltinOperator_NEG, { f }, o);
	};
	/* constant bool tensors -> constants 0 / 1 of the representing type (a new buffer: the old one may be shared) */
	for (size_t t = 0; t < nt; t++) {
		if (repr[t] < 0 || !const_bool((int)t)) continue;
		TensorType type = (TensorType)repr[t];
		const std::vector<uint8_t> &src = model.buffers[sg.tensors[t]->buffer]->data;
		auto b = std::make_unique<BufferT>();
		for (uint8_t v : src) {
			if (type == TensorType_FLOAT32) { float f = v ? 1.0f : 0.0f; const uint8_t *p = (const uint8_t *)&f; b->data.insert(b->data.end(), p, p + 4); }
			else b->data.push_back(type == TensorType_INT8 ? (uint8_t)(v ? 127 : -128) : (uint8_t)(v ? 255 : 0));
		}
		model.buffers.push_back(std::move(b));
		sg.tensors[t]->buffer = (uint32_t)model.buffers.size() - 1;
		set01((int)t, type);
	}
	for (size_t i = 0; i < nops; i++) {
		std::unique_ptr<OperatorT> &op = sg.operators[i];
		BuiltinOperator c = code_of(model, *op);
		int o = op->outputs.empty() ? -1 : op->outputs[0];
		if (drop[i]) continue;
		if (trunc_of[i] >= 0) {
			int x = trunc_of[i]; std::vector<int> shape = sg.tensors[o]->shape;
			int f = tensor(TensorType_FLOAT32, shape, 0, 0, nm("floor")), c0 = tensor(TensorType_FLOAT32, shape, 0, 0, nm("ceil"));
			int p = tensor(TensorType_FLOAT32, shape, 0, 0, nm("pos")), q = tensor(TensorType_FLOAT32, shape, 0, 0, nm("neg"));
			emit(BuiltinOperator_FLOOR, { x }, f); ceil(x, c0, shape);
			emit(BuiltinOperator_MAXIMUM, { f, constant(TensorType_FLOAT32, 0.0f, nm("zero")) }, p);
			emit(BuiltinOperator_MINIMUM, { c0, constant(TensorType_FLOAT32, 0.0f, nm("zero")) }, q);
			emit(BuiltinOperator_ADD, { p, q }, o);
			s.trunc++;
			continue;
		}
		if (unary[i]) {
			const std::vector<int> shape = sg.tensors[o]->shape;
			auto f32 = [&](const char *w) { return tensor(TensorType_FLOAT32, shape, 0, 0, nm(w)); };
			auto k = [&](float v) { return constant(TensorType_FLOAT32, v, nm("c")); };
			bool q = sg.tensors[o]->type != TensorType_FLOAT32;
			int x = op->inputs[0], y = o;
			if (q) { x = f32("dq"); emit(BuiltinOperator_DEQUANTIZE, { op->inputs[0] }, x); y = f32("res"); }
			if (unary[i] == BuiltinOperator_CEIL) { ceil(x, y, shape); s.ceil++; }
			else if (unary[i] == BuiltinOperator_COS) { int a = f32("shift"); emit(BuiltinOperator_ADD, { x, k(1.57079632679f) }, a); emit(BuiltinOperator_SIN, { a }, y); s.cos++; }
			else {
				int h = f32("h"), r = f32("r"), d = f32("d"), e = f32("e"), m = f32("m"), t0 = f32("t0"), tie = f32("tie");
				int hf = f32("half"), fh = f32("fh"), tw = f32("twice"), odd = f32("odd"), p = f32("p");
				emit(BuiltinOperator_ADD, { x, k(0.5f) }, h); emit(BuiltinOperator_FLOOR, { h }, r);
				emit(BuiltinOperator_SUB, { r, x }, d); emit(BuiltinOperator_SUB, { k(0.5f), d }, e);
				emit(BuiltinOperator_MUL, { e, k(60000.0f) }, m); emit(BuiltinOperator_SUB, { k(1.0f), m }, t0);
				emit(BuiltinOperator_MAXIMUM, { t0, k(0.0f) }, tie);
				emit(BuiltinOperator_MUL, { r, k(0.5f) }, hf); emit(BuiltinOperator_FLOOR, { hf }, fh); emit(BuiltinOperator_MUL, { fh, k(2.0f) }, tw);
				emit(BuiltinOperator_SUB, { r, tw }, odd); emit(BuiltinOperator_MUL, { tie, odd }, p); emit(BuiltinOperator_SUB, { r, p }, y);
				s.round++;
			}
			if (q) emit(BuiltinOperator_QUANTIZE, { y }, o);
			continue;
		}
		if (o >= 0 && o < (int)nt && repr[o] >= 0 && (is_cmp(c) || is_logic(c))) {
			TensorType type = (TensorType)repr[o];
			std::vector<int> shape = sg.tensors[o]->shape;
			set01(o, type);
			int a = op->inputs[0], b = op->inputs.size() > 1 ? op->inputs[1] : -1;
			auto tmp01 = [&](const char *w) { int t = tensor(type, shape, 0, 1, nm(w)); if (type != TensorType_FLOAT32) set01(t, type); return t; };
			switch (c) {
			case BuiltinOperator_GREATER: greater(a, b, o, type, shape); break;
			case BuiltinOperator_LESS: greater(b, a, o, type, shape); break;
			case BuiltinOperator_GREATER_EQUAL: { int g = tmp01("lt"); greater(b, a, g, type, shape); one_minus(g, o, type); break; }
			case BuiltinOperator_LESS_EQUAL: { int g = tmp01("gt"); greater(a, b, g, type, shape); one_minus(g, o, type); break; }
			case BuiltinOperator_NOT_EQUAL: case BuiltinOperator_EQUAL: {
				int g1 = tmp01("gt"), g2 = tmp01("lt");
				greater(a, b, g1, type, shape); greater(b, a, g2, type, shape);
				if (c == BuiltinOperator_NOT_EQUAL) emit(BuiltinOperator_ADD, { g1, g2 }, o);
				else { int ne = tmp01("ne"); emit(BuiltinOperator_ADD, { g1, g2 }, ne); one_minus(ne, o, type); }
				break; }
			case BuiltinOperator_LOGICAL_AND: emit(BuiltinOperator_MUL, { a, b }, o); break;
			case BuiltinOperator_LOGICAL_OR: emit(BuiltinOperator_MAXIMUM, { a, b }, o); break;
			case BuiltinOperator_LOGICAL_NOT: one_minus(a, o, type); break;
			default: break;
			}
			s.boolean++;
			continue;
		}
		if ((c == BuiltinOperator_SELECT || c == BuiltinOperator_SELECT_V2) && op->inputs.size() == 3 && repr[op->inputs[0]] >= 0) {
			int cnd = op->inputs[0], a = op->inputs[1], b = op->inputs[2];
			TensorType type = sg.tensors[a]->type;
			if (type != (TensorType)repr[cnd]) {   /* the condition in the type of the data */
				int t = tensor(type, sg.tensors[cnd]->shape, 0, 1, nm("cond"));
				if (type != TensorType_FLOAT32) set01(t, type);
				emit(type == TensorType_FLOAT32 ? BuiltinOperator_DEQUANTIZE : BuiltinOperator_QUANTIZE, { cnd }, t);
				cnd = t;
			}
			std::vector<int> shape = sg.tensors[o]->shape;
			float lo = 0, hi = 0;
			if (type != TensorType_FLOAT32) { QRange ra, rb; qrange(*sg.tensors[a], &ra); qrange(*sg.tensors[b], &rb); lo = ra.lo - rb.hi; hi = ra.hi - rb.lo; }
			int d = tensor(type, shape, lo, hi, nm("sel_diff")), p = tensor(type, shape, std::min(lo, 0.0f), std::max(hi, 0.0f), nm("sel_prod"));
			emit(BuiltinOperator_SUB, { a, b }, d);
			emit(BuiltinOperator_MUL, { cnd, d }, p);
			emit(BuiltinOperator_ADD, { b, p }, o);
			s.boolean++;
			continue;
		}
		if (c == BuiltinOperator_CAST && op->inputs.size() == 1 && repr[op->inputs[0]] >= 0) {
			TensorType type = (TensorType)repr[op->inputs[0]];
			if (type == TensorType_FLOAT32) {
				auto b = std::make_unique<BufferT>(); std::vector<int32_t> sh(sg.tensors[o]->shape.begin(), sg.tensors[o]->shape.end());
				b->data.resize(sh.size() * 4); memcpy(b->data.data(), sh.data(), b->data.size()); model.buffers.push_back(std::move(b));
				auto t = std::make_unique<TensorT>(); t->name = nm("shape"); t->type = TensorType_INT32; t->shape = { (int)sh.size() }; t->buffer = (uint32_t)model.buffers.size() - 1;
				sg.tensors.push_back(std::move(t));
				emit(BuiltinOperator_RESHAPE, { op->inputs[0], (int)sg.tensors.size() - 1 }, o);
			} else emit(BuiltinOperator_DEQUANTIZE, { op->inputs[0] }, o);
			s.boolean++;
			continue;
		}
		out.push_back(std::move(op));
	}
	sg.operators = std::move(out);
	return true;
}

/* PRELU with one alpha for all channels: TIM-VX crashes on such an int8 / uint8 PRELU (alpha of shape [1, 1, 1]); the same operator with the
 * constant alpha repeated along the channels (shape [1, ..., 1, C], as the per-channel PRELU that runs right) gives the same result. */
static inline bool expand_prelu_alpha(ModelT &model, Stats &s)
{
	SubGraphT &sg = *model.subgraphs[0];
	bool changed = false;
	for (auto &op : sg.operators) {
		if (code_of(model, *op) != BuiltinOperator_PRELU || op->inputs.size() != 2) continue;
		const TensorT &x = *sg.tensors[op->inputs[0]];
		TensorT &a = *sg.tensors[op->inputs[1]];
		size_t es = a.type == TensorType_FLOAT32 ? 4 : a.type == TensorType_INT8 || a.type == TensorType_UINT8 ? 1 : 0;
		if (!es || x.shape.empty() || x.shape.back() <= 1 || a.buffer == 0 || a.buffer >= model.buffers.size()) continue;
		const std::vector<uint8_t> &src = model.buffers[a.buffer]->data;
		if (src.size() != es) continue;   /* only one alpha */
		int c = x.shape.back();
		auto b = std::make_unique<BufferT>();
		for (int i = 0; i < c; i++) b->data.insert(b->data.end(), src.begin(), src.end());
		model.buffers.push_back(std::move(b));
		/* a new tensor: the old one may be used elsewhere */
		auto t = std::make_unique<TensorT>();
		t->name = a.name + "_npu_per_channel"; t->type = a.type; t->buffer = (uint32_t)model.buffers.size() - 1;
		t->shape.assign(x.shape.size() - 1, 1); t->shape.back() = c;
		if (a.quantization) { t->quantization = std::make_unique<QuantizationParametersT>(); t->quantization->scale = a.quantization->scale; t->quantization->zero_point = a.quantization->zero_point; }
		sg.tensors.push_back(std::move(t));
		op->inputs[1] = (int)sg.tensors.size() - 1;
		s.prelu++; changed = true;
	}
	return changed;
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
	sg.operators = std::move(out);
	changed = rewrite_bool(*model, s) || changed;
	changed = expand_prelu_alpha(*model, s) || changed;
	if (!changed) { if (st) *st = s; return ""; }
	flatbuffers::FlatBufferBuilder fbb(bytes.size() + 4096);
	fbb.Finish(Model::Pack(fbb, model.get()), ModelIdentifier());
	bytes.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
	if (st) *st = s;
	return "";
}

}  // namespace npu_rewrite
#endif
