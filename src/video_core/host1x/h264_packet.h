// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

#include "common/common_types.h"

namespace Tegra::Host1x {

inline bool IsH264Stream(std::span<const u8> data) {
    const auto valid_nal = [](u8 header) {
        const u8 type = header & 0x1f;
        return (header & 0x80) == 0 && type >= 1 && type <= 23;
    };
    if (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1 &&
        valid_nal(data[3])) {
        return true;
    }
    if (data.size() >= 5) {
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1 &&
            valid_nal(data[4])) {
            return true;
        }
        const u32 length = (u32{data[0]} << 24) | (u32{data[1]} << 16) |
                           (u32{data[2]} << 8) | u32{data[3]};
        if (length > 0 && length <= data.size() - 4 && valid_nal(data[4])) {
            return true;
        }
    }
    return data.size() >= 7 && data[0] == 1 && (data[4] & 0xfc) == 0xfc &&
           (data[5] & 0xe0) == 0xe0 && (data[5] & 0x1f) != 0;
}

class H264PacketConverter {
public:
    void Reset() {
        format = Format::Unknown;
    }

    void Convert(std::span<u8> data) {
        if (format == Format::AnnexB) {
            return;
        }
        const bool length_prefixed = IsLengthPrefixed(data);
        if (format == Format::Unknown) {
            const bool annex_b = HasStartCode(data);
            // An ambiguous packet cannot establish the framing of the stream.
            if (length_prefixed == annex_b) {
                return;
            }
            format = length_prefixed ? Format::LengthPrefixed : Format::AnnexB;
        }
        if (format != Format::LengthPrefixed || !length_prefixed) {
            return;
        }

        std::size_t offset = 0;
        while (offset < data.size()) {
            const u32 length = ReadLength(data.subspan(offset));
            data[offset] = 0;
            data[offset + 1] = 0;
            data[offset + 2] = 0;
            data[offset + 3] = 1;
            offset += std::size_t{4} + length;
        }
    }

private:
    enum class Format { Unknown, AnnexB, LengthPrefixed };

    static u32 ReadLength(std::span<const u8> data) {
        return (static_cast<u32>(data[0]) << 24) | (static_cast<u32>(data[1]) << 16) |
               (static_cast<u32>(data[2]) << 8) | static_cast<u32>(data[3]);
    }

    static bool IsLengthPrefixed(std::span<const u8> data) {
        if (data.empty()) {
            return false;
        }
        while (data.size() >= 4) {
            const u32 length = ReadLength(data);
            data = data.subspan(4);
            if (length == 0 || length > data.size()) {
                return false;
            }
            data = data.subspan(length);
        }
        return data.empty();
    }

    static bool HasStartCode(std::span<const u8> data) {
        return (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
               (data.size() >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
    }

    Format format{Format::Unknown};
};

} // namespace Tegra::Host1x
