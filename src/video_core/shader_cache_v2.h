// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/polyfill_thread.h"

/// Shader cache v2 (EDEN_NX2_SHADER_CACHE_V2): a title's Vulkan pipelines in one append-only log
/// of framed, checked records, after Lichii's (#1036) without its job scheduler and driver
/// binaries.
///
/// File header (32 bytes): "NMSHCACH", u32 format version, u32 header size, u64 creation time
/// (Unix seconds), u32 check of the bytes before it, u32 zero.
/// Record, at an 8-byte aligned offset: u32 magic, u16 kind, u16 layout, u32 payload size, u64 key,
/// u32 check of the bytes before it; the payload; its 64-bit hash seeded with the kind and key;
/// zeros up to the next 8-byte boundary.
///
/// The latest record of a (kind, key) wins, and a tombstone removes the earlier record of its key
/// and of the kind its payload names. A damaged record costs only itself: reading goes on at the
/// next intact record header, and a record cut off at the end is left out.
namespace VideoCommon::ShaderCacheV2 {

/// The log of a title, in its shader cache directory
inline constexpr std::string_view FileName{"vulkan_log.bin"};

/// EDEN_NX2_SHADER_CACHE_V2 (on by default): a title's Vulkan pipelines and used list are kept in
/// its log instead of vulkan.bin, vulkan_mesh.bin and vulkan_used.bin, which the log takes in and
/// never changes. =0 goes back to those files as they were.
[[nodiscard]] bool Enabled();

inline constexpr std::array<char, 8> FileMagic{'N', 'M', 'S', 'H', 'C', 'A', 'C', 'H'};
inline constexpr u32 FormatVersion = 1;
inline constexpr u32 FileHeaderSize = 32;
inline constexpr u32 RecordMagic = 0x52434D4E; // "NMCR"
inline constexpr u32 RecordHeaderSize = 24;
inline constexpr u32 RecordHashSize = 8;
inline constexpr u32 RecordAlignment = 8;
/// A larger payload is taken for a damaged size
inline constexpr u32 MaxPayloadSize = 64u << 20;

enum class RecordKind : u16 {
    /// A pipeline record as vulkan.bin holds one, keyed by its key's hash without the driver
    Graphics = 2,
    /// A compute pipeline record as vulkan.bin holds one, keyed by its hash
    Compute = 3,
    Tombstone = 4,
    /// The used pipeline list as vulkan_used.bin holds it, key 0
    Used = 5,
    /// An older cache file the log took records from (u64 size, s64 write time), keyed by the
    /// hash of its name
    Migrated = 6,
    /// A native program lowered at its first bind (NativePrograms, EDEN_NX2_NATIVE_PROGRAMS), keyed
    /// by NativePrograms::Key, its payload format in the layout. Older builds pass over it.
    Program = 7,
};

struct LogRecord {
    RecordKind kind;
    u16 layout;
    u64 key;
    u64 offset;                  ///< Where the record begins in the file
    std::span<const u8> payload; ///< Into the bytes that were read
};

struct LogStats {
    u64 file_size{};
    u64 records{};    ///< Intact records, tombstones included
    u64 live{};       ///< Records no later record replaces or removes
    u64 superseded{}; ///< Replaced by a later record of their kind and key, or removed
    u64 tombstones{};
    u64 damaged{};    ///< Damaged record headers and payloads, each passed over
    u64 skipped_bytes{};
    u64 torn_tail_bytes{}; ///< A record cut off at the end of the file
    u64 dead_bytes{};      ///< Bytes a compaction would remove
};

enum class HeaderStatus {
    Valid,
    Empty,        ///< No bytes at all
    Damaged,      ///< Not a valid header; the records check themselves and are read
    OtherVersion, ///< Another format version's file, never changed by this build
};

/// A log read into memory. The live records point into `bytes`.
struct LogFile {
    HeaderStatus header{HeaderStatus::Empty};
    u64 created_unix{};
    std::vector<u8> bytes;
    std::vector<LogRecord> live; ///< In the order of the file
    LogStats stats;

    LogFile() = default;
    LogFile(LogFile&&) noexcept = default;
    LogFile& operator=(LogFile&&) noexcept = default;
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
};

[[nodiscard]] u64 Hash64(std::span<const u8> bytes, u64 seed = 0);

/// Appends one framed record to `out`, which must end at an 8-byte boundary of the file
void EncodeRecord(std::vector<u8>& out, RecordKind kind, u64 key, std::span<const u8> payload,
                  u16 layout = 0);

/// The payload of a tombstone for the record of this kind (its key is the record's key)
[[nodiscard]] std::vector<u8> TombstonePayload(RecordKind kind);

/// Reads a log from its bytes. Never throws on its contents.
[[nodiscard]] LogFile ParseLog(std::vector<u8> bytes);

/// Reads a whole log (ParseLog). Never throws on its contents; a missing file reads as empty.
[[nodiscard]] LogFile ReadLogFile(const std::filesystem::path& file);

/// Whether `file` begins with an intact log header of this format version. Reads only the header.
[[nodiscard]] bool HasLog(const std::filesystem::path& file);

enum class WriteResult {
    Written,
    Failed,       ///< The file is as it was
    OtherVersion, ///< The file is another format version's and stays as it is
    Busy,         ///< Another process is writing the file; it is as it was
};

/// The file whose lock (Common::FS::FileLock) is held while a log is written, so that emulators
/// running at once never append to it together or compact it while another appends.
[[nodiscard]] std::filesystem::path LockPath(const std::filesystem::path& file);

/// Appends encoded records (EncodeRecord). The file and its header are made when missing, a
/// damaged header is written again, and a record cut off by a crash is padded to the next record
/// boundary first.
[[nodiscard]] WriteResult AppendToLog(const std::filesystem::path& file,
                                      std::span<const u8> records);

/// Writes the log again with its live records only, to a temporary file renamed over it.
[[nodiscard]] WriteResult CompactLog(const std::filesystem::path& file);

/// The one thread that writes a log. Records are queued from any thread and written in order, a
/// burst at a time; one that fails to be written waits for the next write. What is queued when
/// it is destroyed is written first.
class Writer {
public:
    explicit Writer(std::filesystem::path file);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void Append(RecordKind kind, u64 key, std::span<const u8> payload, u16 layout = 0);

    /// Compacts the log (CompactLog) once the records queued now are written
    void Compact();

private:
    void Run(std::stop_token stop);
    /// Called with `lock` held; releases it while writing.
    void WriteQueued(std::unique_lock<std::mutex>& lock);

    const std::filesystem::path file;
    std::mutex mutex;
    std::condition_variable_any work;
    std::vector<u8> queued;
    u64 queued_records{};
    bool compact{};
    bool other_version{};
    u32 failed_writes{};
    std::jthread thread;
};

} // namespace VideoCommon::ShaderCacheV2
