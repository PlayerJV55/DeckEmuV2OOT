// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <string_view>
#include "common/swap.h"
#include "core/loader/nso.h"
#include "core/loader/nso_compatibility.h"

namespace Loader {
namespace {
struct ElfSymbol {
    u32_le name;
    u8 info;
    u8 other;
    u16_le section;
    u64_le value;
    u64_le size;
};
static_assert(sizeof(ElfSymbol) == 24);

bool Within(u64 offset, u64 size, size_t total) {
    return offset <= total && size <= total - offset;
}
} // namespace

std::optional<u64> ApplyOuncePlatformCompatibility(std::span<u8> image, const NSOHeader& header) {
    constexpr std::string_view Name = "_ZN2nn2pl16IsRunningOnOunceEv";
    const auto& code = header.segments[0];
    const auto& ro = header.segments[1];
    if (!Within(code.location, code.size, image.size()) ||
        !Within(ro.location, ro.size, image.size()) ||
        !Within(header.dynstr_extent.data_offset, header.dynstr_extent.size, ro.size) ||
        !Within(header.dynsyn_extent.data_offset, header.dynsyn_extent.size, ro.size) ||
        (u64{code.location} < u64{ro.location} + ro.size &&
         u64{ro.location} < u64{code.location} + code.size) ||
        header.dynsyn_extent.size % sizeof(ElfSymbol) != 0) {
        return std::nullopt;
    }
    const auto rodata = image.subspan(ro.location, ro.size);
    const auto strings = rodata.subspan(header.dynstr_extent.data_offset, header.dynstr_extent.size);
    const auto symbols = rodata.subspan(header.dynsyn_extent.data_offset, header.dynsyn_extent.size);
    std::optional<u64> candidate;
    for (size_t offset = 0; offset < symbols.size(); offset += sizeof(ElfSymbol)) {
        ElfSymbol symbol{};
        std::memcpy(&symbol, symbols.data() + offset, sizeof(symbol));
        if (!Within(symbol.name, Name.size() + 1, strings.size()) ||
            std::memcmp(strings.data() + symbol.name, Name.data(), Name.size()) != 0 ||
            strings[symbol.name + Name.size()] != 0) {
            continue;
        }
        // Match an exported function, never an import, data symbol or partial name.
        const auto binding = symbol.info >> 4;
        if ((symbol.info & 0xf) != 2 || (binding != 1 && binding != 2) ||
            symbol.section == 0 || (symbol.other & 3) != 0 || symbol.size != 8 ||
            symbol.value < code.location || (symbol.value & 3) != 0 ||
            !Within(symbol.value - code.location, 8, code.size) || candidate) {
            return std::nullopt;
        }
        u32_le result{}, terminator{};
        std::memcpy(&result, image.data() + symbol.value, sizeof(result));
        std::memcpy(&terminator, image.data() + symbol.value + 4, sizeof(terminator));
        if ((result != 0x2a1f03e0 && result != 0x52800000) || terminator != 0xd65f03c0) {
            return std::nullopt;
        }
        candidate = symbol.value;
    }
    if (candidate) {
        const u32_le return_true{0x52800020};
        std::memcpy(image.data() + *candidate, &return_true, sizeof(return_true));
    }
    return candidate;
}
} // namespace Loader
