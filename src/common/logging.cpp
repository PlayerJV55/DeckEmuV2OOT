// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif
#ifdef _WIN32
#include <windows.h> // For OutputDebugStringW
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <boost/algorithm/string/replace.hpp>
#include <fmt/chrono.h>
#include <fmt/ranges.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/literals.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/bounded_threadsafe_queue.h"

namespace Common::Log {

/// @brief A log entry. Log entries are store in a structured format to permit more varied output
/// formatting on different frontends, as well as facilitating filtering and aggregation.
struct Entry {
    char const* message = nullptr;
    size_t message_len = 0;
    std::chrono::microseconds timestamp;
    Class log_class{};
    Level log_level{};
    const char* filename = nullptr;
    const char* function = nullptr;
    uint32_t line_num = 0;
};

namespace {

/// @brief Returns the name of the passed log class as a C-string. Subclasses are separated by periods
/// instead of underscores as in the enumeration.
/// @note GetClassName is a macro defined by Windows.h, grrr...
const char* GetLogClassName(Class log_class) {
    switch (log_class) {
#define CLS(x) case Class::x: return #x;
#define SUB(x, y) case Class::x##_##y: return #x "." #y;
#include "common/log_classes.inc"
#undef CLS
#undef SUB
    default: return "?";
    }
}

/// @brief Returns the name of the passed log level as a C-string.
const char* GetLevelName(Level log_level) {
    switch (log_level) {
#define LVL(x) case Level::x: return #x;
    LVL(Trace)
    LVL(Debug)
    LVL(Info)
    LVL(Warning)
    LVL(Error)
    LVL(Critical)
#undef LVL
    default: return "?";
    }
}

// Some IDEs prefer <file>:<line> instead, so let's just do that :)
std::string FormatLogMessage(const Entry& entry) noexcept {
    if (!entry.filename) return "";
    auto const time_seconds = uint32_t(entry.timestamp.count() / 1000000);
    auto const time_fractional = uint32_t(entry.timestamp.count() % 1000000);
    auto const class_name = GetLogClassName(entry.log_class);
    auto const level_name = GetLevelName(entry.log_level);
    return fmt::format("[{:4d}.{:06d}] {} <{}> {}:{}:{}: {}\n", time_seconds, time_fractional, class_name, level_name, entry.filename, entry.line_num, entry.function, entry.message);
}

template <typename It>
Level GetLevelByName(const It begin, const It end) {
    for (u32 i = 0; i < u32(Level::Count); ++i) {
        const char* level_name = GetLevelName(Level(i));
        if (Common::ComparePartialString(begin, end, level_name))
            return Level(i);
    }
    return Level::Count;
}

template <typename It>
Class GetClassByName(const It begin, const It end) {
    for (u32 i = 0; i < u32(Class::Count); ++i) {
        const char* level_name = GetLogClassName(Class(i));
        if (Common::ComparePartialString(begin, end, level_name))
            return Class(i);
    }
    return Class::Count;
}

template <typename Iterator>
bool ParseFilterRule(Filter& instance, Iterator begin, Iterator end) {
    auto level_separator = std::find(begin, end, ':');
    if (level_separator == end) {
        LOG_ERROR(Log, "Invalid log filter. Must specify a log level after `:`: {}", std::string(begin, end));
        return false;
    }
    const Level level = GetLevelByName(level_separator + 1, end);
    if (level == Level::Count) {
        LOG_ERROR(Log, "Unknown log level in filter: {}", std::string(begin, end));
        return false;
    }
    if (Common::ComparePartialString(begin, level_separator, "*")) {
        instance.class_levels.fill(level);
        return true;
    }
    const Class log_class = GetClassByName(begin, level_separator);
    if (log_class == Class::Count) {
        LOG_ERROR(Log, "Unknown log class in filter: {}", std::string(begin, end));
        return false;
    }
    instance.SetClassLevel(log_class, level);
    return true;
}

/// @brief Trims up to and including the last of ../, ..\, src/, src\ in a string
/// do not be fooled this isn't generating new strings on .rodata :)
constexpr const char* TrimSourcePath(std::string_view source) noexcept {
    const auto rfind = [source](const std::string_view match) {
        return source.rfind(match) == source.npos ? 0 : (source.rfind(match) + match.size());
    };
    auto idx = (std::max)({rfind("src/"), rfind("src\\"), rfind("../"), rfind("..\\")});
    return source.data() + idx;
}

/// @brief Interface for logging backends.
struct Backend {
    virtual ~Backend() noexcept = default;
    virtual void Write(const Entry& entry) noexcept = 0;
    virtual void Flush() noexcept = 0;
};

/// @brief Instead of using fmt::format() just use the system's formatting capabilities directly
struct DirectFormatArgs {
    const char *class_name;
    const char *level_name;
    uint32_t time_seconds;
    uint32_t time_fractional;
};
[[nodiscard]] inline DirectFormatArgs GetDirectFormatArgs(Entry const& entry) noexcept {
    return {
        .class_name = GetLogClassName(entry.log_class),
        .level_name = GetLevelName(entry.log_level),
        .time_seconds = uint32_t(entry.timestamp.count() / 1000000),
        .time_fractional = uint32_t(entry.timestamp.count() % 1000000),
    };
}

/// @brief Backend that writes to stdout and with color
struct ColorConsoleBackend final : public Backend {
#ifdef _WIN32
    explicit ColorConsoleBackend() noexcept {
        console_handle = GetStdHandle(STD_ERROR_HANDLE);
        GetConsoleScreenBufferInfo(console_handle, &original_info);
    }
    ~ColorConsoleBackend() noexcept override {
        SetConsoleTextAttribute(console_handle, original_info.wAttributes);
    }
    void Write(const Entry& entry) noexcept override {
        if (enabled && console_handle != INVALID_HANDLE_VALUE) {
            WORD color = WORD([&entry]() {
                switch (entry.log_level) {
                case Level::Debug: return FOREGROUND_GREEN | FOREGROUND_BLUE; // Cyan
                case Level::Info: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // Bright gray
                case Level::Warning: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
                case Level::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
                case Level::Critical: return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
                default: break;
                }
                return FOREGROUND_INTENSITY; // Grey
            }());
            SetConsoleTextAttribute(console_handle, color);
            // One write per line: MinGW's printf sends console output a character at a time
            // (~1.5 ms a line), which held up the GPU thread enough for games to overwrite
            // command lists it had not read yet.
            const std::string line = FormatLogMessage(entry);
            std::fwrite(line.data(), 1, line.size(), stdout);
        }
    }
    void Flush() noexcept override {}
    CONSOLE_SCREEN_BUFFER_INFO original_info = {};
    HANDLE console_handle = INVALID_HANDLE_VALUE;
    std::atomic_bool enabled = false;
#else // ^^^ Windows vvv POSIX
    explicit ColorConsoleBackend() noexcept {}
    ~ColorConsoleBackend() noexcept override {}
    void Write(const Entry& entry) noexcept override {
        if (enabled) {
            auto const color_str = [&entry]() -> const char* {
                switch (entry.log_level) {
                case Level::Debug: return "[0;36m"; // Cyan
                case Level::Info: return "[0;37m"; // Bright gray
                case Level::Warning: return "[1;33m"; // Bright yellow
                case Level::Error: return "[1;31m"; // Bright red
                case Level::Critical: return "[1;35m"; // Bright magenta
                default: return "[1;30m"; // Grey
                }
            }();
            auto const df = GetDirectFormatArgs(entry);
            // more restrictive, because take for example this simple prelude:
            // [  50.872256] Config <Info> common/settings.cpp:142:LogSettings:
            char buffer[256];
            auto result = fmt::format_to_n(buffer, sizeof(buffer) - 1, "\x1b{}[{:4d}.{:06d}] {} <{}> {}:{}:{}: ", color_str, df.time_seconds, df.time_fractional, df.class_name, df.level_name, entry.filename, entry.line_num, entry.function, entry.message);
            std::fwrite(buffer, 1, (std::min)(sizeof(buffer) - 1, result.size), stdout);
            std::fwrite(entry.message, 1, entry.message_len, stdout);
            std::fwrite("\x1b[0m\n", 1, sizeof("\x1b[0m\n"), stdout);
        }
    }
    void Flush() noexcept override {}
    std::atomic_bool enabled = false;
#endif
};

#ifndef __OPENORBIS__
/// Moves the previous run's log into history/ under its modification time, keeping the newest
/// few. Two quick launches (a crash, then a command-line run, then the GUI) used to rotate a
/// crash log away before anyone could read it.
static void KeepPreviousLog(const std::filesystem::path& old_log) noexcept {
    constexpr size_t KeptLogs = 8;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(old_log, ec)) {
        return;
    }
    const auto history = old_log.parent_path() / "history";
    std::filesystem::create_directories(history, ec);
    const auto written = std::filesystem::last_write_time(old_log, ec);
    if (ec) {
        return;
    }
    const auto mtime = std::chrono::clock_cast<std::chrono::system_clock>(written);
    const auto stamp = fmt::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(mtime));
    std::filesystem::copy_file(old_log, history / fmt::format("nemyu_log-{}.txt", stamp),
                               std::filesystem::copy_options::overwrite_existing, ec);
    std::vector<std::filesystem::path> logs;
    for (const auto& entry : std::filesystem::directory_iterator(history, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".txt") {
            logs.push_back(entry.path());
        }
    }
    if (logs.size() <= KeptLogs) {
        return;
    }
    // The names sort by time.
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + KeptLogs < logs.size(); ++i) {
        std::filesystem::remove(logs[i], ec);
    }
}

/// @brief Backend that writes to a file passed into the constructor
struct FileBackend final : public Backend {
    explicit FileBackend(const std::filesystem::path filename) noexcept {
        auto old_filename = filename;
        old_filename += ".old.txt";
        KeepPreviousLog(old_filename);
        // Existence checks are done within the functions themselves.
        // We don't particularly care if these succeed or not.
        void(FS::RemoveFile(old_filename));
        void(FS::RenameFile(filename, old_filename));
        file.emplace(filename, FS::FileAccessMode::Write, FS::FileType::TextFile);
    }
    ~FileBackend() noexcept override = default;

    void Write(const Entry& entry) noexcept override {
        if (!enabled)
            return;

        auto message = FormatLogMessage(entry);
#ifndef __ANDROID__
        if (Settings::values.censor_username.GetValue()) {
            // This must be a static otherwise it would get checked on EVERY
            // instance of logging an entry...
            static std::string username = []() -> std::string {
                // in order of precedence
                // LOGNAME usually works on UNIX, USERNAME on Windows
                // Some UNIX systems suck and don't use LOGNAME so we also need USER :(
                for (auto const var : { "LOGNAME", "USERNAME", "USER", })
                    if (auto const s = ::getenv(var); s != nullptr)
                        return std::string{s};
                return std::string{};
            }();
            if (!username.empty())
                boost::replace_all(message, username, "user");
        }
#endif
        // Threads log without a shared lock; fwrite locks the stream, these counters are atomic.
        const auto written = file->WriteSpan(std::span<const char>{message.begin(), message.end()});
        const auto total_written =
            bytes_written.fetch_add(written, std::memory_order_relaxed) + written;

        // Option to log each line rather than 4k buffers
        if (Settings::values.log_flush_line.GetValue())
            file->Flush();

        using namespace Common::Literals;
        // Prevent logs from exceeding a set maximum size in the event that log entries are spammed.
        const auto write_limit = Settings::values.extended_logging.GetValue() ? 1_GiB : 100_MiB;
        const bool write_limit_exceeded = total_written > write_limit;
        // Stop writing after the write limit is exceeded.
        // Don't close the file so we can print a stacktrace if necessary
        if (write_limit_exceeded)
            enabled = false;
        // A flush is a synchronous write on the thread that logged, and some games log Errors by
        // the thousand a second: only Critical lines flush at once. Any other line flushes when the
        // last flush is 250 ms old, so the file stays current while lines keep coming.
        const s64 now_us = entry.timestamp.count();
        s64 last_us = last_flush_us.load(std::memory_order_relaxed);
        if (entry.log_level >= Level::Critical || write_limit_exceeded) {
            last_flush_us.store(now_us, std::memory_order_relaxed);
            file->Flush();
        } else if (now_us - last_us >= 250'000 &&
                   last_flush_us.compare_exchange_strong(last_us, now_us,
                                                         std::memory_order_relaxed)) {
            file->Flush();
        }
    }

    void Flush() noexcept override {
        file->Flush();
    }

    /// Appends text and flushes without the stream's lock, which a crashing thread may hold.
    void WriteUnlocked(const char* text, std::size_t size) noexcept {
        std::FILE* const handle = file ? file->GetHandle() : nullptr;
        if (handle == nullptr) {
            return;
        }
#ifdef _WIN32
        _fwrite_nolock(text, 1, size, handle);
        _fflush_nolock(handle);
#else
        std::fwrite(text, 1, size, handle);
        std::fflush(handle);
#endif
    }
private:
    std::optional<FS::IOFile> file;
    std::atomic<std::size_t> bytes_written = 0;
    std::atomic<s64> last_flush_us = 0;
    std::atomic_bool enabled = true;
};
#endif
#ifdef _WIN32
/// @brief Backend that writes to Visual Studio's output window
struct DebuggerBackend final : public Backend {
    explicit DebuggerBackend() noexcept = default;
    ~DebuggerBackend() noexcept override = default;
    void Write(const Entry& entry) noexcept override {
        // Only a debugger reads this; EDEN_LOG_DEBUG_OUTPUT=1 keeps it on for DebugView.
        static const bool always = [] {
            const char* const value = std::getenv("EDEN_LOG_DEBUG_OUTPUT");
            return value != nullptr && value[0] == '1';
        }();
        if (always || ::IsDebuggerPresent()) {
            ::OutputDebugStringW(UTF8ToUTF16W(FormatLogMessage(entry)).c_str());
        }
    }
    void Flush() noexcept override {}
};
#endif
#ifdef __ANDROID__
/// @brief Backend that writes to the Android logcat
struct LogcatBackend : public Backend {
    explicit LogcatBackend() noexcept = default;
    ~LogcatBackend() noexcept override = default;
    void Write(const Entry& entry) noexcept override {
        android_LogPriority android_log_priority = [&]() {
            switch (entry.log_level) {
            case Level::Debug: return ANDROID_LOG_DEBUG;
            case Level::Info: return ANDROID_LOG_INFO;
            case Level::Warning: return ANDROID_LOG_WARN;
            case Level::Error: return ANDROID_LOG_ERROR;
            case Level::Critical: return ANDROID_LOG_FATAL;
            case Level::Count:
            case Level::Trace: return ANDROID_LOG_VERBOSE;
            }
        }();
        auto const df = GetDirectFormatArgs(entry);
        __android_log_print(android_log_priority, "YuzuNative", "%s %s:%u:%s: %s", df.class_name, entry.filename, entry.line_num, entry.function, entry.message);
    }
    void Flush() noexcept override {}
};
#endif

/// @brief Static state as a singleton.
struct Impl {
    // Well, I mean it's the default constructor!
    explicit Impl() noexcept : filter(Level::Trace) {}

    template<typename F>
    void ForEachBackend(F&& lambda) noexcept {
        lambda(static_cast<Backend&>(color_console_backend));
#ifndef __OPENORBIS__
        if (file_backend)
            lambda(static_cast<Backend&>(*file_backend));
#endif
#ifdef _WIN32
        lambda(static_cast<Backend&>(debugger_backend));
#endif
#ifdef __ANDROID__
        lambda(static_cast<Backend&>(lc_backend));
#endif
    }

    Filter filter;
    ColorConsoleBackend color_console_backend{};
#ifndef __OPENORBIS__
    std::optional<FileBackend> file_backend;
#endif
#ifdef _WIN32
    DebuggerBackend debugger_backend{};
#endif
#ifdef __ANDROID__
    LogcatBackend lc_backend{};
#endif
    std::chrono::steady_clock::time_point time_origin{std::chrono::steady_clock::now()};
};
} // Anonymous namespace

void Filter::ParseFilterString(std::string_view filter_view) {
    auto clause_begin = filter_view.cbegin();
    while (clause_begin < filter_view.cend()) {
        auto clause_end = std::find(clause_begin, filter_view.cend(), ' ');
        // If clause isn't empty
        if (clause_end != clause_begin) {
            ParseFilterRule(*this, clause_begin, clause_end);
        }
        if (clause_end != filter_view.cend()) {
            // Skip over the whitespace
            ++clause_end;
        }
        clause_begin = clause_end;
    }
}

// Constructor shall NOT depend upon Settings() or whatever
// it's ran at global static ctor() time... so BE CAREFUL MFER!
static std::optional<Common::Log::Impl> logging_instance{};

void Initialize() {
    if (logging_instance) {
        LOG_WARNING(Log, "Reinitializing logging backend");
    } else {
        logging_instance.emplace();
        logging_instance->filter.ParseFilterString(Settings::values.log_filter.GetValue());
#ifndef __OPENORBIS__
        using namespace Common::FS;
        const auto& log_dir = GetEdenPath(EdenPath::LogDir);
        void(CreateDir(log_dir));
        logging_instance->file_backend.emplace(log_dir / LOG_FILE);
#endif
    }
}

void Start() {
}

void Stop() {
    if (logging_instance)
        logging_instance->ForEachBackend([](Backend& backend) { backend.Flush(); });
}

#ifdef _WIN32
// A crash ended the log wherever its buffer happened to be flushed, with no exception code or
// address: Windows Error Reporting kept those. The filter logs them, then lets WER go on.
static LONG WINAPI LogUnhandledException(EXCEPTION_POINTERS* exception) {
    static std::atomic_bool logged{};
    if (logged.exchange(true) || !logging_instance || !logging_instance->file_backend) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char text[4096];
    std::size_t length = 0;
    const auto append = [&text, &length](const char* format, auto... args) {
        const int written = std::snprintf(text + length, sizeof(text) - length, format, args...);
        if (written > 0) {
            length = (std::min)(length + static_cast<std::size_t>(written), sizeof(text) - 1);
        }
    };
    const auto append_address = [&append](const char* label, const void* address) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "unknown";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(address), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* const base_name = std::strrchr(name, '\\');
        append("  %s %s+%#llx\n", label, base_name ? base_name + 1 : name,
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                               reinterpret_cast<uintptr_t>(module)));
    };
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    append("[ crash ] Unhandled exception %#lx on thread %lu (info %#llx %#llx)\n",
           record.ExceptionCode, GetCurrentThreadId(),
           static_cast<unsigned long long>(
               record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0),
           static_cast<unsigned long long>(
               record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0));
    append_address("at", record.ExceptionAddress);
    void* frames[32];
    const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        append_address("frame", frames[i]);
    }
    logging_instance->file_backend->WriteUnlocked(text, length);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void InstallCrashHandler() {
#ifdef _WIN32
    const char* const value = std::getenv("EDEN_NX2_CRASH_LOG");
    if (value == nullptr || value[0] != '0') {
        SetUnhandledExceptionFilter(LogUnhandledException);
    }
#endif
}

bool ShouldLogRepeat(u64 count) noexcept {
    static const bool unlimited = [] {
        const char* const value = std::getenv("EDEN_LOG_UNLIMITED");
        return value != nullptr && value[0] == '1';
    }();
    return count < 8 || (count & (count - 1)) == 0 || unlimited ||
           ::Settings::values.use_debug_asserts.GetValue();
}

void SetGlobalFilter(const Filter& filter) {
    if (logging_instance)
        logging_instance->filter = filter;
}

void SetColorConsoleBackendEnabled(bool enabled) {
    if (logging_instance)
        logging_instance->color_console_backend.enabled = enabled;
}

void FmtLogMessageImpl(Class log_class, Level log_level, const char* filename, unsigned int line_num, const char* function, fmt::string_view format, const fmt::format_args& args) {
    if (logging_instance && logging_instance->filter.CheckMessage(log_class, log_level)) {
        auto const flush = ::Settings::values.log_flush_line.GetValue();
        char buffer[BUFSIZ];
        auto result = fmt::vformat_to_n(buffer, sizeof(buffer) - 1, format, args);
        Entry e{
            .message = nullptr,
            .message_len = 0,
            .timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - logging_instance->time_origin),
            .log_class = log_class,
            .log_level = log_level,
            .filename = TrimSourcePath(filename),
            .function = function,
            .line_num = line_num,
        };
        if (result.size <= sizeof(buffer) - 1) {
            buffer[(std::min)(result.size, sizeof(buffer) - 1)] = '\0';
            e.message = buffer;
            e.message_len = (std::min)(result.size, sizeof(buffer) - 1);
            logging_instance->ForEachBackend([=](Backend& backend) {
                backend.Write(e);
                if (flush) backend.Flush();
            });
        } else {
            std::string s = fmt::vformat(format, args);
            e.message = s.c_str();
            e.message_len = s.size();
            logging_instance->ForEachBackend([=](Backend& backend) {
                backend.Write(e);
                if (flush) backend.Flush();
            });
        }
    }
}
} // namespace Common::Log
