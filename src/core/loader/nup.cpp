// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/loader/nup.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

namespace {

using FileSys::NupContainer;

// NACP stores 16 uncompressed language entries; map BCP-47 tags onto them.
struct LanguageSlot {
    std::string_view tag;
    std::size_t index;
};

constexpr std::array<LanguageSlot, 18> LanguageSlots{{
    {"en-us", 0},   {"en-gb", 1},  {"ja", 2},     {"fr", 3},     {"de", 4},
    {"es-419", 5},  {"es", 6},     {"it", 7},     {"nl", 8},     {"fr-ca", 9},
    {"pt", 10},     {"ru", 11},    {"ko", 12},    {"zh-hant", 13}, {"zh-hans", 14},
    {"pt-br", 15},  {"pl", 16},    {"th", 17},
}};

void CopyNameToEntry(FileSys::LanguageEntry& entry, std::string_view name) {
    const std::size_t length = std::min(name.size(), entry.application_name.size() - 1);
    std::memcpy(entry.application_name.data(), name.data(), length);
    entry.application_name[length] = '\0';
}

void CopyDeveloperToEntry(FileSys::LanguageEntry& entry, std::string_view developer) {
    const std::size_t length = std::min(developer.size(), entry.developer_name.size() - 1);
    std::memcpy(entry.developer_name.data(), developer.data(), length);
    entry.developer_name[length] = '\0';
}

std::unique_ptr<FileSys::NACP> BuildNacp(const NupContainer::PackageInfo& base,
                                         const std::string& version) {
    // A package without a title name is still named after the dump it was made from.
    const std::string& effective_title =
        !base.title_name.empty() ? base.title_name : base.source_dump;
    if (effective_title.empty() && base.localized_titles.empty() && base.publisher.empty() &&
        version.empty()) {
        return nullptr;
    }
    auto nacp = std::make_unique<FileSys::NACP>();
    nacp->raw.supported_languages = static_cast<FileSys::SupportedLanguage>(0x3FFFF);

    for (const auto& [lang, name] : base.localized_titles) {
        for (const auto& slot : LanguageSlots) {
            // The uncompressed RawNACP only stores 16 entries; Polish/Thai exist
            // only in the compressed format, so they fall back to the default.
            if (slot.index < 16 && slot.tag == lang) {
                CopyNameToEntry(nacp->raw.language_entries.language_entries[slot.index], name);
            }
        }
    }
    // Slot 0 is en-us; an exact en-us localized title wins over the default title (SPEC 5.1).
    auto& english = nacp->raw.language_entries.language_entries[0];
    if (!effective_title.empty() && english.application_name[0] == '\0') {
        CopyNameToEntry(english, effective_title);
    }
    if (!base.publisher.empty()) {
        for (auto& entry : nacp->raw.language_entries.language_entries) {
            CopyDeveloperToEntry(entry, base.publisher);
        }
    }
    if (!version.empty()) {
        const std::size_t length = std::min(version.size(), nacp->raw.version_string.size() - 1);
        std::memcpy(nacp->raw.version_string.data(), version.data(), length);
        nacp->raw.version_string[length] = '\0';
    }
    nacp->raw.save_data_owner_id = base.title_id;

    nacp->language_entries.resize(16);
    std::memcpy(nacp->language_entries.data(), nacp->raw.language_entries.language_entries.data(),
                sizeof(nacp->raw.language_entries.language_entries));
    return nacp;
}

std::string DecodeVersion(u32 version) {
    if (version == 0) {
        return {};
    }
    return fmt::format("{}.{}.{}", version >> 16, (version >> 8) & 0xFF, version & 0xFF);
}

bool IsAddonDisabled(u64 title_id, std::string_view name) {
    const auto it = Settings::values.disabled_addons.find(title_id);
    return it != Settings::values.disabled_addons.end() &&
           std::find(it->second.begin(), it->second.end(), name) != it->second.end();
}

bool IsUpdateDisabled(u64 title_id) {
    return IsAddonDisabled(title_id, "Update");
}

// A standalone-update container is a patch (SPEC §3.1.2): it boots only
// against its base package. Look for a companion base .nup next to the file
// (matching title ID) and return it together with its base package index.
// Another game's base is never used, since its content would not match the update.
std::unique_ptr<FileSys::NupContainer> FindCompanionBase(
    const FileSys::VirtualFile& nup_file, u64 update_title_id,
    std::optional<std::size_t>* out_base_index) {
    *out_base_index = std::nullopt;
    const auto dir = nup_file->GetContainingDirectory();
    if (dir == nullptr) {
        return nullptr;
    }
    for (const auto& candidate : dir->GetFiles()) {
        const std::string name = candidate->GetName();
        if (name == nup_file->GetName() || name.size() < 4) {
            continue;
        }
        const std::string suffix = name.substr(name.size() - 4);
        if (suffix != ".nup" && suffix != ".NUP") {
            continue;
        }
        // Only the package records are read until a candidate carries this title's base, so a
        // folder full of other games' containers is not parsed on every load and list refresh.
        const auto peeked = FileSys::NupContainer::PeekBaseTitleId(candidate);
        if (!peeked || (*peeked != update_title_id && (update_title_id & ~0x800ULL) != *peeked)) {
            continue;
        }
        auto container = std::make_unique<FileSys::NupContainer>(candidate);
        if (container->GetStatus() != ResultStatus::Success) {
            continue;
        }
        const auto base_index = container->FindBasePackage();
        if (!base_index || container->GetPackages()[*base_index].title_id != *peeked ||
            container->GetExeFSDir(*base_index) == nullptr) {
            continue;
        }
        *out_base_index = base_index;
        return container;
    }
    return nullptr;
}

} // namespace

AppLoader_NUP::AppLoader_NUP(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), nup(std::make_unique<NupContainer>(file)) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return;
    }

    const auto base_index = nup->FindBasePackage();
    auto update_index = nup->FindBestUpdatePackage();
    // The update turned off in the game's add-ons, when there is a base to boot without it
    if (base_index && update_index && nup->GetExeFSDir(*base_index) != nullptr &&
        IsUpdateDisabled(nup->GetPackages()[*base_index].title_id)) {
        LOG_INFO(Loader, "NUP update is disabled in the game's add-ons; booting the base");
        update_index.reset();
    }
    selected_update_index = update_index;

    // The program comes from the base package. An update-only container
    // (standalone_update, SPEC §3.1.2) is a patch: it boots only against its
    // base package, so look for a companion base .nup next to this file.
    std::optional<std::size_t> program_index;
    std::optional<std::size_t> exefs_index;
    bool program_from_base_nup = false;
    if (base_index && nup->GetExeFSDir(*base_index) != nullptr) {
        program_index = base_index;
        exefs_index = update_index && nup->GetExeFSDir(*update_index) != nullptr
                          ? update_index
                          : base_index;
    } else if (update_index && nup->GetExeFSDir(*update_index) != nullptr) {
        const auto& update = nup->GetPackages()[*update_index];
        LOG_INFO(Loader, "NUP container is an update-only container (standalone_update)");
        base_nup = FindCompanionBase(file, update.title_id, &base_nup_index);
        if (base_nup && base_nup_index) {
            const auto& base = base_nup->GetPackages()[*base_nup_index];
            LOG_INFO(Loader,
                     "Update-only NUP: using companion base package {:016X} for program "
                     "identity and content",
                     static_cast<u64>(base.title_id));
            program_index = base_nup_index;
            program_from_base_nup = true;
            exefs_index = update_index;
        } else {
            // A standalone update packed from a merged dump carries the whole game (Pokopia's)
            LOG_INFO(Loader,
                     "Update-only NUP {:016X}: no base .nup of this title in the same folder, "
                     "booting the update on its own. If it is a patch-only update and the game "
                     "shows a black screen, place the base game's .nup next to it.",
                     static_cast<u64>(update.title_id));
            program_index = update_index;
            exefs_index = update_index;
        }
    }
    if (!program_index || !exefs_index) {
        LOG_ERROR(Loader, "NUP container has no package with a usable ExeFS");
        return;
    }

    const auto& program = program_from_base_nup ? base_nup->GetPackages()[*base_nup_index]
                                                : nup->GetPackages()[*program_index];
    base_title_id = program.title_id;
    has_program = true;

    const auto exefs = nup->GetExeFSDir(*exefs_index);
    if (exefs == nullptr || exefs->GetFile("main") == nullptr ||
        exefs->GetFile("main.npdm") == nullptr) {
        LOG_ERROR(Loader, "NUP package has no usable ExeFS");
        return;
    }
    secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true);

    if (update_index) {
        const auto& update = nup->GetPackages()[*update_index];
        display_version = update.display_version.empty() ? DecodeVersion(update.version)
                                                         : update.display_version;
    } else {
        display_version = program.display_version.empty() ? DecodeVersion(program.version)
                                                          : program.display_version;
    }
    nacp_file = BuildNacp(program, display_version);
    if (nacp_file != nullptr && !program.icon.empty()) {
        icon_file = std::make_shared<FileSys::VectorVfsFile>(program.icon, "icon.dat");
    } else if (nacp_file != nullptr) {
        for (const auto& [lang, icon] : program.localized_icons) {
            if (lang == "en-us") {
                icon_file = std::make_shared<FileSys::VectorVfsFile>(icon, "icon_" + lang + ".dat");
                break;
            }
        }
        if (icon_file == nullptr && !program.localized_icons.empty()) {
            icon_file = std::make_shared<FileSys::VectorVfsFile>(
                program.localized_icons.front().second, "icon.dat");
        }
    }

    const auto addon_count = std::count_if(nup->GetPackages().begin(), nup->GetPackages().end(),
                                           [](const auto& package) {
                                               return package.kind ==
                                                      NupContainer::PackageKind::Addon;
                                           });
    if (addon_count > 0) {
        LOG_INFO(Loader, "NUP container has {} add-on content package(s)", addon_count);
    }
}

AppLoader_NUP::~AppLoader_NUP() = default;

FileType AppLoader_NUP::IdentifyType(const FileSys::VirtualFile& nup_file) {
    return FileSys::NupContainer::IsNupFile(nup_file) ? FileType::NUP : FileType::Error;
}

AppLoader::LoadResult AppLoader_NUP::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (nup->GetStatus() != ResultStatus::Success) {
        return {nup->GetStatus(), {}};
    }
    if (!has_program || secondary_loader == nullptr) {
        return {ResultStatus::ErrorNUPNoBasePackage, {}};
    }

    const auto result = secondary_loader->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }

    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), base_title_id,
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));

    is_loaded = true;
    return result;
}

ResultStatus AppLoader_NUP::VerifyIntegrity(
    std::function<bool(size_t, size_t)> progress_callback) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return nup->GetStatus();
    }
    return nup->VerifyIntegrity(std::move(progress_callback));
}

std::vector<FileSys::Patch> AppLoader_NUP::GetCarriedPatches() const {
    std::vector<FileSys::Patch> out;
    if (!has_program) {
        return out;
    }
    // A standalone update is the game itself, there is nothing to turn off
    const auto base_index = nup->FindBasePackage();
    const auto update_index = nup->FindBestUpdatePackage();
    if (base_index && update_index) {
        const auto& update = nup->GetPackages()[*update_index];
        out.push_back({
            .enabled = !IsUpdateDisabled(base_title_id),
            .name = "Update",
            .version = update.display_version.empty() ? DecodeVersion(update.version)
                                                      : update.display_version,
            .type = FileSys::PatchType::Update,
            .program_id = base_title_id,
            .title_id = update.title_id,
            .source = FileSys::PatchSource::Packed,
            .location = {},
        });
    }
    std::vector<u64> addons;
    for (const auto& package : nup->GetPackages()) {
        if (package.kind == NupContainer::PackageKind::Addon &&
            FileSys::GetBaseTitleID(package.title_id) == base_title_id &&
            std::find(addons.begin(), addons.end(), package.title_id) == addons.end()) {
            addons.push_back(package.title_id);
        }
    }
    if (!addons.empty()) {
        std::sort(addons.begin(), addons.end());
        std::string ids;
        for (const u64 title_id : addons) {
            ids += fmt::format("{}{}", ids.empty() ? "" : ", ", FileSys::GetAOCID(title_id));
        }
        out.push_back({
            .enabled = !IsAddonDisabled(base_title_id, "DLC"),
            .name = "DLC",
            .version = std::move(ids),
            .type = FileSys::PatchType::DLC,
            .program_id = base_title_id,
            .title_id = base_title_id,
            .source = FileSys::PatchSource::Packed,
            .location = {},
        });
    }
    return out;
}

ResultStatus AppLoader_NUP::ReadAddOnTitleIds(std::vector<u64>& out_title_ids) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return nup->GetStatus();
    }
    for (const auto& package : nup->GetPackages()) {
        if (package.kind == NupContainer::PackageKind::Addon &&
            std::find(out_title_ids.begin(), out_title_ids.end(), package.title_id) ==
                out_title_ids.end()) {
            out_title_ids.push_back(package.title_id);
        }
    }
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadAddOnRomFS(u64 title_id, FileSys::VirtualFile& out_file) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return nup->GetStatus();
    }
    std::scoped_lock lock{addon_romfs_mutex};
    if (const auto it = addon_romfs.find(title_id); it != addon_romfs.end()) {
        out_file = it->second;
        return ResultStatus::Success;
    }
    // The newest version when the container carries more than one of the same add-on
    std::optional<std::size_t> index;
    const auto& packages = nup->GetPackages();
    for (std::size_t i = 0; i < packages.size(); ++i) {
        if (packages[i].kind == NupContainer::PackageKind::Addon &&
            packages[i].title_id == title_id &&
            (!index || packages[i].version > packages[*index].version)) {
            index = i;
        }
    }
    const auto dir = index ? nup->GetRomFSDir(*index) : FileSys::VirtualDir{};
    if (dir == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    // The package holds a loose tree; the program reads add-on content as a RomFS image
    out_file = FileSys::CreateRomFS(dir);
    if (out_file == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    LOG_INFO(Loader, "Add-on content {:016X} from the NUP container", title_id);
    addon_romfs.emplace(title_id, out_file);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadRomFS(FileSys::VirtualFile& out_file) {
    FileSys::VirtualDir base_romfs;
    if (base_nup && base_nup_index) {
        base_romfs = base_nup->GetRomFSDir(*base_nup_index);
    } else {
        const auto base_index = nup->FindBasePackage();
        base_romfs = base_index ? nup->GetRomFSDir(*base_index) : FileSys::VirtualDir{};
    }
    const auto update_romfs = selected_update_index ? nup->GetRomFSDir(*selected_update_index)
                                                    : FileSys::VirtualDir{};
    // A standalone update carries the complete merged tree; a base+update pair
    // overlays the update onto the base (SPEC §3.1.4).
    const auto merged = FileSys::NupContainer::MergeShadowDirs(base_romfs, update_romfs);
    if (merged == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = FileSys::CreateRomFS(merged);
    if (out_file == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    return ResultStatus::Success;
}

bool AppLoader_NUP::IsRomFSUpdatable() const {
    // The update is already merged into the exposed RomFS.
    return false;
}

ResultStatus AppLoader_NUP::ReadProgramId(u64& out_program_id) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_id = base_title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_ids.push_back(base_title_id);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadIcon(std::vector<u8>& buffer) {
    if (icon_file == nullptr) {
        return ResultStatus::ErrorNoIcon;
    }
    buffer = icon_file->ReadAllBytes();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadTitle(std::string& title) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    title = nacp_file->GetApplicationName();
    if (title.empty()) {
        return ResultStatus::ErrorNoControl;
    }
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadControlData(FileSys::NACP& control) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    control = *nacp_file;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadNSOModules(Modules& modules) {
    if (secondary_loader == nullptr) {
        return ResultStatus::ErrorNotInitialized;
    }
    return secondary_loader->ReadNSOModules(modules);
}

} // namespace Loader
