// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The shader cache v2 reader against damaged copies of a log: every cut-off length, and a few
// thousand copies damaged at random (bit flips, overwrites, zeroed runs, bytes inserted or
// removed, cuts) from a fixed seed. Reading never throws, and every record that ends before the
// first damaged byte is read back as written.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/shader_cache_v2.h"

namespace {
namespace fs = std::filesystem;
namespace V2 = VideoCommon::ShaderCacheV2;

/// The key of the record a carrier record's payload holds
constexpr u64 NestedKey = 0x9999;

struct Written {
    V2::RecordKind kind;
    u16 layout;
    u64 key;
    std::vector<u8> payload;
    size_t offset{};
    size_t framed_end{}; ///< Past the payload's hash, before the padding
};

struct SyntheticLog {
    std::vector<u8> bytes;
    std::vector<Written> records; ///< In the order of the file
    std::vector<u8> nested_payload;
};

using RecordId = std::pair<u16, u64>;
using LiveRecords = std::map<RecordId, const Written*>;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / "nemyu_shader_cache_v2_reader_test";
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::vector<u8> RandomBytes(std::mt19937_64& rng, size_t size) {
    std::vector<u8> bytes(size);
    for (u8& byte : bytes) {
        byte = static_cast<u8>(rng());
    }
    return bytes;
}

/// A log as AppendToLog writes it: graphics, compute and program records with payloads of 0 to
/// 299 bytes, a used list, a record a later one of its key replaces, one a tombstone removes, and
/// a carrier whose payload holds a whole encoded record (read only once the carrier is damaged).
SyntheticLog MakeLog() {
    std::mt19937_64 rng{0x4E454D5955763231ULL};
    SyntheticLog log;
    const auto add = [&log](V2::RecordKind kind, u64 key, std::vector<u8> payload, u16 layout) {
        log.records.push_back(
            {.kind = kind, .layout = layout, .key = key, .payload = std::move(payload)});
    };
    constexpr V2::RecordKind kinds[] = {V2::RecordKind::Graphics, V2::RecordKind::Compute,
                                        V2::RecordKind::Program};
    for (u64 i = 0; i < 40; ++i) {
        add(kinds[i % 3], 0x1000 + i, RandomBytes(rng, rng() % 300), static_cast<u16>(i % 4));
        if (i == 5) {
            add(kinds[3 % 3], 0x1003, RandomBytes(rng, 120), 9);
        } else if (i == 9) {
            add(V2::RecordKind::Tombstone, 0x1004, V2::TombstonePayload(kinds[4 % 3]), 0);
        } else if (i == 20) {
            log.nested_payload = RandomBytes(rng, 40);
            std::vector<u8> nested;
            V2::EncodeRecord(nested, V2::RecordKind::Compute, NestedKey, log.nested_payload);
            // At an 8-byte aligned offset of the file, where reading looks for records
            std::vector<u8> carrier = RandomBytes(rng, 8);
            carrier.insert(carrier.end(), nested.begin(), nested.end());
            const std::vector<u8> tail = RandomBytes(rng, 16);
            carrier.insert(carrier.end(), tail.begin(), tail.end());
            add(V2::RecordKind::Graphics, 0x2000, std::move(carrier), 1);
        } else if (i == 30) {
            add(V2::RecordKind::Used, 0, RandomBytes(rng, 64), 0);
        }
    }

    std::vector<u8> encoded;
    for (Written& record : log.records) {
        record.offset = V2::FileHeaderSize + encoded.size();
        record.framed_end =
            record.offset + V2::RecordHeaderSize + record.payload.size() + V2::RecordHashSize;
        V2::EncodeRecord(encoded, record.kind, record.key, record.payload, record.layout);
    }
    const TempDir dir;
    const fs::path file = dir.path / V2::FileName;
    REQUIRE(V2::AppendToLog(file, encoded) == V2::WriteResult::Written);
    {
        std::ifstream stream(file, std::ios::binary);
        log.bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    REQUIRE(log.bytes.size() == V2::FileHeaderSize + encoded.size());
    return log;
}

/// The records a reader keeps of the first `length` bytes: the later of a key replaces the
/// earlier, a tombstone removes the one it names.
LiveRecords Replay(const SyntheticLog& log, size_t length) {
    LiveRecords live;
    for (const Written& record : log.records) {
        if (record.framed_end > length) {
            break;
        }
        if (record.kind == V2::RecordKind::Tombstone) {
            u16 kind{};
            std::memcpy(&kind, record.payload.data(), sizeof(kind));
            live.erase(RecordId{kind, record.key});
        } else {
            live[RecordId{static_cast<u16>(record.kind), record.key}] = &record;
        }
    }
    return live;
}

bool IsNested(const V2::LogRecord& record) {
    return record.kind == V2::RecordKind::Compute && record.key == NestedKey;
}

/// What reading `size` bytes got wrong, empty when nothing: the stats agree with the records,
/// the payloads lie in the bytes read, every record of `kept` is read back as written, and
/// every record read is one the log holds. With `exact`, no record past `kept` is read.
std::string Check(const SyntheticLog& log, const V2::LogFile& read, size_t size,
                  const LiveRecords& kept, bool exact) {
    if (read.bytes.size() != size || read.stats.file_size != size) {
        return "file size";
    }
    if (read.stats.live != read.live.size() || read.stats.records < read.live.size()) {
        return "live count";
    }
    if (read.stats.dead_bytes > size) {
        return "dead bytes past the file size";
    }
    std::map<RecordId, const V2::LogRecord*> found;
    for (const V2::LogRecord& record : read.live) {
        const u8* const data = record.payload.data();
        if (!record.payload.empty() &&
            (data < read.bytes.data() ||
             data + record.payload.size() > read.bytes.data() + read.bytes.size())) {
            return "payload outside the bytes read";
        }
        if (!found.emplace(RecordId{static_cast<u16>(record.kind), record.key}, &record).second) {
            return "two live records of one key";
        }
        if (IsNested(record)) {
            if (!std::ranges::equal(record.payload, log.nested_payload)) {
                return "changed nested record";
            }
            continue;
        }
        const bool written = std::ranges::any_of(log.records, [&record](const Written& entry) {
            return entry.kind == record.kind && entry.key == record.key &&
                   entry.layout == record.layout &&
                   std::ranges::equal(entry.payload, record.payload);
        });
        if (!written) {
            return "record the log never held, key " + std::to_string(record.key);
        }
        if (exact && !kept.contains(RecordId{static_cast<u16>(record.kind), record.key})) {
            return "record past the cut, key " + std::to_string(record.key);
        }
    }
    for (const auto& [id, written] : kept) {
        const auto it = found.find(id);
        if (it == found.end()) {
            return "lost record, key " + std::to_string(id.second);
        }
        if (it->second->layout != written->layout ||
            !std::ranges::equal(it->second->payload, written->payload)) {
            return "changed record, key " + std::to_string(id.second);
        }
    }
    return {};
}

/// The live records written again under the original header read back the same, with nothing
/// dead (Lichii's fuzz target #1036 checks the same).
std::string CheckRewrite(const SyntheticLog& log, const V2::LogFile& read) {
    std::vector<u8> bytes(log.bytes.begin(), log.bytes.begin() + V2::FileHeaderSize);
    for (const V2::LogRecord& record : read.live) {
        V2::EncodeRecord(bytes, record.kind, record.key, record.payload, record.layout);
    }
    const V2::LogFile again = V2::ParseLog(std::move(bytes));
    if (again.header != V2::HeaderStatus::Valid || again.stats.dead_bytes != 0 ||
        again.live.size() != read.live.size()) {
        return "rewritten log differs";
    }
    for (size_t i = 0; i < read.live.size(); ++i) {
        const V2::LogRecord& before = read.live[i];
        const V2::LogRecord& after = again.live[i];
        if (before.kind != after.kind || before.key != after.key ||
            before.layout != after.layout || !std::ranges::equal(before.payload, after.payload)) {
            return "rewritten record differs";
        }
    }
    return {};
}

} // Anonymous namespace

TEST_CASE("ShaderCacheV2: the reader keeps every record before a cut", "[video_core]") {
    const SyntheticLog log = MakeLog();
    {
        const V2::LogFile read = V2::ParseLog(log.bytes);
        REQUIRE(read.header == V2::HeaderStatus::Valid);
        REQUIRE(read.stats.damaged == 0);
        REQUIRE(read.stats.superseded == 2);
        REQUIRE(read.stats.tombstones == 1);
        REQUIRE(Check(log, read, log.bytes.size(), Replay(log, log.bytes.size()), true).empty());
    }
    for (size_t length = 0; length <= log.bytes.size(); ++length) {
        INFO("cut at " << length);
        V2::LogFile read;
        REQUIRE_NOTHROW(
            read = V2::ParseLog(std::vector<u8>(log.bytes.begin(), log.bytes.begin() + length)));
        const V2::HeaderStatus header = length == 0                    ? V2::HeaderStatus::Empty
                                        : length < V2::FileHeaderSize ? V2::HeaderStatus::Damaged
                                                                       : V2::HeaderStatus::Valid;
        REQUIRE(read.header == header);
        const std::string failure = Check(log, read, length, Replay(log, length), true);
        INFO(failure);
        REQUIRE(failure.empty());
    }
}

TEST_CASE("ShaderCacheV2: the reader survives random damage", "[video_core]") {
    const SyntheticLog log = MakeLog();
    const LiveRecords live = Replay(log, log.bytes.size());
    std::mt19937_64 rng{0x4E454D59555A5A31ULL};
    const auto below = [&rng](size_t bound) { return static_cast<size_t>(rng() % bound); };

    for (int iteration = 0; iteration < 3000; ++iteration) {
        std::vector<u8> bytes = log.bytes;
        const size_t first = below(bytes.size());
        size_t at = first;
        const size_t damages = 1 + below(3);
        for (size_t damage = 0; damage < damages; ++damage) {
            if (damage != 0) {
                if (at >= bytes.size()) {
                    break;
                }
                at += below(bytes.size() - at);
            }
            const size_t run = std::min<size_t>(1 + below(64), bytes.size() - at);
            const auto run_begin = bytes.begin() + static_cast<std::ptrdiff_t>(at);
            const auto run_end = run_begin + static_cast<std::ptrdiff_t>(run);
            switch (below(6)) {
            case 0:
                bytes.resize(at);
                break;
            case 1:
                bytes[at] ^= static_cast<u8>(1U << below(8));
                for (size_t flips = below(8); flips != 0; --flips) {
                    bytes[at + below(bytes.size() - at)] ^= static_cast<u8>(1U << below(8));
                }
                break;
            case 2:
                std::generate(run_begin, run_end, [&rng] { return static_cast<u8>(rng()); });
                break;
            case 3:
                std::fill(run_begin, run_end, u8{0});
                break;
            case 4: {
                const std::vector<u8> inserted = RandomBytes(rng, 1 + below(64));
                bytes.insert(run_begin, inserted.begin(), inserted.end());
                break;
            }
            case 5:
                bytes.erase(run_begin, run_end);
                break;
            }
        }

        LiveRecords kept;
        for (const auto& [id, record] : live) {
            if (record->framed_end <= first) {
                kept.emplace(id, record);
            }
        }
        INFO("iteration " << iteration << ", first damaged byte " << first);
        const size_t size = bytes.size();
        V2::LogFile read;
        REQUIRE_NOTHROW(read = V2::ParseLog(std::move(bytes)));
        std::string failure = Check(log, read, size, kept, false);
        if (failure.empty()) {
            failure = CheckRewrite(log, read);
        }
        INFO(failure);
        REQUIRE(failure.empty());
    }
}
