#pragma once

#include "FrameTypes.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace lbgm461::wire {

inline void AppendVarint(std::uint64_t value, std::vector<std::uint8_t>& out) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

inline void AppendTag(std::uint32_t field_number, std::uint8_t wire_type, std::vector<std::uint8_t>& out) {
    AppendVarint((static_cast<std::uint64_t>(field_number) << 3U) | wire_type, out);
}

inline void AppendBytesField(std::uint32_t field_number, const std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& out) {
    AppendTag(field_number, 2, out);
    AppendVarint(bytes.size(), out);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline void AppendStringField(std::uint32_t field_number, const std::string& value, std::vector<std::uint8_t>& out) {
    AppendTag(field_number, 2, out);
    AppendVarint(value.size(), out);
    out.insert(out.end(), value.begin(), value.end());
}

inline void AppendBoolField(std::uint32_t field_number, bool value, std::vector<std::uint8_t>& out) {
    AppendTag(field_number, 0, out);
    AppendVarint(value ? 1U : 0U, out);
}

inline void AppendInt32Field(std::uint32_t field_number, std::int32_t value, std::vector<std::uint8_t>& out) {
    AppendTag(field_number, 0, out);
    AppendVarint(static_cast<std::uint32_t>(value), out);
}

inline void AppendInt64Field(std::uint32_t field_number, std::int64_t value, std::vector<std::uint8_t>& out) {
    AppendTag(field_number, 0, out);
    AppendVarint(static_cast<std::uint64_t>(value), out);
}

inline void AppendFloatField(std::uint32_t field_number, float value, std::vector<std::uint8_t>& out) {
    AppendTag(field_number, 5, out);
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    out.push_back(static_cast<std::uint8_t>(raw & 0xFF));
    out.push_back(static_cast<std::uint8_t>((raw >> 8U) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((raw >> 16U) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((raw >> 24U) & 0xFF));
}

inline void AppendDoubleField(std::uint32_t field_number, double value, std::vector<std::uint8_t>& out) {
    AppendTag(field_number, 1, out);
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((raw >> shift) & 0xFF));
    }
}

inline std::vector<std::uint8_t> EncodeImageMatData(const ImageBuffer& buffer) {
    std::vector<std::uint8_t> out;
    AppendInt32Field(1, buffer.height, out);
    AppendInt32Field(2, buffer.width, out);
    AppendBytesField(3, buffer.data, out);
    AppendInt32Field(4, buffer.mat_type, out);
    return out;
}

inline std::vector<std::uint8_t> EncodeShareData(const FrameSnapshot& snapshot) {
    std::vector<std::uint8_t> out;

    if (!snapshot.ip.empty()) {
        AppendStringField(1, snapshot.ip, out);
    }

    AppendBytesField(2, EncodeImageMatData(snapshot.color), out);
    AppendBytesField(3, EncodeImageMatData(snapshot.depth), out);
    AppendInt32Field(4, snapshot.intrinsics.width, out);
    AppendInt32Field(5, snapshot.intrinsics.height, out);
    AppendDoubleField(6, snapshot.intrinsics.ppx, out);
    AppendDoubleField(7, snapshot.intrinsics.ppy, out);
    AppendDoubleField(8, snapshot.intrinsics.fx, out);
    AppendDoubleField(9, snapshot.intrinsics.fy, out);
    AppendInt32Field(10, snapshot.intrinsics.model, out);

    if (!snapshot.intrinsics.coeffs.empty()) {
        std::vector<std::uint8_t> packed_coeffs;
        for (double coeff : snapshot.intrinsics.coeffs) {
            std::uint64_t raw = 0;
            std::memcpy(&raw, &coeff, sizeof(raw));
            for (int shift = 0; shift < 64; shift += 8) {
                packed_coeffs.push_back(static_cast<std::uint8_t>((raw >> shift) & 0xFF));
            }
        }
        AppendTag(11, 2, out);
        AppendVarint(packed_coeffs.size(), out);
        out.insert(out.end(), packed_coeffs.begin(), packed_coeffs.end());
    }

    AppendFloatField(13, snapshot.depth_scale, out);
    return out;
}

}  // namespace lbgm461::wire
