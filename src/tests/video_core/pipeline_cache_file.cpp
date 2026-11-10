// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_cache_file.h"

namespace {
namespace fs = std::filesystem;
namespace CacheFile = Vulkan::PipelineCacheFile;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / "nemyu_pipeline_cache_file_test";
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void WriteHeader(const fs::path& path, std::string_view magic, u32 version) {
    std::ofstream file(path, std::ios::binary);
    file.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
}
} // Anonymous namespace

TEST_CASE("PipelineCacheFile reads the version of a cache header", "[video_core]") {
    const TempDir dir;
    REQUIRE_FALSE(CacheFile::ReadVersion(dir.path / "missing.bin"));
    WriteHeader(dir.path / "other.bin", "notcache", 33);
    REQUIRE_FALSE(CacheFile::ReadVersion(dir.path / "other.bin"));
    WriteHeader(dir.path / "lichii.bin", "yuzucach", 53);
    REQUIRE(CacheFile::ReadVersion(dir.path / "lichii.bin") == 53U);
}

TEST_CASE("PipelineCacheFile merges only caches of its own version and Lichii's", "[video_core]") {
    const TempDir dir;
    const fs::path game = dir.path / "game";
    WriteHeader(dir.path / "old.bin", "yuzucach", 18);
    const auto foreign = CacheFile::Merge(dir.path / "old.bin", game);
    REQUIRE(foreign.added == 0);
    REQUIRE_FALSE(foreign.error.empty());
    REQUIRE_FALSE(fs::exists(game / "vulkan.bin"));

    WriteHeader(dir.path / "lichii.bin", "yuzucach", 53);
    REQUIRE_FALSE(CacheFile::Inspect(dir.path / "lichii.bin").damaged);
    const auto lichii = CacheFile::Merge(dir.path / "lichii.bin", game);
    REQUIRE(lichii.error.empty());
    REQUIRE(lichii.added == 0);

    WriteHeader(dir.path / "empty.bin", "yuzucach", CacheFile::CurrentVersion);
    const auto contents = CacheFile::Inspect(dir.path / "empty.bin");
    REQUIRE(contents.graphics + contents.compute + contents.mesh == 0);
    REQUIRE_FALSE(contents.damaged);
    const auto empty = CacheFile::Merge(dir.path / "empty.bin", game);
    REQUIRE(empty.error.empty());
    REQUIRE(empty.added == 0);
}

// Run by hand with EDEN_TEST_PIPELINE_CACHE set to a vulkan.bin of this build's version.
TEST_CASE("PipelineCacheFile merges a real cache once", "[.][video_core]") {
    const char* source = std::getenv("EDEN_TEST_PIPELINE_CACHE");
    REQUIRE(source != nullptr);
    const TempDir dir;
    const auto contents = CacheFile::Inspect(source);
    const u32 total = contents.graphics + contents.compute + contents.mesh;
    REQUIRE(total != 0);
    REQUIRE_FALSE(contents.damaged);
    const auto first = CacheFile::Merge(source, dir.path);
    REQUIRE(first.error.empty());
    REQUIRE(first.added + first.duplicates == total);
    const auto second = CacheFile::Merge(source, dir.path);
    REQUIRE(second.added == 0);
    REQUIRE(second.duplicates == total);
    const auto merged = CacheFile::Inspect(dir.path / "vulkan.bin");
    const auto merged_mesh = CacheFile::Inspect(dir.path / "vulkan_mesh.bin");
    REQUIRE(merged.graphics + merged.compute + merged_mesh.mesh + merged.mesh == first.added);
}
