// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "common/cityhash.h"
#include "common/fs/file_lock.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nx2_title_defaults.h"
#include "common/thread.h"
#include "video_core/shader_cache_v2.h"

namespace VideoCommon::ShaderCacheV2 {
namespace {

/// Records queued past this while writes fail are dropped
constexpr size_t MaxQueuedBytes = 256u << 20;
/// A burst of new pipelines is written at once
constexpr std::chrono::milliseconds BatchDelay{500};
/// Writes that fail in a row before the queued records are dropped
constexpr u32 MaxFailedWrites = 20;

template <typename T>
T Load(std::span<const u8> bytes, u64 offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Store(u8* out, T value) {
    std::memcpy(out, &value, sizeof(T));
}

u32 Check32(const u8* data, size_t size) {
    return static_cast<u32>(Common::CityHash64(reinterpret_cast<const char*>(data), size));
}

u64 PayloadHash(u16 kind, u64 key, std::span<const u8> payload) {
    return Hash64(payload, key ^ (u64{kind} << 48));
}

u64 UnixNow() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
}

std::array<u8, FileHeaderSize> EncodeFileHeader(u64 created_unix) {
    std::array<u8, FileHeaderSize> header{};
    std::memcpy(header.data(), FileMagic.data(), FileMagic.size());
    Store<u32>(header.data() + 8, FormatVersion);
    Store<u32>(header.data() + 12, FileHeaderSize);
    Store<u64>(header.data() + 16, created_unix);
    Store<u32>(header.data() + 24, Check32(header.data(), 24));
    return header;
}

/// The header of a file of at least FileHeaderSize bytes
HeaderStatus ReadFileHeader(std::span<const u8> bytes, u64& created_unix) {
    if (std::memcmp(bytes.data(), FileMagic.data(), FileMagic.size()) != 0 ||
        Load<u32>(bytes, 24) != Check32(bytes.data(), 24)) {
        return HeaderStatus::Damaged;
    }
    if (Load<u32>(bytes, 8) != FormatVersion) {
        return HeaderStatus::OtherVersion;
    }
    created_unix = Load<u64>(bytes, 16);
    return Load<u32>(bytes, 12) == FileHeaderSize ? HeaderStatus::Valid : HeaderStatus::Damaged;
}

struct RecordHeader {
    u16 kind;
    u16 layout;
    u32 payload_size;
    u64 key;
};

/// The record header at `offset` (which leaves room for one), when it is intact
std::optional<RecordHeader> ReadRecordHeader(std::span<const u8> bytes, u64 offset) {
    if (Load<u32>(bytes, offset) != RecordMagic ||
        Load<u32>(bytes, offset + 20) != Check32(bytes.data() + offset, 20)) {
        return std::nullopt;
    }
    const RecordHeader header{
        .kind = Load<u16>(bytes, offset + 4),
        .layout = Load<u16>(bytes, offset + 6),
        .payload_size = Load<u32>(bytes, offset + 8),
        .key = Load<u64>(bytes, offset + 12),
    };
    if (header.kind == 0 || header.payload_size > MaxPayloadSize) {
        return std::nullopt;
    }
    return header;
}

/// The next 8-byte aligned offset from `offset` holding an intact record header, else the size
u64 FindRecord(std::span<const u8> bytes, u64 offset) {
    for (; offset + RecordHeaderSize <= bytes.size(); offset += RecordAlignment) {
        if (ReadRecordHeader(bytes, offset)) {
            return offset;
        }
    }
    return bytes.size();
}

u64 RecordSize(u64 payload_size) {
    return (RecordHeaderSize + payload_size + RecordHashSize + RecordAlignment - 1) /
           RecordAlignment * RecordAlignment;
}

struct KindKey {
    u16 kind;
    u64 key;
    bool operator==(const KindKey&) const = default;
};

struct KindKeyHash {
    size_t operator()(const KindKey& value) const noexcept {
        return static_cast<size_t>(value.key ^ (u64{value.kind} * 0x9e3779b97f4a7c15ULL));
    }
};

/// Fills `log.live` and `log.stats` from `log.bytes`, past the file header
void ParseRecords(LogFile& log) {
    const std::span<const u8> bytes{log.bytes};
    LogStats& stats = log.stats;
    struct Entry {
        LogRecord record;
        u64 size;
        bool live;
    };
    std::vector<Entry> entries;
    std::unordered_map<KindKey, size_t, KindKeyHash> latest;
    const auto drop = [&](const KindKey& target) {
        if (const auto it = latest.find(target); it != latest.end()) {
            entries[it->second].live = false;
            latest.erase(it);
        }
    };
    const auto skip_to_next = [&](u64 offset) {
        ++stats.damaged;
        const u64 next = FindRecord(bytes, offset + RecordAlignment);
        stats.skipped_bytes += next - offset;
        return next;
    };
    u64 offset = FileHeaderSize;
    while (offset < bytes.size()) {
        const u64 left = bytes.size() - offset;
        if (left < RecordHeaderSize) {
            stats.torn_tail_bytes = left;
            break;
        }
        const std::optional<RecordHeader> header = ReadRecordHeader(bytes, offset);
        if (!header) {
            offset = skip_to_next(offset);
            continue;
        }
        // The padding of the last record may be cut off.
        const u64 framed = u64{RecordHeaderSize} + header->payload_size + RecordHashSize;
        if (framed > left) {
            if (FindRecord(bytes, offset + RecordAlignment) == bytes.size()) {
                stats.torn_tail_bytes = left;
                break;
            }
            // Cut off by a crash, then appended to
            offset = skip_to_next(offset);
            continue;
        }
        const auto payload = bytes.subspan(offset + RecordHeaderSize, header->payload_size);
        if (Load<u64>(bytes, offset + RecordHeaderSize + header->payload_size) !=
            PayloadHash(header->kind, header->key, payload)) {
            offset = skip_to_next(offset);
            continue;
        }
        const u64 size = std::min(RecordSize(header->payload_size), left);
        ++stats.records;
        if (static_cast<RecordKind>(header->kind) == RecordKind::Tombstone) {
            ++stats.tombstones;
            stats.dead_bytes += size;
            if (payload.size() == sizeof(u16)) {
                drop({Load<u16>(payload, 0), header->key});
            }
        } else {
            const KindKey id{header->kind, header->key};
            drop(id);
            latest.emplace(id, entries.size());
            entries.push_back({
                .record = {.kind = static_cast<RecordKind>(header->kind),
                           .layout = header->layout,
                           .key = header->key,
                           .offset = offset,
                           .payload = payload},
                .size = size,
                .live = true,
            });
        }
        offset += size;
    }
    for (const Entry& entry : entries) {
        if (entry.live) {
            log.live.push_back(entry.record);
        } else {
            ++stats.superseded;
            stats.dead_bytes += entry.size;
        }
    }
    stats.live = log.live.size();
    stats.dead_bytes += stats.skipped_bytes + stats.torn_tail_bytes;
}

bool WriteFile(const std::filesystem::path& file, std::span<const u8> bytes,
               std::ios::openmode mode) {
    std::ofstream out(file, std::ios::binary | mode);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return !out.fail();
}

} // Anonymous namespace

bool Enabled() {
    return Common::Nx2::Setting("EDEN_NX2_SHADER_CACHE_V2", "1") != "0";
}

u64 Hash64(std::span<const u8> bytes, u64 seed) {
    return Common::CityHash64WithSeed(reinterpret_cast<const char*>(bytes.data()), bytes.size(),
                                      seed);
}

void EncodeRecord(std::vector<u8>& out, RecordKind kind, u64 key, std::span<const u8> payload,
                  u16 layout) {
    std::array<u8, RecordHeaderSize> header{};
    Store<u32>(header.data(), RecordMagic);
    Store<u16>(header.data() + 4, static_cast<u16>(kind));
    Store<u16>(header.data() + 6, layout);
    Store<u32>(header.data() + 8, static_cast<u32>(payload.size()));
    Store<u64>(header.data() + 12, key);
    Store<u32>(header.data() + 20, Check32(header.data(), 20));
    std::array<u8, RecordHashSize> hash{};
    Store<u64>(hash.data(), PayloadHash(static_cast<u16>(kind), key, payload));
    const size_t end = out.size() + RecordSize(payload.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), hash.begin(), hash.end());
    out.resize(end);
}

std::vector<u8> TombstonePayload(RecordKind kind) {
    std::vector<u8> payload(sizeof(u16));
    Store<u16>(payload.data(), static_cast<u16>(kind));
    return payload;
}

LogFile ParseLog(std::vector<u8> bytes) {
    LogFile log;
    log.bytes = std::move(bytes);
    log.stats.file_size = log.bytes.size();
    if (log.bytes.empty()) {
        log.header = HeaderStatus::Empty;
        return log;
    }
    if (log.bytes.size() < FileHeaderSize) {
        log.header = HeaderStatus::Damaged;
        log.stats.torn_tail_bytes = log.bytes.size();
        log.stats.dead_bytes = log.bytes.size();
        return log;
    }
    log.header = ReadFileHeader(log.bytes, log.created_unix);
    if (log.header != HeaderStatus::OtherVersion) {
        ParseRecords(log);
    }
    return log;
}

LogFile ReadLogFile(const std::filesystem::path& file) {
    std::vector<u8> bytes;
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (stream.is_open()) {
        const std::streamoff size = stream.tellg();
        if (size > 0) {
            bytes.resize(static_cast<size_t>(size));
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(bytes.data()), size);
            bytes.resize(static_cast<size_t>(stream.gcount()));
        }
    }
    return ParseLog(std::move(bytes));
}

bool HasLog(const std::filesystem::path& file) {
    std::array<u8, FileHeaderSize> header{};
    std::ifstream stream(file, std::ios::binary);
    u64 created_unix{};
    return stream.read(reinterpret_cast<char*>(header.data()), header.size()) &&
           ReadFileHeader(header, created_unix) == HeaderStatus::Valid;
}

std::filesystem::path LockPath(const std::filesystem::path& file) {
    auto lock = file;
    lock += ".lock";
    return lock;
}

WriteResult AppendToLog(const std::filesystem::path& file, std::span<const u8> records) {
    // Without locks on this file system (neither held nor busy) it goes on unlocked.
    const Common::FS::FileLock lock{LockPath(file)};
    if (lock.Busy()) {
        return WriteResult::Busy;
    }
    std::error_code error;
    const auto file_size = std::filesystem::file_size(file, error);
    const u64 size = error ? 0 : static_cast<u64>(file_size);
    std::vector<u8> bytes;
    if (size < FileHeaderSize) {
        // New, or cut off within its header before any record was written
        const auto header = EncodeFileHeader(UnixNow());
        bytes.assign(header.begin(), header.end());
        bytes.insert(bytes.end(), records.begin(), records.end());
        if (WriteFile(file, bytes, std::ios::trunc)) {
            return WriteResult::Written;
        }
        LOG_ERROR(Common_Filesystem, "Failed to create shader cache {}",
                  Common::FS::PathToUTF8String(file));
        return WriteResult::Failed;
    }
    std::array<u8, FileHeaderSize> header{};
    {
        std::ifstream source(file, std::ios::binary);
        if (!source.read(reinterpret_cast<char*>(header.data()), header.size())) {
            return WriteResult::Failed;
        }
    }
    u64 created_unix{};
    switch (ReadFileHeader(header, created_unix)) {
    case HeaderStatus::OtherVersion:
        return WriteResult::OtherVersion;
    case HeaderStatus::Damaged: {
        // The records check themselves and stay
        std::fstream target(file, std::ios::binary | std::ios::in | std::ios::out);
        const auto fresh = EncodeFileHeader(UnixNow());
        target.write(reinterpret_cast<const char*>(fresh.data()), fresh.size());
        target.close();
        if (target.fail()) {
            return WriteResult::Failed;
        }
        break;
    }
    case HeaderStatus::Valid:
    case HeaderStatus::Empty:
        break;
    }
    // A record cut off by a crash leaves the end off a record boundary.
    bytes.resize((RecordAlignment - size % RecordAlignment) % RecordAlignment);
    bytes.insert(bytes.end(), records.begin(), records.end());
    if (WriteFile(file, bytes, std::ios::app)) {
        return WriteResult::Written;
    }
    // Cut off what was written (a full disk), so that the next records follow complete ones.
    std::filesystem::resize_file(file, size, error);
    LOG_ERROR(Common_Filesystem, "Failed to append to shader cache {}",
              Common::FS::PathToUTF8String(file));
    return WriteResult::Failed;
}

WriteResult CompactLog(const std::filesystem::path& file) {
    const Common::FS::FileLock lock{LockPath(file)};
    if (lock.Busy()) {
        return WriteResult::Busy;
    }
    const LogFile log = ReadLogFile(file);
    if (log.header == HeaderStatus::OtherVersion) {
        return WriteResult::OtherVersion;
    }
    if (log.header == HeaderStatus::Empty) {
        return WriteResult::Written;
    }
    const auto header = EncodeFileHeader(log.created_unix != 0 ? log.created_unix : UnixNow());
    std::vector<u8> bytes(header.begin(), header.end());
    for (const LogRecord& record : log.live) {
        EncodeRecord(bytes, record.kind, record.key, record.payload, record.layout);
    }
    auto temporary = file;
    temporary += ".tmp";
    std::error_code error;
    if (WriteFile(temporary, bytes, std::ios::trunc)) {
        std::filesystem::rename(temporary, file, error);
        if (!error) {
            LOG_INFO(Common_Filesystem, "Shader cache {} compacted from {} to {} bytes",
                     Common::FS::PathToUTF8String(file), log.bytes.size(), bytes.size());
            return WriteResult::Written;
        }
    }
    LOG_ERROR(Common_Filesystem, "Failed to compact shader cache {}: {}",
              Common::FS::PathToUTF8String(file),
              error ? error.message() : "the new file can't be written");
    std::filesystem::remove(temporary, error);
    return WriteResult::Failed;
}

Writer::Writer(std::filesystem::path file_)
    : file{std::move(file_)}, thread{[this](std::stop_token stop) { Run(stop); }} {}

Writer::~Writer() {
    thread.request_stop();
    if (thread.joinable()) {
        thread.join();
    }
    if (queued_records != 0) {
        LOG_WARNING(Common_Filesystem, "{} shader cache records were not written to {}",
                    queued_records, Common::FS::PathToUTF8String(file));
    }
}

void Writer::Append(RecordKind kind, u64 key, std::span<const u8> payload, u16 layout) {
    std::vector<u8> record;
    EncodeRecord(record, kind, key, payload, layout);
    {
        std::scoped_lock lock{mutex};
        if (other_version || queued.size() + record.size() > MaxQueuedBytes) {
            return;
        }
        queued.insert(queued.end(), record.begin(), record.end());
        ++queued_records;
    }
    work.notify_one();
}

void Writer::Compact() {
    {
        std::scoped_lock lock{mutex};
        compact = true;
    }
    work.notify_one();
}

void Writer::Run(std::stop_token stop) {
    Common::SetCurrentThreadName("ShaderCacheV2");
    std::unique_lock lock{mutex};
    while (!stop.stop_requested()) {
        work.wait(lock, stop, [this] { return queued_records != 0 || compact; });
        // Waits for the rest of a burst, or between tries of a write that failed
        (void)work.wait_for(lock, stop, BatchDelay, [] { return false; });
        WriteQueued(lock);
    }
    WriteQueued(lock);
}

void Writer::WriteQueued(std::unique_lock<std::mutex>& lock) {
    if (queued_records != 0) {
        std::vector<u8> batch = std::exchange(queued, {});
        const u64 count = std::exchange(queued_records, 0);
        lock.unlock();
        const WriteResult result = AppendToLog(file, batch);
        lock.lock();
        if (result == WriteResult::OtherVersion) {
            LOG_ERROR(Common_Filesystem,
                      "Not writing to shader cache {}: another format version made it",
                      Common::FS::PathToUTF8String(file));
            other_version = true;
            queued.clear();
            queued_records = 0;
            return;
        }
        if (result == WriteResult::Failed || result == WriteResult::Busy) {
            // Ahead of the records queued meanwhile, so that later records stay later. Another
            // process holds the lock only while it writes (Busy), so that isn't a failure.
            if ((result == WriteResult::Busy || ++failed_writes < MaxFailedWrites) &&
                batch.size() + queued.size() <= MaxQueuedBytes) {
                batch.insert(batch.end(), queued.begin(), queued.end());
                queued = std::move(batch);
                queued_records += count;
            } else {
                LOG_ERROR(Common_Filesystem, "{} shader cache records dropped: {} can't be written",
                          count, Common::FS::PathToUTF8String(file));
            }
            return;
        }
        failed_writes = 0;
    }
    if (compact && queued_records == 0) {
        compact = false;
        lock.unlock();
        (void)CompactLog(file);
        lock.lock();
    }
}

} // namespace VideoCommon::ShaderCacheV2
