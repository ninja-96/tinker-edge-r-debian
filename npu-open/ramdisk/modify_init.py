#!/usr/bin/env python3
"""Makes the two init scripts of the Rockchip NPU ramdisk start our server instead of rknn_server.

    modify_init.py S50usbdevice S50usbdevice.new
    modify_init.py S60NPU_init   S60NPU_init.new

(the originals come from the ramdisk of Rockchip's boot.img: unpack with tools/npu_ramdisk.py --list / take them from the image, do not store them here)
S50usbdevice: after the FunctionFS mount of the "ntb" function our server is started and must write its descriptors before the USB controller is bound;
              if it is not ready in 10 s the stock rknn_server path is used (/tmp/npu.fallback), so that the NPU stays reachable for debugging.
S60NPU_init:  start_rknn.sh is started only in that fallback case.
"""
import sys

S50_OLD = '''	if [ $NTB_EN = on ];then
		mkdir -p /dev/usb-ffs/ntb
		mount -o uid=2000,gid=2000 -t functionfs ntb /dev/usb-ffs/ntb
	fi
'''
S50_NEW = '''	if [ $NTB_EN = on ];then
		mkdir -p /dev/usb-ffs/ntb
		mount -o uid=2000,gid=2000 -t functionfs ntb /dev/usb-ffs/ntb
		# our server takes the "ntb" function: it writes the descriptors, then the controller is bound (FunctionFS
		# cannot reopen a function that was closed while bound). Not ready in 10 s: the stock rknn_server is used.
		if [ -x /usr/bin/npu_server ]; then
			rm -f /tmp/npu.ready /tmp/npu.fallback
			/usr/bin/npu_server > /dev/null 2>&1 &
			NPU_PID=$!
			i=0
			while [ ! -e /tmp/npu.ready ] && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done
			if [ ! -e /tmp/npu.ready ]; then
				echo "npu server not ready after 10 s: fallback to rknn_server" >> /tmp/npu_server.log
				kill $NPU_PID 2>/dev/null
				sleep 1
				touch /tmp/npu.fallback
			fi
		else
			touch /tmp/npu.fallback
		fi
	fi
'''
S60_OLD = "\t\tstart_rknn.sh &"
S60_NEW = "\t\t[ -e /tmp/npu.fallback ] && start_rknn.sh &"


def main(src, dst):
    text = open(src).read()
    if "S50usbdevice" in src:
        old, new = S50_OLD, S50_NEW
    elif "S60NPU_init" in src:
        old, new = S60_OLD, S60_NEW
    else:
        sys.exit("give S50usbdevice or S60NPU_init")
    if old not in text:
        sys.exit("%s: the expected text was not found: another version of the firmware?" % src)
    open(dst, "w").write(text.replace(old, new, 1))


if len(sys.argv) != 3:
    sys.exit(__doc__)
main(*sys.argv[1:])
