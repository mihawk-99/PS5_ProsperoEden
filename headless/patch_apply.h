// SPDX-License-Identifier: GPL-3.0-or-later
// The patch library on the console: listing /data/prosperoeden/patches and turning a game's
// chosen entries into the mod folder Eden applies (headless/patch_library.h).
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "patch_library.h"
#include "storage_paths.h"
#if defined(__PROSPERO__)
#include "native_directory.h"
#endif

namespace Eden::Patches {
inline Lister FolderLister() {
    return [](const std::string& path) {
        std::vector<std::pair<std::string, bool>> out;
        std::error_code error;
#if defined(__PROSPERO__)
        const auto entries = Eden::ReadNativeDirectory(path, error);
#else
        std::vector<std::filesystem::directory_entry> entries;
        for (std::filesystem::directory_iterator it{path, error}, end; !error && it != end; it.increment(error))
            entries.push_back(*it);
#endif
        for (const auto& entry : entries) {
            std::error_code type_error;
            out.emplace_back(entry.path().filename().string(), entry.is_directory(type_error));
        }
        return out;
    };
}

inline Reader FileReader() {
    return [](const std::string& path, std::string& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.assign(std::istreambuf_iterator<char>(file), {});
        return out.size() <= (16u << 20); // patch files are small; anything larger is not one
    };
}

// The entries in the patch folder for this game build (none when the folder is missing).
inline std::vector<Entry> ForGame(std::uint64_t title_id, const std::string& build_id) {
    std::error_code error;
    if (!std::filesystem::is_directory(PatchesDir(), error)) return {};
    return Find(PatchesDir(), title_id, build_id, FolderLister(), FileReader());
}

// Eden's mod folder for the chosen entries: load/<title ID>/ProsperoEden Patches.
inline std::string ModFolder(std::uint64_t title_id) {
    char title[17];
    std::snprintf(title, sizeof(title), "%016llX", static_cast<unsigned long long>(title_id));
    return UserDir() + "/load/" + title + "/" + std::string(kModFolder);
}

// Rewrites the game's mod folder from the chosen entries that match this build, or removes it
// when none do. Returns how many entries it applied, -1 when the folder could not be written.
inline int Apply(std::uint64_t title_id, const std::string& build_id, const std::vector<std::string>& chosen_ids) {
    const std::string folder = ModFolder(title_id);
    std::error_code error;
    std::filesystem::remove_all(folder, error); // only ever the folder this function writes
    if (chosen_ids.empty() || build_id.size() < 16) return 0;
    const std::set<std::string> chosen(chosen_ids.begin(), chosen_ids.end());
    const auto entries = ForGame(title_id, build_id);
    int applied = 0;
    const std::string cheats = CheatFile(entries, chosen);
    if (!cheats.empty()) {
        std::filesystem::create_directories(folder + "/cheats", error);
        std::ofstream file(folder + "/cheats/" + build_id.substr(0, 16) + ".txt", std::ios::binary);
        if (!(file << cheats)) return -1;
        for (const auto& entry : entries) applied += entry.kind == Kind::Cheat && chosen.contains(entry.id);
    }
    for (const auto& entry : entries) {
        if (entry.kind != Kind::Pchtxt || !chosen.contains(entry.id)) continue;
        std::filesystem::create_directories(folder + "/exefs", error);
        const std::string name = std::to_string(applied) + "-" +
            entry.source.substr(entry.source.find_last_of('/') + 1);
        std::ofstream file(folder + "/exefs/" + name, std::ios::binary);
        if (!(file << entry.text)) return -1;
        ++applied;
    }
    return applied;
}
} // namespace Eden::Patches
