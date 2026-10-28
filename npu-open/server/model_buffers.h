/* Constant data stored after the flatbuffer of a TFLite model.
 *
 * Newer converters (ai-edge-torch, TF 2.15+ for large models) keep big constants outside the flatbuffer: the Buffer table then holds an offset
 * from the start of the file and a size instead of the data. The object API (UnPackModel / Pack) only carries the flatbuffer, so a model that is
 * rewritten and packed again would point at data that is no longer there ("Constant buffer N specified an out of range offset"). The passes of
 * the server call this right after UnPackModel: the data is copied into the buffers, which makes the model self-contained.
 */
#ifndef NPU_MODEL_BUFFERS_H
#define NPU_MODEL_BUFFERS_H

#include <string>
#include <vector>
#include "tflite/schema/schema_generated.h"

namespace npu_model {

/* "" or an error text */
static inline std::string inline_buffers(tflite::ModelT &model, const std::vector<uint8_t> &bytes)
{
	for (auto &b : model.buffers) {
		if (!b || b->offset <= 1) continue;   /* 0: data inside the flatbuffer (or none), 1: a placeholder of the converter */
		if (b->offset > bytes.size() || b->size > bytes.size() - b->offset) return "a constant buffer points outside the model file";
		b->data.assign(bytes.begin() + b->offset, bytes.begin() + b->offset + b->size);
		b->offset = 0; b->size = 0;
	}
	return "";
}

}  // namespace npu_model

#endif
