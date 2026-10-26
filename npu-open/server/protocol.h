/* Protocol between the host and the server on the NPU die: messages over the USB bulk channel (all values little endian).
 * Every message: a 16-byte header and a payload of `len` bytes. A reply has the type of its request and the tag of its request;
 * flags bit 0 = 1 marks an error reply (the payload is a UTF-8 text). */
#pragma once
#include <stdint.h>

#define NPU_MAGIC 0x3155504eu /* "NPU1" */
#define NPU_PROTO_VERSION 1

enum npu_msg_type {
	NPU_HELLO  = 1,  /* request: empty. reply: struct npu_hello */
	NPU_LOAD   = 2,  /* request: the .tflite bytes. reply: struct npu_load_reply + (n_in + n_out) * struct npu_tensor + u32 nbg_len + the
	                  * compiled graph (NBG) of the model: keep it and use LOAD_NBG next time, no 13 seconds of graph building */
	NPU_RUN    = 3,  /* request: the input tensors, concatenated. reply: u32 invoke_us, then the output tensors concatenated */
	NPU_UNLOAD = 4,  /* request: empty. reply: empty */
	NPU_PING   = 5,  /* request: any payload. reply: the same payload */
	NPU_LOAD_NBG = 6, /* request: u32 n_in, u32 n_out, (n_in + n_out) * struct npu_tensor, then the compiled graph (NBG). reply: as LOAD, nbg_len = 0 */
};

#define NPU_FLAG_ERROR 1
#define NPU_FLAG_PAD   2  /* one byte of padding follows the payload: a USB transfer whose size is a multiple of 1024 has no short packet at the end and the reader's
                           * pending read would not complete until the next message arrives; every message is sent as ONE stream whose size is never a multiple of 1024 */

struct npu_hdr {
	uint32_t magic;
	uint16_t type;
	uint16_t flags;
	uint32_t tag;  /* chosen by the host, repeated in the reply (several requests may be in flight) */
	uint32_t len;
} __attribute__((packed));

struct npu_hello {
	uint32_t proto;
	uint32_t chip_model, chip_revision, chip_product;
	uint32_t chip_ok;           /* 1: this is the chip the build is made for (0x8000 / 0x7100 / 0x45080001) */
	uint32_t mem_total_kb, mem_avail_kb;
	uint32_t model_loaded;      /* 1: a model is loaded, LOAD is refused until UNLOAD */
	char driver[32];            /* galcore version */
} __attribute__((packed));

struct npu_tensor {
	uint8_t type;               /* TfLiteType: 1 float32, 2 int32, 3 uint8, 9 int8, 7 int16, 10 float16 ... */
	uint8_t ndims;
	uint16_t reserved;
	int32_t dims[6];
	float scale;
	int32_t zero_point;
	uint32_t bytes;
	char name[32];
} __attribute__((packed));

struct npu_load_reply {
	uint32_t n_inputs, n_outputs;
	uint32_t load_ms;           /* time of graph preparation on the NPU */
	uint32_t mem_used_kb;       /* memory taken by the model */
} __attribute__((packed));
