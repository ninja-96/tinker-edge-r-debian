# Hardware video decoding (release 0.6): the mainline rkvdec V4L2 stateless
# decoder (device tree variant -browser) is exposed to Chromium by the
# v4l2_request VA-API driver (scripts/build-v4l2-request.sh, installed to
# /usr/lib/<multiarch>/dri/). Chromium's own V4L2 decoder was removed upstream,
# so video decode goes through VA-API. Select the driver explicitly; with it
# absent Chromium falls back to software decode.
export LIBVA_DRIVER_NAME=v4l2_request
