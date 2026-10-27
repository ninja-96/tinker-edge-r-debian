# Makes the two small test models of the NPU self-test and their expected outputs (run inside a TensorFlow 2.13 container, on the PC):
#   docker run --rm -u $(id -u):$(id -g) -v $PWD/tests/npu:/w --entrypoint python npu-tf213 /w/make_models.py
# net_int8.tflite   int8, per-channel weights whose scales differ a lot (the server splits such layers into groups, as it does for MobileNet V2)
# net_uint8.tflite  uint8, per-tensor weights (the fast path of the NPU)
# Both: 32x32x3 input, conv 3x3 - depthwise 3x3 - conv 1x1 - global mean - conv 1x1 (10 classes). expected.npz holds the input and what TFLite on the CPU gives.
import numpy as np, tensorflow as tf
rng = np.random.RandomState(7)
L = tf.keras.layers

def build():
    inp = tf.keras.Input((32, 32, 3), batch_size=1)
    x = L.Conv2D(16, 3, padding="same", activation="relu6")(inp)
    x = L.DepthwiseConv2D(3, padding="same", activation="relu6")(x)
    x = L.Conv2D(32, 1, activation="relu6")(x)
    x = L.Lambda(lambda t: tf.reduce_mean(t, axis=[1, 2], keepdims=True))(x)
    x = L.Conv2D(10, 1)(x)
    m = tf.keras.Model(inp, tf.reshape(x, [1, 10]))
    for l in m.layers:   # channel scales that differ by up to 40 times
        if isinstance(l, (L.Conv2D, L.DepthwiseConv2D)):
            w = l.get_weights(); k = w[0] * rng.uniform(0.5, 1.5, w[0].shape)
            n = k.shape[-1] if isinstance(l, L.Conv2D) else k.shape[2] * k.shape[3]
            f = np.exp(rng.uniform(0, np.log(40), n))
            k = k * (f.reshape(1, 1, 1, -1) if isinstance(l, L.Conv2D) else f.reshape(1, 1, -1, 1))
            l.set_weights([k.astype(np.float32), rng.randn(*w[1].shape).astype(np.float32) * 0.1])
    return m

m = build()
X = rng.uniform(0, 1, (8, 32, 32, 3)).astype(np.float32)
def rep():
    for i in range(8): yield [X[i:i + 1]]

def convert(name, io_type, per_channel):
    c = tf.lite.TFLiteConverter.from_keras_model(m)
    c.optimizations = [tf.lite.Optimize.DEFAULT]; c.representative_dataset = rep
    c.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    c.inference_input_type = io_type; c.inference_output_type = io_type
    c._experimental_disable_per_channel = not per_channel
    data = c.convert(); open("/w/%s.tflite" % name, "wb").write(data)
    it = tf.lite.Interpreter(model_content=data); it.allocate_tensors()
    i, o = it.get_input_details()[0], it.get_output_details()[0]
    s, z = i["quantization"]; q = np.clip(np.round(X[:1] / s + z), np.iinfo(i["dtype"]).min, np.iinfo(i["dtype"]).max).astype(i["dtype"])
    it.set_tensor(i["index"], q); it.invoke()
    return q, it.get_tensor(o["index"])

qi, oi = convert("net_int8", tf.int8, True)
qu, ou = convert("net_uint8", tf.uint8, False)
np.savez("/w/expected.npz", in_int8=qi, out_int8=oi, in_uint8=qu, out_uint8=ou)
print("ok", oi.ravel(), ou.ravel())
