// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

// The open firmware exposes a vendor interface with one bulk endpoint in each direction.
inline bool npu_usb_channel(const std::string &raw, unsigned &iface, unsigned &in, unsigned &out)
{
	bool selected = false;
	in = out = 0;
	for (size_t pos = 18; pos < raw.size();) {
		if (raw.size() - pos < 2) return false;
		const auto *d = reinterpret_cast<const uint8_t *>(raw.data() + pos);
		unsigned len = d[0];
		if (len < 2 || len > raw.size() - pos) return false;
		if (d[1] == 2 || d[1] == 4) {
			if (selected && in && out) return true;
			selected = d[1] == 4 && len >= 9 && d[3] == 0 && d[5] == 0xff && d[6] == 0x4e && d[7] == 0x50;
			if (selected) iface = d[2];
			in = out = 0;
		} else if (selected && d[1] == 5 && len >= 7 && (d[3] & 3) == 2) {
			unsigned &ep = (d[2] & 0x80) ? in : out;
			if (ep || !(d[2] & 15)) return false;
			ep = d[2];
		}
		pos += len;
	}
	return selected && in && out;
}
