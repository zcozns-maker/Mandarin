// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <string_view>
#include "common/logging.h"
#include "common/random.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/romfs.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/dlss_trace.h"
#include "core/hle/guest_hooks.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nso.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

#ifndef HAS_NCE
namespace Core::NCE {
class Patcher {};
} // namespace Core::NCE
#endif

namespace Loader {

struct PatchCollection {
    explicit PatchCollection(bool is_application_) : is_application{is_application_} {
        module_patcher_indices.fill(-1);
        patchers.emplace_back();
    }

    std::vector<Core::NCE::Patcher>* GetPatchers() {
        if (is_application && Settings::IsNceEnabled()) {
            return &patchers;
        }
        return nullptr;
    }

    size_t GetTotalPatchSize() const {
        size_t total_size{};
#ifdef HAS_NCE
        for (auto& patcher : patchers) {
            total_size += patcher.GetSectionSize();
        }
#endif
        return total_size;
    }

    void SaveIndex(size_t module) {
        module_patcher_indices[module] = static_cast<s32>(patchers.size() - 1);
    }

    s32 GetIndex(size_t module) const {
        return module_patcher_indices[module];
    }

    s32 GetLastIndex() const {
        return static_cast<s32>(patchers.size()) - 1;
    }

    bool is_application;
    std::vector<Core::NCE::Patcher> patchers;
    std::array<s32, 13> module_patcher_indices{};
};

// The display version of a dump without control data, from its RomFS: Nintendo EPD titles name
// their resource size tables after it (System/Resource/ResourceSizeTable.Product.180.* in Mario
// Kart World 1.8.0). A hotfix can keep its minor version's tables (Product.110 in a 1.1.1).
static std::string RomFSProductVersion(const FileSys::VirtualDir& romfs_dir) {
    if (romfs_dir == nullptr) {
        return {};
    }
    const auto system_dir = romfs_dir->GetSubdirectory("System");
    const auto resource_dir = system_dir != nullptr ? system_dir->GetSubdirectory("Resource") : nullptr;
    if (resource_dir == nullptr) {
        return {};
    }
    constexpr std::string_view Prefix = "ResourceSizeTable.Product.";
    std::string best;
    for (const auto& file : resource_dir->GetFiles()) {
        const std::string file_name = file->GetName();
        if (!file_name.starts_with(Prefix) || file_name.size() < Prefix.size() + 4) {
            continue;
        }
        const std::string_view digits = std::string_view{file_name}.substr(Prefix.size(), 3);
        if (file_name[Prefix.size() + 3] != '.' ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        if (std::string version{digits}; version > best) {
            best = version;
        }
    }
    if (best.empty()) {
        return {};
    }
    return fmt::format("{}.{}.{}", best[0], best[1], best[2]);
}

// One metadata path for every loose ExeFS/RomFS dump, so every game gets its icon, title and
// version the same way, whatever folder layout its dumper used:
//   <game>/exefs, <game>/base/exefs, <game>/updates/<version>/exefs, <game>/<Game_Name>/base/exefs
// Searched folders, in order: the ExeFS folder, the folders above it up to the game's root (the
// first folder that is not exefs/base/updates/<number>), the root's parent when it is the same
// name spelled differently ("Yoshi And The ... - Demo/YoshiAndThe..._Demo"), then the game's
// other version folders (<root>/base, then <root>/updates/<n>, newest first). A sidecar or
// control.nacp in any of them serves every version of the game. It never climbs above the game,
// so files in a shared "all my games" folder are not picked up. Sources, first found wins:
//   title:   control.nacp > game.ini name=/title= > title.txt > the game's folder name
//   icon:    icon_<language>.dat / any image next to main > icon.png/jpg/jpeg/bmp, icon_*.dat in
//            the searched folders
//   version: control.nacp > game.ini version= > RomFS Product tables (else the game list and the
//            game get "1.0.0", as before)
// Optional sidecars (hand-made, any text encoding): game.ini (name=, version=), title.txt (first
// line is the name), icon.png / icon.jpg.
struct SidecarMetadata {
    std::string name;
    std::string version;
    std::vector<u8> icon;
    std::string name_source;
    std::string icon_source;
};

static bool IsAllDigits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Folder names a dumper uses for a game's parts rather than for the game itself.
static bool IsGenericDumpFolder(const std::string& folder_name) {
    const std::string lower = Common::ToLower(folder_name);
    return lower == "exefs" || lower == "romfs" || lower == "base" || lower == "updates" ||
           lower == "update" || lower == "dlc" || lower == "control" || IsAllDigits(lower);
}

// Letters and digits only, lower case: "Yoshi And The Mysterious Book - Demo" and
// "YoshiAndTheMysteriousBook_Demo" compare equal.
static std::string NormalizeFolderName(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) {
            out.push_back(c);
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (static_cast<u8>(c) >= 0x80) {
            out.push_back(c);
        }
    }
    return out;
}

// A readable title from a folder name: "_" to spaces, CamelCase split when there are no spaces,
// and a trailing version ("Mario Kart World 1.0", "Pokopia 2.0.1", "Game v1.2") dropped.
static std::string FolderNameToTitle(std::string text) {
    std::replace(text.begin(), text.end(), '_', ' ');
    if (text.find(' ') == std::string::npos) {
        std::string split;
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (i > 0 && c >= 'A' && c <= 'Z') {
                const char prev = text[i - 1];
                const bool prev_lower = prev >= 'a' && prev <= 'z';
                const bool next_lower =
                    i + 1 < text.size() && text[i + 1] >= 'a' && text[i + 1] <= 'z';
                const bool prev_upper = prev >= 'A' && prev <= 'Z';
                if (prev_lower || (prev_upper && next_lower)) {
                    split.push_back(' ');
                }
            }
            split.push_back(c);
        }
        text = std::move(split);
    }
    // Drop a trailing version token made of digits and dots, optionally prefixed by v/V.
    while (true) {
        const auto last_space = text.find_last_of(' ');
        if (last_space == std::string::npos || last_space == 0) {
            break;
        }
        std::string_view token{text};
        token.remove_prefix(last_space + 1);
        if (!token.empty() && (token[0] == 'v' || token[0] == 'V')) {
            token.remove_prefix(1);
        }
        const bool is_version =
            !token.empty() && token.find('.') != std::string_view::npos &&
            std::all_of(token.begin(), token.end(),
                        [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
        if (!is_version) {
            break;
        }
        text.erase(last_space);
    }
    // Collapse runs of spaces and trim.
    std::string out;
    for (const char c : text) {
        if (c == ' ' && (out.empty() || out.back() == ' ')) {
            continue;
        }
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-')) {
        out.pop_back();
    }
    return out;
}

struct DumpFolders {
    std::vector<FileSys::VirtualDir> search; ///< Where sidecars and control.nacp are looked for
    std::string title;                       ///< Title from the game's folder name
};

static DumpFolders FindDumpFolders(const FileSys::VirtualDir& file_dir) {
    DumpFolders result;
    const auto add = [&result](const FileSys::VirtualDir& dir) {
        if (dir == nullptr) {
            return;
        }
        const std::string path = dir->GetFullPath();
        if (std::none_of(result.search.begin(), result.search.end(),
                         [&path](const FileSys::VirtualDir& d) { return d->GetFullPath() == path; })) {
            result.search.push_back(dir);
        }
    };

    // Climb to the game's root: the first folder that is not a part folder (at most 4 levels).
    FileSys::VirtualDir root;
    auto dir = file_dir;
    for (int level = 0; level < 5 && dir != nullptr; ++level) {
        add(dir);
        if (!IsGenericDumpFolder(dir->GetName())) {
            root = dir;
            break;
        }
        dir = dir->GetParentDirectory();
    }
    if (root == nullptr) {
        // Unusual layout: keep the old behaviour (the folder of main and the two above it).
        auto up = file_dir;
        for (int level = 0; level < 3 && up != nullptr; ++level, up = up->GetParentDirectory()) {
            add(up);
        }
        return result;
    }
    if (root == file_dir) {
        // main directly in the game folder: also look one folder up, as before.
        add(root->GetParentDirectory());
    }

    std::string title_folder = root->GetName();
    if (const auto parent = root->GetParentDirectory()) {
        if (NormalizeFolderName(parent->GetName()) == NormalizeFolderName(root->GetName())) {
            add(parent);
            if (parent->GetName().find(' ') != std::string::npos) {
                title_folder = parent->GetName();
            }
        }
    }
    result.title = FolderNameToTitle(title_folder);

    // The game's other version folders.
    for (const auto& sub : root->GetSubdirectories()) {
        if (Common::ToLower(sub->GetName()) == "base") {
            add(sub);
        }
    }
    for (const auto& sub : root->GetSubdirectories()) {
        if (Common::ToLower(sub->GetName()) != "updates") {
            continue;
        }
        auto versions = sub->GetSubdirectories();
        std::erase_if(versions, [](const FileSys::VirtualDir& d) { return !IsAllDigits(d->GetName()); });
        std::sort(versions.begin(), versions.end(),
                  [](const FileSys::VirtualDir& a, const FileSys::VirtualDir& b) {
                      // Numeric order without parsing (names can be any length).
                      const auto& x = a->GetName();
                      const auto& y = b->GetName();
                      return x.size() != y.size() ? x.size() > y.size() : x > y;
                  });
        for (const auto& version : versions) {
            add(version);
        }
    }
    return result;
}

static std::string TrimSidecarText(std::string text) {
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

static bool IsValidUtf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const u8 c = static_cast<u8>(text[i]);
        size_t extra = 0;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size() + (extra == 0 ? 1 : 0) && extra != 0) {
            return false;
        }
        for (size_t k = 1; k <= extra; ++k) {
            if (i + k >= text.size() || (static_cast<u8>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

// Sidecar files are hand-written, so accept UTF-8 (with or without BOM), UTF-16 with a BOM
// (Notepad's "Unicode"), and fall back to Latin-1 / Windows-1252 for anything that isn't valid
// UTF-8, so a name like "Pokémon" shows correctly whatever editor saved it.
static std::string DecodeSidecarText(const std::vector<u8>& bytes) {
    if (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
                              (bytes[0] == 0xFE && bytes[1] == 0xFF))) {
        const bool little = bytes[0] == 0xFF;
        std::u16string wide;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            wide.push_back(static_cast<char16_t>(little ? (bytes[i] | (bytes[i + 1] << 8))
                                                        : ((bytes[i] << 8) | bytes[i + 1])));
        }
        return Common::UTF16ToUTF8(wide);
    }
    std::string text(bytes.begin(), bytes.end());
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    if (IsValidUtf8(text)) {
        return text;
    }
    std::string converted;
    converted.reserve(text.size() * 2);
    for (const char ch : text) {
        const u8 c = static_cast<u8>(ch);
        if (c < 0x80) {
            converted.push_back(ch);
        } else {
            converted.push_back(static_cast<char>(0xC0 | (c >> 6)));
            converted.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return converted;
}

static SidecarMetadata ReadSidecarMetadata(const std::vector<FileSys::VirtualDir>& dirs) {
    SidecarMetadata result;
    for (const auto& dir : dirs) {
        if (result.name.empty() || result.version.empty()) {
            if (const auto ini = dir->GetFile("game.ini")) {
                const std::string text = DecodeSidecarText(ini->ReadAllBytes());
                size_t pos = 0;
                while (pos < text.size()) {
                    auto end = text.find('\n', pos);
                    if (end == std::string::npos) end = text.size();
                    const std::string line = TrimSidecarText(text.substr(pos, end - pos));
                    pos = end + 1;
                    const auto eq = line.find('=');
                    if (eq == std::string::npos || line.starts_with(';') || line.starts_with('#')) {
                        continue;
                    }
                    const std::string key = Common::ToLower(TrimSidecarText(line.substr(0, eq)));
                    const std::string value = TrimSidecarText(line.substr(eq + 1));
                    if ((key == "name" || key == "title") && result.name.empty() && !value.empty()) {
                        result.name = value;
                        result.name_source = ini->GetFullPath();
                    } else if (key == "version" && result.version.empty()) {
                        result.version = value;
                    }
                }
            }
        }
        if (result.name.empty()) {
            if (const auto txt = dir->GetFile("title.txt")) {
                std::string text = TrimSidecarText(DecodeSidecarText(txt->ReadAllBytes()));
                result.name = TrimSidecarText(text.substr(0, text.find('\n')));
                if (!result.name.empty()) {
                    result.name_source = txt->GetFullPath();
                }
            }
        }
        if (result.icon.empty()) {
            for (const char* icon_name : {"icon.png", "icon.jpg", "icon.jpeg", "icon.bmp"}) {
                if (const auto icon = dir->GetFile(icon_name)) {
                    result.icon = icon->ReadAllBytes();
                    result.icon_source = icon->GetFullPath();
                    break;
                }
            }
        }
        if (result.icon.empty()) {
            for (const auto& language : FileSys::LANGUAGE_NAMES) {
                if (const auto icon = dir->GetFile("icon_" + std::string(language) + ".dat")) {
                    result.icon = icon->ReadAllBytes();
                    result.icon_source = icon->GetFullPath();
                    break;
                }
            }
        }
    }
    return result;
}
AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(FileSys::VirtualFile file_, bool override_update_)
    : AppLoader(std::move(file_))
    , override_update(override_update_)
{
    const auto file_dir = file->GetContainingDirectory();

    // Title ID
    const auto npdm = file_dir->GetFile("main.npdm");
    if (npdm != nullptr) {
        const auto res = metadata.Load(npdm);
        if (res == ResultStatus::Success)
            title_id = metadata.GetTitleID();
    }

    // Metadata, the same way for every loose dump (see FindDumpFolders / ReadSidecarMetadata).
    const DumpFolders folders = FindDumpFolders(file_dir);
    std::string icon_source;

    // Icon next to main: icon_<language>.dat, else any png, jpeg, or bmp file.
    for (const auto& language : FileSys::LANGUAGE_NAMES) {
        if (const auto icon_file = file_dir->GetFile("icon_" + std::string(language) + ".dat")) {
            icon_data = icon_file->ReadAllBytes();
            icon_source = icon_file->GetFullPath();
            break;
        }
    }
    if (icon_data.empty()) {
        const auto& files = file_dir->GetFiles();
        const auto icon_iter =
            std::find_if(files.begin(), files.end(), [](const FileSys::VirtualFile& f) {
                return f->GetExtension() == "png" || f->GetExtension() == "jpg" ||
                       f->GetExtension() == "bmp" || f->GetExtension() == "jpeg";
            });
        if (icon_iter != files.end()) {
            icon_data = (*icon_iter)->ReadAllBytes();
            icon_source = (*icon_iter)->GetFullPath();
        }
    }

    // Control data: control.nacp (or any .nacp) next to main, else a control.nacp in any searched
    // folder (dumps keep the title's or the update's control data at their root).
    FileSys::VirtualFile nacp_file = file_dir->GetFile("control.nacp");
    if (nacp_file == nullptr) {
        const auto& files = file_dir->GetFiles();
        const auto nacp_iter =
            std::find_if(files.begin(), files.end(),
                         [](const FileSys::VirtualFile& f) { return f->GetExtension() == "nacp"; });
        if (nacp_iter != files.end())
            nacp_file = *nacp_iter;
    }
    for (const auto& search_dir : folders.search) {
        if (nacp_file != nullptr) {
            break;
        }
        nacp_file = search_dir->GetFile("control.nacp");
    }

    std::string title_source;
    if (nacp_file != nullptr) {
        control.emplace(nacp_file);
        name = control->GetApplicationName();
        if (!name.empty()) {
            title_source = nacp_file->GetFullPath();
        }
    }

    // The loose RomFS beside the exefs folder (as ReadRomFS finds it).
    if (const auto title_dir = file_dir->GetParentDirectory()) {
        romfs_version = RomFSProductVersion(title_dir->GetSubdirectory("romfs"));
    }

    // Sidecar name/version/icon for dumps without (complete) control data.
    auto sidecar = ReadSidecarMetadata(folders.search);
    if (name.empty() && !sidecar.name.empty()) {
        name = std::move(sidecar.name);
        title_source = std::move(sidecar.name_source);
    }
    if (icon_data.empty() && !sidecar.icon.empty()) {
        icon_data = std::move(sidecar.icon);
        icon_source = std::move(sidecar.icon_source);
    }
    sidecar_version = std::move(sidecar.version);

    // Last resort for the title: the game's folder name, so no dump shows up untitled.
    if (name.empty() && !folders.title.empty()) {
        name = folders.title;
        title_source = "folder name";
    }

    std::string version;
    void(ReadDisplayVersion(version));
    LOG_INFO(Loader,
             "Dump metadata for {:016X}: title \"{}\" ({}), icon {} ({}), version {} ({} folders "
             "searched)",
             title_id, name, title_source.empty() ? "none" : title_source,
             icon_data.empty() ? "none" : fmt::format("{} bytes", icon_data.size()),
             icon_source.empty() ? "none - add icon.jpg next to exefs" : icon_source,
             version.empty() ? "unknown (1.0.0)" : version, folders.search.size());
}
AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(
    FileSys::VirtualDir directory, bool override_update_)
    : AppLoader(directory->GetFile("main"))
    , dir(std::move(directory))
    , override_update(override_update_)
    , standalone(false)
{}

FileType AppLoader_DeconstructedRomDirectory::IdentifyType(const FileSys::VirtualFile& dir_file) {
    const auto directory = dir_file->GetContainingDirectory();
    if (dir_file->GetName() == "main" && directory &&
        AppLoader_NSO::IdentifyType(dir_file) == FileType::NSO &&
        FileSys::IsDirectoryExeFS(directory)) {
        return FileType::DeconstructedRomDirectory;
    }

    return FileType::Error;
}

AppLoader_DeconstructedRomDirectory::LoadResult AppLoader_DeconstructedRomDirectory::Load(
    Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (dir == nullptr) {
        if (file == nullptr) {
            return {ResultStatus::ErrorNullFile, {}};
        }

        dir = file->GetContainingDirectory();
    }

    // Read meta to determine title ID
    FileSys::VirtualFile npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result = metadata.Load(npdm);
    if (result != ResultStatus::Success) {
        return {result, {}};
    }

    if (override_update) {
        const FileSys::PatchManager patch_manager(
            metadata.GetTitleID(), system.GetFileSystemController(), system.GetContentProvider());
        dir = patch_manager.PatchExeFS(dir);
    }

    // Reread in case PatchExeFS affected the main.npdm
    npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result2 = metadata.Reload(npdm);
    if (result2 != ResultStatus::Success) {
        return {result2, {}};
    }
    metadata.Print();

    // Extended ACID programs currently require the Dynarmic compatibility path.
    const bool is_39bit =
        metadata.GetAddressSpaceType() == FileSys::ProgramAddressSpaceType::Is39Bit;
    const bool is_application = metadata.GetPoolPartition() == FileSys::PoolPartition::Application;
    Settings::SetNceEnabled(is_39bit && !metadata.UsesExtendedAcid());

    const std::array static_modules = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                       "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                       "subsdk8", "subsdk9", "sdk"};

    std::size_t code_size{};

    // Define an nce patch context for each potential module.
    PatchCollection patch_ctx{is_application};

    // Use the NSO module loader to figure out the code layout
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, code_size, should_pass_arguments, false, {},
            patch_ctx.GetPatchers(), patch_ctx.GetLastIndex());
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        patch_ctx.SaveIndex(i);
        code_size = *tentative_next_load_addr;
    }

    // Enable direct memory mapping in case of NCE.
    const u64 fastmem_base = [&]() -> size_t {
        if (is_application && Settings::IsNceEnabled()) {
            auto& buffer = system.DeviceMemory().buffer;
            buffer.EnableDirectMappedAddress();
            return reinterpret_cast<u64>(buffer.VirtualBasePointer());
        }
        return 0;
    }();

    // Add patch size to the total module size
    code_size += patch_ctx.GetTotalPatchSize();

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process.LoadFromMetadata(system.Kernel(), metadata, code_size, fastmem_base, aslr_offset).IsError()) {
        return {ResultStatus::ErrorUnableToParseKernelMetadata, {}};
    }

    // Load NSO modules
    modules.clear();
    const VAddr base_address{GetInteger(process.GetEntryPoint())};
    VAddr next_load_addr{base_address};
    const FileSys::PatchManager pm{metadata.GetTitleID(), system.GetFileSystemController(),
                                   system.GetContentProvider()};
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const VAddr load_addr{next_load_addr};
        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, load_addr, should_pass_arguments, true, pm,
            patch_ctx.GetPatchers(), patch_ctx.GetIndex(i));
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        next_load_addr = *tentative_next_load_addr;
        modules.insert_or_assign(load_addr, module);
        LOG_DEBUG(Loader, "loaded module {} @ {:#x}", module, load_addr);
        Core::HLE::DlssTrace::InstallModule(process, module, load_addr, next_load_addr - load_addr);
        NSOHeader nso_header{};
        if (module_file->ReadObject(&nso_header) == sizeof(nso_header)) {
            Core::HLE::GuestHooks::InstallModule(process, module, load_addr,
                                                 next_load_addr - load_addr,
                                                 nso_header.segments[0].size);
        }
    }

    is_loaded = true;
    if (standalone) {
        title_id = metadata.GetTitleID();
        // Loose-ExeFS loads bypass the NCA/NSP loaders, which are the ones that apply LayeredFS
        // to the RomFS; apply it here so mods in load/<titleid>/ work too (no update is applied
        // without a base NCA).
        if (FileSys::VirtualFile base; ReadRomFS(base) == ResultStatus::Success) {
            const FileSys::PatchManager patch_manager(title_id, system.GetFileSystemController(),
                                                      system.GetContentProvider());
            if (auto patched = patch_manager.PatchRomFS(nullptr, base); patched != nullptr) {
                romfs = std::move(patched);
            }
        }
        system.GetFileSystemController().RegisterProcess(
            process.GetProcessId(), {},
            std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                    system.GetFileSystemController()));
    }
    return {ResultStatus::Success,
            LoadParameters{metadata.GetMainThreadPriority(), metadata.GetMainThreadStackSize()}};
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadRomFS(FileSys::VirtualFile& out_dir) {
    if (romfs == nullptr && standalone) {
        const auto directory = file->GetContainingDirectory();
        for (const auto& entry : directory->GetFiles()) {
            if (entry->GetExtension() == "romfs") {
                romfs = entry;
                break;
            }
        }
        if (romfs == nullptr) {
            auto loose = directory->GetSubdirectory("romfs");
            if (!loose && directory->GetName() == "exefs") {
                const auto parent = directory->GetParentDirectory();
                if (parent) {
                    loose = parent->GetSubdirectory("romfs");
                }
            }
            if (loose) {
                romfs = FileSys::CreateRomFS(std::move(loose));
            }
        }
    }
    if (romfs == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }

    out_dir = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadIcon(std::vector<u8>& out_buffer) {
    if (icon_data.empty()) {
        return ResultStatus::ErrorNoIcon;
    }

    out_buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadProgramId(u64& out_program_id) {
    out_program_id = title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadTitle(std::string& out_title) {
    if (name.empty()) {
        return ResultStatus::ErrorNoControl;
    }

    out_title = name;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadControlData(FileSys::NACP& out_control) {
    if (!control) {
        return ResultStatus::ErrorNoControl;
    }
    out_control = *control;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadDisplayVersion(std::string& version) {
    if (control && !control->GetVersionString().empty()) {
        version = control->GetVersionString();
        return ResultStatus::Success;
    }
    if (!sidecar_version.empty()) {
        version = sidecar_version;
        return ResultStatus::Success;
    }
    if (!romfs_version.empty()) {
        version = romfs_version;
        return ResultStatus::Success;
    }
    return ResultStatus::ErrorNoControl;
}

bool AppLoader_DeconstructedRomDirectory::IsRomFSUpdatable() const {
    return false;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadNSOModules(Modules& out_modules) {
    if (!is_loaded) {
        return ResultStatus::ErrorNotInitialized;
    }

    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
