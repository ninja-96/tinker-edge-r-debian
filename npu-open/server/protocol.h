/* Protocol between the host and the server on the NPU die: messages over the USB bulk channel (all values little endian).
 * Every message: a 20-byte header and a payload of `len` bytes. A reply has the type, the tag and the model of its request;
 * flags bit 0 = 1 marks an error reply (the payload is a UTF-8 text).
 * Version 2 (release 0.4.1): several models at once, each with an id; the models stay loaded until UNLOAD (also when the host goes away). */
#pragma once
#include <stdint.h>

#define NPU_MAGIC 0x3255504eu /* "NPU2" */
#define NPU_PROTO_VERSION 2
#define NPU_ALL_MODELS 0xffffffffu   /* UNLOAD of every model */

enum npu_msg_type {
	NPU_HELLO  = 1,  /* request: empty. reply: struct npu_hello */
	NPU_LOAD   = 2,  /* request: struct npu_load_req + the .tflite bytes. reply (header.model = the new id): struct npu_load_reply +
	                  * (n_in + n_out) * struct npu_tensor + u32 nbg_len + the compiled graph (NBG) of the model: keep it and use LOAD_NBG next time.
	                  * A model with the same key that is loaded already is not loaded again: the reply describes it (nbg_len = 0, attached = 1) */
	NPU_RUN    = 3,  /* request (header.model): the input tensors, concatenated. reply: u32 invoke_us, then the output tensors concatenated */
	NPU_UNLOAD = 4,  /* request (header.model, or NPU_ALL_MODELS): empty. reply: empty */
	NPU_PING   = 5,  /* request: any payload. reply: the same payload */
	NPU_LOAD_NBG = 6, /* request: struct npu_load_req + u32 n_in, u32 n_out, (n_in + n_out) * struct npu_tensor, then the compiled graph (NBG).
	                   * reply: as LOAD, nbg_len = 0 */
	NPU_LIST   = 7,  /* request: empty. reply: u32 count, then count * struct npu_model_entry */
	NPU_DESCRIBE = 8, /* request (header.model): empty. reply: as LOAD (nbg_len = 0, attached = 1) */
	NPU_OPS    = 9,  /* request (header.model): empty. reply: u32 count, then count * struct npu_op_stat: the operators of the model and where they run */
};

/* struct npu_load_req.flags: the plan of a LOAD (an NBG load is always the whole graph on the NPU) */
#define NPU_LOAD_CPU_ONLY 1   /* no VX delegate: TensorFlow Lite with XNNPACK on the CPU of the NPU die */
#define NPU_LOAD_NO_SPLIT 2   /* per-channel int8 layers are not split into per-tensor groups (the slow, untouched path of the NPU) */
#define NPU_LOAD_STRICT   4   /* the whole model on the NPU or an error naming the operators that would stay on the CPU (device "npu" of the host library) */
#define NPU_MAX_DENY 30

#define NPU_FLAG_ERROR 1
#define NPU_FLAG_PAD   2  /* one byte of padding follows the payload: a USB transfer whose size is a multiple of 1024 has no short packet at the end and the reader's
                           * pending read would not complete until the next message arrives; every message is sent as ONE stream whose size is never a multiple of 1024 */

struct npu_hdr {
	uint32_t magic;
	uint16_t type;
	uint16_t flags;
	uint32_t tag;    /* chosen by the host, repeated in the reply (several requests may be in flight) */
	uint32_t model;  /* the model a request is for (RUN, UNLOAD, DESCRIBE); in a LOAD reply: the id given to the model */
	uint32_t len;
} __attribute__((packed));

struct npu_hello {
	uint32_t proto;
	uint32_t chip_model, chip_revision, chip_product;
	uint32_t chip_ok;           /* 1: this is the chip the build is made for (0x8000 / 0x7100 / 0x45080001) */
	uint32_t mem_total_kb, mem_avail_kb;
	uint32_t models;            /* number of loaded models */
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

struct npu_load_req {
	char key[72];               /* identity of the model: sha256 of the .tflite file (hex), chosen by the host */
	char name[56];              /* a name for lists (the file name) */
	uint32_t flags;             /* NPU_LOAD_* */
	uint16_t deny_ops[NPU_MAX_DENY];  /* builtin operator codes left to the CPU (0 ends the list): a mixed plan made by autotune */
} __attribute__((packed));

struct npu_op_stat {
	uint16_t code;              /* TfLiteBuiltinOperator (custom operators: 0xffff) */
	uint16_t total;             /* operators of this type in the model (after our rewrites) */
	uint16_t on_npu;            /* of them taken by the VX delegate */
	uint16_t reserved;
} __attribute__((packed));

struct npu_load_reply {
	uint32_t n_inputs, n_outputs;
	uint32_t load_ms;           /* time of graph preparation on the NPU */
	uint32_t mem_used_kb;       /* memory taken by the model */
	uint32_t attached;          /* 1: the model was loaded already (by this or another program) */
} __attribute__((packed));

struct npu_model_entry {
	uint32_t id;
	uint32_t n_inputs, n_outputs;
	uint32_t load_ms, mem_used_kb;
	uint32_t runs;              /* RUN requests served */
	uint32_t age_s;             /* seconds since it was loaded */
	uint32_t from_nbg;          /* 1: loaded from a compiled graph (no TensorFlow Lite in its engine) */
	char key[72];
	char name[56];
	char plan[48];              /* "npu", "npu-nosplit", "cpu", "mixed:<codes on the CPU>" */
} __attribute__((packed));
