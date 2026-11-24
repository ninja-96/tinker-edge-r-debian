// Run with c++ -std=c++17 tests/npu/usb_channel.cc -o /tmp/channel && /tmp/channel
#include "../../npu-open/host/npu_usb_channel.h"
#include <cassert>

int main()
{
	const std::string prefix(18, '\0');
	const std::string iface("\x09\x04\x03\x00\x02\xff\x4e\x50\x00", 9);
	const std::string rx("\x07\x05\x81\x02\x00\x02\x00", 7);
	const std::string tx("\x07\x05\x02\x02\x00\x02\x00", 7);
	unsigned number, in, out;
	for (const auto &endpoints : {rx + tx, tx + rx}) {
		assert(npu_usb_channel(prefix + iface + endpoints, number, in, out));
		assert(number == 3 && in == 0x81 && out == 2);
	}
	for (const auto &invalid : {rx + rx, rx + iface + tx, rx + tx + rx.substr(0, 1), tx + rx.substr(0, 4), std::string(2, 0)})
		assert(!npu_usb_channel(prefix + iface + invalid, number, in, out));
}
