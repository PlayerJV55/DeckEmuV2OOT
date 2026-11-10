// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "common/container/unordered_map.h"

#include "common/assert.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#ifdef __ANDROID__
#include "common/fs/fs_android.h"
#endif
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

#ifdef _WIN32
#include <shlobj.h> // Used in GetExeDirectory()
#else
#include <cstdlib>     // Used in Get(Home/Data)Directory()
#include <fcntl.h>     // Used in LegacyMigrationLock
#include <pwd.h>       // Used in GetHomeDirectory()
#include <sys/file.h>  // Used in LegacyMigrationLock
#include <sys/types.h> // Used in GetHomeDirectory()
#include <unistd.h>    // Used in GetDataDirectory()
#endif

#ifdef __APPLE__
#include <sys/param.h> // Used in GetBundleDirectory()

// CFURL contains __attribute__ directives that gcc does not know how to parse, so we need to just
// ignore them if we're not using clang. The macro is only used to prevent linking against
// functions that don't exist on older versions of macOS, and the worst case scenario is a linker
// error, so this is perfectly safe, just inconvenient.
#ifndef __clang__
#define availability(...)
#endif
#include <CoreFoundation/CFBundle.h> // Used in GetBundleDirectory()
#include <CoreFoundation/CFString.h> // Used in GetBundleDirectory()
#include <CoreFoundation/CFURL.h>    // Used in GetBundleDirectory()
#ifdef availability
#undef availability
#endif
#endif

#ifndef MAX_PATH
#ifdef _WIN32
// This is the maximum number of UTF-16 code units permissible in Windows file paths
#define MAX_PATH 260
#else
// This is the maximum number of UTF-8 code units permissible in all other OSes' file paths
#define MAX_PATH 1024
#endif
#endif

namespace Common::FS {

namespace fs = std::filesystem;

namespace {

struct LegacyMigrationMessage {
    bool failed;
    std::string text;
};

// Filled before logging starts and flushed by LogLegacyDataMigration.
std::vector<LegacyMigrationMessage> legacy_migration_log;

#ifndef __ANDROID__
struct LegacyRoot {
    fs::path legacy;
    fs::path current;
};

// Kept in the new data directory while the copy runs, so an interrupted copy resumes on the next
// launch instead of leaving a half-filled directory behind.
constexpr const char* LegacyMigrationMarker = ".legacy_copy_incomplete";

// Installed titles (nand/user/Contents), logs, crash dumps, screenshots and dumps stay behind.
constexpr std::array<const char*, 14> LegacyDataSubdirs{
    KEYS_DIR,        NAND_DIR "/system", NAND_DIR "/user/save", NAND_DIR "/user/saveMeta",
    SDMC_DIR,        LOAD_DIR,           SHADER_DIR,            AMIIBO_DIR,
    PLAY_TIME_DIR,   POST_PRESET_DIR,    POST_SHADER_DIR,       ICONS_DIR,
    TAS_DIR,         LOSSLESS_DIR,
};

// Deep enough for any real layout, but stops a link that points back at a parent folder.
constexpr int MaxLegacyCopyDepth = 32;

// Each folder is opened on its own, so links and junctions (like saves linked to Ryujinx) are
// followed, and a folder that cannot be opened counts as one failure instead of ending the walk.
void CopyDirLenient(const fs::path& from, const fs::path& to, int depth, size_t& copied,
                    size_t& failed) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (!ec) {
        fs::directory_iterator it{from, ec};
        for (; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
            std::error_code entry_ec;
            const fs::path target = to / it->path().filename();
            if (it->is_directory(entry_ec)) {
                if (depth < MaxLegacyCopyDepth) {
                    CopyDirLenient(it->path(), target, depth + 1, copied, failed);
                } else {
                    ++failed;
                }
            } else if (it->is_regular_file(entry_ec)) {
                fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, entry_ec);
                copied += entry_ec ? 0 : 1;
            }
            failed += entry_ec ? 1 : 0;
        }
    }
    failed += ec ? 1 : 0;
}

/// Copies a directory tree file by file, skipping whatever fails instead of stopping there.
void CopyTreeLenient(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        return;
    }
    size_t copied = 0;
    size_t failed = 0;
    CopyDirLenient(from, to, 0, copied, failed);
    legacy_migration_log.push_back(
        {failed != 0, fmt::format("Copied {} files from {} to {}, {} failed", copied,
                                  PathToUTF8String(from), PathToUTF8String(to), failed)});
}

bool LegacyPathCharsMatch(char a, char b) {
    if (IsDirSeparator(a) && IsDirSeparator(b)) {
        return true;
    }
#ifdef _WIN32
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
#else
    return a == b;
#endif
}

/// Points absolute paths into a legacy root at the new root. Only the last component differs, so
/// the rest of each match keeps the spelling it had (slashes, drive letter case).
bool ReplaceLegacyRoot(std::string& text, std::string_view legacy_root) {
    constexpr std::string_view legacy_name = LEGACY_EDEN_DIR;
    constexpr std::string_view new_name = EDEN_DIR;
    if (!legacy_root.ends_with(legacy_name)) {
        return false;
    }
    const size_t keep = legacy_root.size() - legacy_name.size();
    bool changed = false;
    for (size_t pos = 0; pos + legacy_root.size() <= text.size(); ++pos) {
        if (!std::equal(legacy_root.begin(), legacy_root.end(), text.begin() + pos,
                        LegacyPathCharsMatch)) {
            continue;
        }
        const size_t end = pos + legacy_root.size();
        if (end < text.size() && !IsDirSeparator(text[end]) && text[end] != '\r' &&
            text[end] != '\n' && text[end] != '"') {
            continue;
        }
        text.replace(pos + keep, legacy_name.size(), new_name);
        pos += keep + new_name.size() - 1;
        changed = true;
    }
    return changed;
}

void RewriteLegacyConfigPaths(const fs::path& config_dir,
                              const std::array<const LegacyRoot*, 3>& roots) {
    std::error_code ec;
    fs::recursive_directory_iterator it{config_dir, fs::directory_options::skip_permission_denied,
                                        ec};
    for (; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || it->path().extension() != ".ini") {
            continue;
        }
        std::string text = ReadStringFromFile(it->path(), FileType::BinaryFile);
        bool changed = false;
        for (const LegacyRoot* root : roots) {
            changed |= ReplaceLegacyRoot(text, PathToUTF8String(root->legacy));
        }
        if (!changed) {
            continue;
        }
        const bool written =
            WriteStringToFile(it->path(), FileType::BinaryFile, text) == text.size();
        legacy_migration_log.push_back(
            {!written, fmt::format("{} legacy data paths in {}",
                                   written ? "Rewrote" : "Failed to rewrite",
                                   PathToUTF8String(it->path()))});
    }
}

/// Makes a second instance wait while another one copies, instead of taking the marker for an
/// interrupted copy. The marker itself is never locked, since it has to outlive a crash.
class LegacyMigrationLock {
public:
    explicit LegacyMigrationLock([[maybe_unused]] const fs::path& legacy) {
#ifdef _WIN32
        handle = CreateMutexW(nullptr, FALSE, L"Local\\NemyuLegacyDataMigration");
        if (handle != nullptr) {
            // A mutex abandoned by a killed owner is still handed over.
            const DWORD result = WaitForSingleObject(handle, INFINITE);
            owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
#else
        fd = ::open(legacy.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            while (::flock(fd, LOCK_EX) != 0 && errno == EINTR) {
            }
        }
#endif
    }

    ~LegacyMigrationLock() {
#ifdef _WIN32
        if (owned) {
            ReleaseMutex(handle);
        }
        if (handle != nullptr) {
            CloseHandle(handle);
        }
#else
        if (fd >= 0) {
            ::close(fd);
        }
#endif
    }

    LegacyMigrationLock(const LegacyMigrationLock&) = delete;
    LegacyMigrationLock& operator=(const LegacyMigrationLock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle = nullptr;
    bool owned = false;
#else
    int fd = -1;
#endif
};

/// The data directory used to be named after Eden. Copy what Nemyu needs from it once, and leave
/// the original alone since a separate Eden install may still use it.
void MigrateLegacyDataDir(const LegacyRoot& data, const LegacyRoot& config,
                          const LegacyRoot& cache) {
    std::error_code ec;
    const fs::path marker = data.current / LegacyMigrationMarker;
    if (!fs::is_directory(data.legacy, ec) && !fs::exists(marker, ec)) {
        return;
    }
    LegacyMigrationLock lock{data.legacy};
    const bool resume = fs::exists(marker, ec);
    // On XDG layouts config lives apart from data, so a kept config dir also counts as migrated.
    if (!resume && (fs::exists(data.current, ec) || fs::exists(config.current, ec) ||
                    !fs::is_directory(data.legacy, ec))) {
        return;
    }
    fs::create_directories(data.current, ec);
    if (ec) {
        legacy_migration_log.push_back(
            {true, fmt::format("Could not create {}, legacy data was not copied",
                               PathToUTF8String(data.current))});
        return;
    }
    if (!resume) {
        void(NewFile(marker));
    }
    legacy_migration_log.push_back(
        {false, fmt::format("{} the one-time copy of legacy data from {}",
                            resume ? "Resuming" : "Starting", PathToUTF8String(data.legacy))});

    // This runs from the path manager's constructor, so nothing may escape it.
    try {
        for (const char* subdir : LegacyDataSubdirs) {
            CopyTreeLenient(data.legacy / subdir, data.current / subdir);
        }
        CopyTreeLenient(cache.legacy, cache.current);
        // A config dir linked to the legacy one would otherwise get Eden's own files rewritten.
        if (!fs::equivalent(config.legacy, config.current, ec)) {
            CopyTreeLenient(config.legacy, config.current);
            RewriteLegacyConfigPaths(config.current, {&data, &config, &cache});
        }
    } catch (const std::exception& e) {
        legacy_migration_log.push_back(
            {true, fmt::format("Copying legacy data stopped early: {}", e.what())});
    }

    fs::remove(marker, ec);
}
#endif

} // Anonymous namespace

/**
 * The PathManagerImpl is a singleton allowing to manage the mapping of
 * EdenPath enums to real filesystem paths.
 * This class provides 2 functions: GetEdenPathImpl and SetEdenPathImpl.
 * These are used by GetEdenPath and SetEdenPath respectively to get or modify
 * the path mapped by the EdenPath enum.
 */
class PathManagerImpl {
public:
    static PathManagerImpl& GetInstance() {
        static PathManagerImpl path_manager_impl;

        return path_manager_impl;
    }

    PathManagerImpl(const PathManagerImpl&) = delete;
    PathManagerImpl& operator=(const PathManagerImpl&) = delete;

    PathManagerImpl(PathManagerImpl&&) = delete;
    PathManagerImpl& operator=(PathManagerImpl&&) = delete;

    [[nodiscard]] const fs::path& GetEdenPathImpl(EdenPath eden_path) {
        return eden_paths.at(eden_path);
    }

    [[nodiscard]] const fs::path& GetLegacyPathImpl(EmuPath legacy_path) {
        return legacy_paths.at(legacy_path);
    }

    void CreateEdenPaths() {
        std::for_each(eden_paths.begin(), eden_paths.end(), [](auto &path) {
            void(FS::CreateDirs(path.second));
        });
    }

    void SetEdenPathImpl(EdenPath eden_path, const fs::path& new_path) {
        eden_paths.insert_or_assign(eden_path, new_path);
    }

    void SetLegacyPathImpl(EmuPath legacy_path, const fs::path& new_path) {
        legacy_paths.insert_or_assign(legacy_path, new_path);
    }

    /// In non-android devices, the current directory will first search for "user"
    /// if such directory (and it must be a directory) is found, that takes priority
    /// over the global configuration directory (in other words, portable directories
    /// take priority over the global ones, always)
    /// On Android, the behaviour is to look for the current directory only.
    void Reinitialize(fs::path eden_path = {}) {
        fs::path eden_path_cache;
        fs::path eden_path_config;
#ifdef _WIN32
        // User directory takes priority over global %AppData% directory
        eden_path = GetExeDirectory() / PORTABLE_DIR;
        if (!Exists(eden_path) || !IsDir(eden_path)) {
            eden_path = GetAppDataRoamingDirectory() / EDEN_DIR;
            const fs::path legacy_path = GetAppDataRoamingDirectory() / LEGACY_EDEN_DIR;
            MigrateLegacyDataDir({legacy_path, eden_path},
                                 {legacy_path / CONFIG_DIR, eden_path / CONFIG_DIR},
                                 {legacy_path / CACHE_DIR, eden_path / CACHE_DIR});
        }
        eden_path_cache = eden_path / CACHE_DIR;
        eden_path_config = eden_path / CONFIG_DIR;
#define LEGACY_PATH(titleName, upperName) GenerateLegacyPath(EmuPath::titleName##Dir, GetAppDataRoamingDirectory() / upperName##_DIR); \
        GenerateLegacyPath(EmuPath::titleName##ConfigDir, GetAppDataRoamingDirectory() / upperName##_DIR / CONFIG_DIR); \
        GenerateLegacyPath(EmuPath::titleName##CacheDir, GetAppDataRoamingDirectory() / upperName##_DIR / CACHE_DIR);
        LEGACY_PATH(Citron, CITRON)
        LEGACY_PATH(Sudachi, SUDACHI)
        LEGACY_PATH(Yuzu, YUZU)
        LEGACY_PATH(Suyu, SUYU)
#undef LEGACY_PATH
#elif __ANDROID__
        ASSERT(!eden_path.empty());
        eden_path_cache = eden_path / CACHE_DIR;
        eden_path_config = eden_path / CONFIG_DIR;
#else
        eden_path = GetCurrentDir() / PORTABLE_DIR;
        if (!Exists(eden_path) || !IsDir(eden_path)) {
            eden_path = GetDataDirectory("XDG_DATA_HOME") / EDEN_DIR;
            eden_path_cache = GetDataDirectory("XDG_CACHE_HOME") / EDEN_DIR;
            eden_path_config = GetDataDirectory("XDG_CONFIG_HOME") / EDEN_DIR;
            MigrateLegacyDataDir(
                {GetDataDirectory("XDG_DATA_HOME") / LEGACY_EDEN_DIR, eden_path},
                {GetDataDirectory("XDG_CONFIG_HOME") / LEGACY_EDEN_DIR, eden_path_config},
                {GetDataDirectory("XDG_CACHE_HOME") / LEGACY_EDEN_DIR, eden_path_cache});
        } else {
            eden_path_cache = eden_path / CACHE_DIR;
            eden_path_config = eden_path / CONFIG_DIR;
        }
#define LEGACY_PATH(titleName, upperName) GenerateLegacyPath(EmuPath::titleName##Dir, GetDataDirectory("XDG_DATA_HOME") / upperName##_DIR); \
        GenerateLegacyPath(EmuPath::titleName##ConfigDir, GetDataDirectory("XDG_CONFIG_HOME") / upperName##_DIR); \
        GenerateLegacyPath(EmuPath::titleName##CacheDir, GetDataDirectory("XDG_CACHE_HOME") / upperName##_DIR);
        LEGACY_PATH(Citron, CITRON)
        LEGACY_PATH(Sudachi, SUDACHI)
        LEGACY_PATH(Yuzu, YUZU)
        LEGACY_PATH(Suyu, SUYU)
#undef LEGACY_PATH
#endif
        // data
        GenerateEdenPath(EdenPath::EdenDir, eden_path);
        GenerateEdenPath(EdenPath::AmiiboDir, eden_path / AMIIBO_DIR);
        GenerateEdenPath(EdenPath::CrashDumpsDir, eden_path / CRASH_DUMPS_DIR);
        GenerateEdenPath(EdenPath::DumpDir, eden_path / DUMP_DIR);
        GenerateEdenPath(EdenPath::KeysDir, eden_path / KEYS_DIR);
        GenerateEdenPath(EdenPath::LoadDir, eden_path / LOAD_DIR);
        GenerateEdenPath(EdenPath::LogDir, eden_path / LOG_DIR);
        GenerateEdenPath(EdenPath::LosslessDir, eden_path / LOSSLESS_DIR);
        GenerateEdenPath(EdenPath::NANDDir, eden_path / NAND_DIR);
        GenerateEdenPath(EdenPath::PlayTimeDir, eden_path / PLAY_TIME_DIR);
        GenerateEdenPath(EdenPath::PostPresetDir, eden_path / POST_PRESET_DIR);
        GenerateEdenPath(EdenPath::PostShaderDir, eden_path / POST_SHADER_DIR);
        GenerateEdenPath(EdenPath::SaveDir, eden_path / NAND_DIR);
        GenerateEdenPath(EdenPath::ScreenshotsDir, eden_path / SCREENSHOTS_DIR);
        GenerateEdenPath(EdenPath::SDMCDir, eden_path / SDMC_DIR);
        GenerateEdenPath(EdenPath::TASDir, eden_path / TAS_DIR);
        GenerateEdenPath(EdenPath::IconsDir, eden_path / ICONS_DIR);
        GenerateEdenPath(EdenPath::StreamlineDir, eden_path / STREAMLINE_DIR);
        // config
        GenerateEdenPath(EdenPath::ConfigDir, eden_path_config);
        // cache
        GenerateEdenPath(EdenPath::CacheDir, eden_path_cache);
        GenerateEdenPath(EdenPath::ShaderDir, eden_path_cache / SHADER_DIR);
#ifdef _WIN32
        GenerateLegacyPath(EmuPath::RyujinxDir, GetAppDataRoamingDirectory() / RYUJINX_DIR);
#else
        // In Ryujinx's infinite wisdom, it places EVERYTHING in the config directory on UNIX
        // This is incredibly stupid and violates a million XDG standards, but whatever
        GenerateLegacyPath(EmuPath::RyujinxDir, GetDataDirectory("XDG_CONFIG_HOME") / RYUJINX_DIR);
#endif

    }

private:
    PathManagerImpl() {
        Reinitialize();
    }

    ~PathManagerImpl() = default;

    void GenerateEdenPath(EdenPath eden_path, const fs::path& new_path) {
        // Defer path creation
        SetEdenPathImpl(eden_path, new_path);
    }

    void GenerateLegacyPath(EmuPath legacy_path, const fs::path& new_path) {
        SetLegacyPathImpl(legacy_path, new_path);
    }

    ::Common::unordered_map<EdenPath, fs::path> eden_paths;
    ::Common::unordered_map<EmuPath, fs::path> legacy_paths;
};

bool ValidatePath(const fs::path& path) {
    if (path.empty()) {
        LOG_ERROR(Common_Filesystem, "Input path is empty, path={}", PathToUTF8String(path));
        return false;
    }

#ifdef _WIN32
    if (path.u16string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#else
    if (path.u8string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#endif

    return true;
}

fs::path ConcatPath(const fs::path& first, const fs::path& second) {
    const bool second_has_dir_sep = IsDirSeparator(second.u8string().front());

    if (!second_has_dir_sep) {
        return (first / second).lexically_normal();
    }

    fs::path concat_path = first;
    concat_path += second;

    return concat_path.lexically_normal();
}

fs::path ConcatPathSafe(const fs::path& base, const fs::path& offset) {
    const auto concatenated_path = ConcatPath(base, offset);

    if (!IsPathSandboxed(base, concatenated_path)) {
        return base;
    }

    return concatenated_path;
}

bool IsPathSandboxed(const fs::path& base, const fs::path& path) {
    const auto base_string = RemoveTrailingSeparators(base.lexically_normal()).u8string();
    const auto path_string = RemoveTrailingSeparators(path.lexically_normal()).u8string();

    if (path_string.size() < base_string.size()) {
        return false;
    }

    return base_string.compare(0, base_string.size(), path_string, 0, base_string.size()) == 0;
}

bool IsDirSeparator(char character) {
    return character == '/' || character == '\\';
}

bool IsDirSeparator(char8_t character) {
    return character == u8'/' || character == u8'\\';
}

fs::path RemoveTrailingSeparators(const fs::path& path) {
    if (path.empty()) {
        return path;
    }

    auto string_path = path.u8string();

    while (IsDirSeparator(string_path.back())) {
        string_path.pop_back();
    }

    return fs::path{string_path};
}

void SetAppDirectory(const std::string& app_directory) {
    PathManagerImpl::GetInstance().Reinitialize(app_directory);
}

const fs::path& GetEdenPath(EdenPath eden_path) {
    return PathManagerImpl::GetInstance().GetEdenPathImpl(eden_path);
}

const std::filesystem::path& GetLegacyPath(EmuPath legacy_path) {
    return PathManagerImpl::GetInstance().GetLegacyPathImpl(legacy_path);
}

std::string GetEdenPathString(EdenPath eden_path) {
    return PathToUTF8String(GetEdenPath(eden_path));
}

std::string GetLegacyPathString(EmuPath legacy_path) {
    return PathToUTF8String(GetLegacyPath(legacy_path));
}

void SetEdenPath(EdenPath eden_path, const fs::path& new_path) {
    auto& instance = PathManagerImpl::GetInstance();
    if (FS::IsDir(new_path)) {
        instance.SetEdenPathImpl(eden_path, new_path);
    } else {
        LOG_ERROR(Common_Filesystem, "Filesystem object at new_path={} is not a directory", PathToUTF8String(new_path));
    }
}

void CreateEdenPaths() {
    PathManagerImpl::GetInstance().CreateEdenPaths();
}

void LogLegacyDataMigration() {
    for (const auto& [failed, text] : legacy_migration_log) {
        if (failed) {
            LOG_WARNING(Common_Filesystem, "{}", text);
        } else {
            LOG_INFO(Common_Filesystem, "{}", text);
        }
    }
    legacy_migration_log.clear();
}

#ifdef _WIN32

fs::path GetExeDirectory() {
    wchar_t exe_path[MAX_PATH];

    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        LOG_ERROR(Common_Filesystem,
                  "Failed to get the path to the executable of the current process");
    }

    return fs::path{exe_path}.parent_path();
}

fs::path GetAppDataRoamingDirectory() {
    PWSTR appdata_roaming_path = nullptr;

    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata_roaming_path);

    auto fs_appdata_roaming_path = fs::path{appdata_roaming_path};

    CoTaskMemFree(appdata_roaming_path);

    if (fs_appdata_roaming_path.empty()) {
        LOG_ERROR(Common_Filesystem, "Failed to get the path to the %APPDATA% directory");
    }

    return fs_appdata_roaming_path;
}

#else

fs::path GetHomeDirectory() {
    const char* home_env_var = getenv("HOME");

    if (home_env_var) {
        return fs::path{home_env_var};
    }

    LOG_INFO(Common_Filesystem,
             "$HOME is not defined in the environment variables, "
             "attempting to query passwd to get the home path of the current user");

    const auto* pw = getpwuid(getuid());

    if (!pw) {
        LOG_ERROR(Common_Filesystem, "Failed to get the home path of the current user");
        return {};
    }

    return fs::path{pw->pw_dir};
}

fs::path GetDataDirectory(const std::string& env_name) {
    const char* data_env_var = getenv(env_name.c_str());

    if (data_env_var) {
        return fs::path{data_env_var};
    }

    if (env_name == "XDG_DATA_HOME") {
        return GetHomeDirectory() / ".local/share";
    } else if (env_name == "XDG_CACHE_HOME") {
        return GetHomeDirectory() / ".cache";
    } else if (env_name == "XDG_CONFIG_HOME") {
        return GetHomeDirectory() / ".config";
    }

    return {};
}

#endif

#ifdef __APPLE__

fs::path GetBundleDirectory() {
    char app_bundle_path[MAXPATHLEN];

    // Get the main bundle for the app
    CFURLRef bundle_ref = CFBundleCopyBundleURL(CFBundleGetMainBundle());
    CFStringRef bundle_path = CFURLCopyFileSystemPath(bundle_ref, kCFURLPOSIXPathStyle);

    CFStringGetFileSystemRepresentation(bundle_path, app_bundle_path, sizeof(app_bundle_path));

    CFRelease(bundle_ref);
    CFRelease(bundle_path);

    return fs::path{app_bundle_path};
}

#endif

// vvvvvvvvvv Deprecated vvvvvvvvvv //

std::string_view RemoveTrailingSlash(std::string_view path) {
    if (path.empty()) {
        return path;
    }

    if (path.back() == '\\' || path.back() == '/') {
        path.remove_suffix(1);
        return path;
    }

    return path;
}

template <typename F>
static void ForEachPathComponent(std::string_view filename, F&& cb) {
    const char* component_begin = filename.data();
    const char* const end = component_begin + filename.size();
    for (const char* it = component_begin; it != end; ++it) {
        const char c = *it;
        if (c == '\\' || c == '/') {
            if (component_begin != it) {
                cb(std::string_view{component_begin, it});
            }
            component_begin = it + 1;
        }
    }
    if (component_begin != end) {
        cb(std::string_view{component_begin, end});
    }
}

std::vector<std::string_view> SplitPathComponents(std::string_view filename) {
    std::vector<std::string_view> components;
    ForEachPathComponent(filename, [&](auto component) { components.emplace_back(component); });

    return components;
}

std::vector<std::string> SplitPathComponentsCopy(std::string_view filename) {
    std::vector<std::string> components;
    ForEachPathComponent(filename, [&](auto component) { components.emplace_back(component); });

    return components;
}

std::string SanitizePath(std::string_view path_, DirectorySeparator directory_separator) {
    std::string path(path_);
#ifdef __ANDROID__
    if (Android::IsContentUri(path)) {
        return path;
    }
#endif // __ANDROID__

    char type1 = directory_separator == DirectorySeparator::BackwardSlash ? '/' : '\\';
    char type2 = directory_separator == DirectorySeparator::BackwardSlash ? '\\' : '/';

    if (directory_separator == DirectorySeparator::PlatformDefault) {
#ifdef _WIN32
        type1 = '/';
        type2 = '\\';
#endif
    }

    std::replace(path.begin(), path.end(), type1, type2);

    auto start = path.begin();
#ifdef _WIN32
    // allow network paths which start with a double backslash (e.g. \\server\share)
    if (start != path.end())
        ++start;
#endif
    path.erase(std::unique(start, path.end(),
                           [type2](char c1, char c2) { return c1 == type2 && c2 == type2; }),
               path.end());

    std::string root;
    std::string_view components{path};
    bool drive_relative = false;

#ifdef _WIN32
    const bool network = path.size() > 1 && path[0] == type2 && path[1] == type2;
    const bool drive =
        path.size() > 1 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';

    if (network) {
        root.assign(2, type2);
        components.remove_prefix(2);
    } else if (drive) {
        root.assign(path.data(), 2);
        components.remove_prefix(2);
        if (!components.empty() && components.front() == type2) {
            root += type2;
            components.remove_prefix(1);
        } else {
            drive_relative = true;
        }
    }
#endif

    if (root.empty() && !components.empty() && components.front() == type2) {
        root += type2;
        components.remove_prefix(1);
    }

    const auto path_parts = SplitPathComponents(components);
    std::size_t root_component_count = 0;
#ifdef _WIN32
    if (network) {
        root_component_count = 2;

        const auto is_unc = [](std::string_view part) {
            return part.size() == 3 && (part[0] == 'U' || part[0] == 'u') &&
                   (part[1] == 'N' || part[1] == 'n') && (part[2] == 'C' || part[2] == 'c');
        };
        if (path_parts.size() >= 2 && path_parts[0] == "?" && is_unc(path_parts[1])) {
            root_component_count = 4;
        }
    }
#endif

    std::vector<std::string_view> parts;
    for (std::size_t i = 0; i < path_parts.size(); ++i) {
        const auto part = path_parts[i];
        if (i < root_component_count) {
            parts.push_back(part);
        } else if (part.empty() || part == ".") {
            continue;
        } else if (part == "..") {
            if (parts.size() > root_component_count) {
                parts.pop_back();
            }
        } else {
            parts.push_back(part);
        }
    }

    const std::size_t root_length = root.size();
    std::string resolved = std::move(root);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0 || (!resolved.empty() && resolved.back() != type2 && !drive_relative))
            resolved += type2;
        resolved.append(parts[i].data(), parts[i].size());
    }

    path = std::move(resolved);

    if (!path.empty() && path.size() == root_length) {
        return path;
    }
    return std::string(RemoveTrailingSlash(path));
}

std::string GetParentPath(std::string_view path) {
    if (path.empty()) {
        return std::string(path);
    }

#ifdef __ANDROID__
    if (path[0] != '/') {
        std::string path_string{path};
        return FS::Android::GetParentDirectory(path_string);
    }
#endif
    const auto name_bck_index = path.rfind('\\');
    const auto name_fwd_index = path.rfind('/');
    std::size_t name_index;

    if (name_bck_index == std::string_view::npos || name_fwd_index == std::string_view::npos) {
        name_index = (std::min)(name_bck_index, name_fwd_index);
    } else {
        name_index = (std::max)(name_bck_index, name_fwd_index);
    }

    return std::string(path.substr(0, name_index));
}

std::string_view GetPathWithoutTop(std::string_view path) {
    if (path.empty()) {
        return path;
    }

    while (path[0] == '\\' || path[0] == '/') {
        path.remove_prefix(1);
        if (path.empty()) {
            return path;
        }
    }

    const auto name_bck_index = path.find('\\');
    const auto name_fwd_index = path.find('/');
    return path.substr((std::min)(name_bck_index, name_fwd_index) + 1);
}

} // namespace Common::FS
