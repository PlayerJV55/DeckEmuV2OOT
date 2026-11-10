// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/swap.h"
#include "core/loader/nso.h"
#include "core/loader/nso_compatibility.h"

namespace {
struct Symbol {
    u32_le name{};
    u8 info{0x12};
    u8 other{};
    u16_le section{1};
    u64_le value{0x20};
    u64_le size{8};
};
static_assert(sizeof(Symbol) == 24);
struct Module {
    Loader::NSOHeader header{};
    std::vector<u8> bytes = std::vector<u8>(0x400);
    Symbol symbol;
    Module() {
        constexpr char name[] = "_ZN2nn2pl16IsRunningOnOunceEv";
        header.segments[0] = {0, 0, 0x80, {}};
        header.segments[1] = {0, 0x100, 0x200, {}};
        header.dynstr_extent = {0, sizeof(name)};
        header.dynsyn_extent = {0x80, sizeof(Symbol)};
        std::memcpy(bytes.data() + 0x100, name, sizeof(name));
        Store(0x20, 0x2a1f03e0);
        Store(0x24, 0xd65f03c0);
    }
    void Store(size_t offset, u32 value) {
        const u32_le word{value};
        std::memcpy(bytes.data() + offset, &word, sizeof(word));
    }
    void WriteSymbol(size_t offset = 0x180) {
        std::memcpy(bytes.data() + offset, &symbol, sizeof(symbol));
    }
};
} // namespace

TEST_CASE("Ounce compatibility changes only a proven exported platform-query stub", "[nso_compatibility]") {
    Module module;
    SECTION("MOV W0 WZR") {}
    SECTION("MOV W0 immediate zero") { module.Store(0x20, 0x52800000); }
    SECTION("Weak exported function") { module.symbol.info = 0x22; }
    module.WriteSymbol();
    auto expected = module.bytes;
    const u32_le result{0x52800020};
    std::memcpy(expected.data() + 0x20, &result, sizeof(result));
    CHECK(Loader::ApplyOuncePlatformCompatibility(module.bytes, module.header) == u64{0x20});
    CHECK(module.bytes == expected);
    CHECK_FALSE(Loader::ApplyOuncePlatformCompatibility(module.bytes, module.header));
    CHECK(module.bytes == expected);
}

TEST_CASE("Ounce compatibility refuses ambiguous or malformed module metadata", "[nso_compatibility]") {
    Module module;
    SECTION("String table offset overflow") { module.header.dynstr_extent.data_offset = std::numeric_limits<u32>::max(); }
    SECTION("String table size overflow") { module.header.dynstr_extent.size = std::numeric_limits<u32>::max(); }
    SECTION("Symbol table offset overflow") { module.header.dynsyn_extent.data_offset = std::numeric_limits<u32>::max(); }
    SECTION("Incomplete symbol") { module.header.dynsyn_extent.size = sizeof(Symbol) - 1; }
    SECTION("Missing name terminator") { module.bytes[0x100 + module.header.dynstr_extent.size - 1] = 'x'; }
    SECTION("Wrong symbol name") { module.bytes[0x101] = 'x'; }
    SECTION("Name outside string table") { module.symbol.name = std::numeric_limits<u32>::max(); }
    SECTION("Undefined import") { module.symbol.section = 0; }
    SECTION("Local function") { module.symbol.info = 2; }
    SECTION("Data symbol") { module.symbol.info = 0x11; }
    SECTION("Hidden symbol") { module.symbol.other = 2; }
    SECTION("Different function size") { module.symbol.size = 12; }
    SECTION("Function outside text") { module.symbol.value = 0x100; }
    SECTION("Unaligned function") { module.symbol.value = 0x21; }
    SECTION("Text region outside image") { module.header.segments[0].size = 0x1000; }
    SECTION("Rodata outside image") { module.header.segments[1].location = 0x1000; }
    SECTION("Overlapping text and metadata") { module.header.segments[0].size = 0x200; }
    SECTION("Unrecognized function body") { module.Store(0x20, 0x52800040); }
    SECTION("No return instruction") { module.Store(0x24, 0x14000000); }
    SECTION("Duplicate exports") {
        module.header.dynsyn_extent.size = 2 * sizeof(Symbol);
        module.WriteSymbol(0x180 + sizeof(Symbol));
    }
    module.WriteSymbol();
    const auto before = module.bytes;
    CHECK_FALSE(Loader::ApplyOuncePlatformCompatibility(module.bytes, module.header));
    CHECK(module.bytes == before);
}
