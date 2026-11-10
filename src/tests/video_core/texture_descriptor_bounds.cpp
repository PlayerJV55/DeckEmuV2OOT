// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "video_core/textures/texture.h"

namespace {
struct TextureMemory {
    bool native{};
    unsigned reads{};
    GPUVAddr last_address{};

    bool UsesNativeTiling() const {
        return native;
    }
    void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) {
        ++reads;
        last_address = address;
        std::memset(output, 0x5a, size);
    }
};
} // namespace

TEST_CASE("Texture descriptor reads respect the inclusive pool limit", "[video_core][texture]") {
    constexpr GPUVAddr base = 0x1000;
    constexpr u32 limit = 5;
    TextureMemory memory;

    // Packed handles keep their independent sampler bits out of the texture index.
    const auto last =
        Tegra::Texture::ReadTextureDescriptor(memory, base, limit, false, (7U << 20) | limit);
    CHECK(memory.reads == 1);
    CHECK(memory.last_address == base + limit * sizeof(last));
    CHECK(last.address_low == 0x5a5a5a5a);

    const auto invalid =
        Tegra::Texture::ReadTextureDescriptor(memory, base, limit, false, (7U << 20) | (limit + 1));
    CHECK(memory.reads == 1);
    CHECK(invalid.address_low == 0);

    // Native byte offsets are decoded before the limit is enforced.
    memory.native = true;
    (void)Tegra::Texture::ReadTextureDescriptor(memory, base, limit, true, limit);
    CHECK(memory.reads == 2);
    CHECK(memory.last_address == base + limit * sizeof(last));
    const auto native_last =
        Tegra::Texture::ReadTextureDescriptor(memory, base, limit, true, limit * 32);
    CHECK(memory.reads == 3);
    CHECK(memory.last_address == base + limit * sizeof(native_last));
    (void)Tegra::Texture::ReadTextureDescriptor(memory, base, limit, false,
                                                (7U << 20) | (limit * 32));
    CHECK(memory.reads == 4);
    CHECK(memory.last_address == base + limit * sizeof(native_last));
    const auto native_invalid =
        Tegra::Texture::ReadTextureDescriptor(memory, base, limit, true, (limit + 1) * 32);
    CHECK(memory.reads == 4);
    CHECK(native_invalid.address_low == 0);

    // The same offset is not decoded on legacy tiling, and a non-aligned native offset is invalid.
    memory.native = false;
    (void)Tegra::Texture::ReadTextureDescriptor(memory, base, limit, true, limit * 32);
    memory.native = true;
    (void)Tegra::Texture::ReadTextureDescriptor(memory, base, limit, true, limit * 32 + 1);
    CHECK(memory.reads == 4);
}

TEST_CASE("A texture handle past the pool reads as a plain 2D RGBA8 texture",
          "[video_core][texture]") {
    TextureMemory memory;
    const auto entry = Tegra::Texture::ReadTextureDescriptor(memory, 0x1000, 5, false, 6);
    CHECK(memory.reads == 0);
    CHECK(entry.texture_type == Tegra::Texture::TextureType::Texture2D);
    CHECK(entry.format == Tegra::Texture::TextureFormat::A8B8G8R8);
    CHECK(entry.r_type == Tegra::Texture::ComponentType::UNORM);
}
