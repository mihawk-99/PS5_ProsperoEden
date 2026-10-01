// SPDX-License-Identifier: GPL-3.0-or-later
// The patch library: collections the player copies into /data/prosperoeden/patches, exactly as
// downloaded, matched to a game by its executable's build ID. Nothing here ships third-party
// patches; it reads what is in the folder.
//
//   Cheat files     .../cheats/<first 16 hex digits of the build ID>.txt, the Atmosphere layout
//                   (titles/<title ID>/cheats/... in cheat databases, atmosphere/contents/...).
//                   Each [entry] is chosen on its own; the file's {master} code comes with them.
//   IPSwitch files  *.pchtxt anywhere (also inside extracted mods' exefs folders) whose
//                   @nsobid-<build ID> matches. Chosen whole, as their authors wrote them.
//
// At launch the chosen entries become one mod folder in Eden's load directory
// (load/<title ID>/ProsperoEden Patches/), which Eden applies like any other mod.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Eden::Patches {
enum class Kind { Cheat, Pchtxt };

struct Entry {
    std::string id;     // saved in the settings: source path + "#" + entry name (cheats)
    std::string name;   // shown
    std::string source; // file, relative to the patch folder
    std::string group;  // entries of one source with the same group exclude each other
    Kind kind{};
    std::string text;   // a cheat's [name] block, or the whole .pchtxt
    std::string master; // a cheat file's {master} blocks
    std::string build;  // the build it is made for: first 16 hex digits, lower case
};

inline constexpr std::string_view kModFolder = "ProsperoEden Patches";

inline std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline std::string Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

// Choices that replace each other: frame rates ("60 FPS", "30 FPS"), and resolutions per mode
// ("Docked 900p", "Docked RRS 90% (810p)"). Other entries combine freely.
inline std::string GroupKey(std::string_view name) {
    const std::string lower = Lower(name);
    std::vector<std::string> words;
    std::string word;
    for (const char c : lower + " ") {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word += c;
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    const auto has = [&](std::string_view w) { return std::find(words.begin(), words.end(), w) != words.end(); };
    bool fps = false, resolution = has("rrs") || has("resolution");
    for (const auto& w : words) {
        fps |= w == "fps" || (w.size() > 3 && w.ends_with("fps") && std::isdigit(static_cast<unsigned char>(w[0])));
        resolution |= w.size() > 1 && w.back() == 'p' &&
                      std::all_of(w.begin(), w.end() - 1, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    }
    if (fps) return has("dynamic") ? "dynamic fps" : "fps";
    if (resolution && !has("disable") && !has("lock"))
        return has("handheld") ? "resolution handheld" : has("docked") ? "resolution docked" : "resolution";
    return {};
}

// The [entries] of an Atmosphere cheat file; entries without code (section titles) are left out.
inline std::vector<Entry> ParseCheats(std::string_view text, const std::string& source) {
    std::vector<Entry> entries;
    std::string master;
    Entry* current = nullptr;
    bool in_master = false;
    bool current_has_code = false;
    const auto finish = [&] {
        if (current && !current_has_code) entries.pop_back();
        current = nullptr;
    };
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty()) {
            if (end == text.size()) break;
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            finish();
            in_master = false;
            Entry entry;
            entry.name = Trim(std::string_view(line).substr(1, line.size() - 2));
            entry.source = source;
            entry.id = source + "#" + entry.name;
            entry.kind = Kind::Cheat;
            entry.group = GroupKey(entry.name);
            entry.text = line + "\n";
            const auto slash = source.find_last_of('/');
            entry.build = Lower(source.substr(slash + 1, 16));
            entries.push_back(std::move(entry));
            current = &entries.back();
            current_has_code = false;
        } else if (line.front() == '{' && line.back() == '}') {
            finish();
            in_master = true;
            master += line + "\n";
        } else if (in_master) {
            master += line + "\n";
        } else if (current) {
            current->text += line + "\n";
            current_has_code |= std::isxdigit(static_cast<unsigned char>(line.front())) != 0;
        }
        if (end == text.size()) break;
    }
    finish();
    for (auto& entry : entries) entry.master = master;
    return entries;
}

// The build ID an IPSwitch file names (@nsobid-<hex>), lower case; empty when it has none.
inline std::string PchtxtBuildId(std::string_view text) {
    const auto at = text.find("@nsobid-");
    if (at == std::string_view::npos) return {};
    std::string id;
    for (std::size_t i = at + 8; i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i])); ++i)
        id += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    return id;
}

// Build IDs match on their first 16 hex digits, as Eden's cheat lookup does.
inline bool SameBuild(std::string_view a, std::string_view b) {
    return a.size() >= 16 && b.size() >= 16 && Lower(a.substr(0, 16)) == Lower(b.substr(0, 16));
}

// Directory listing: (name, is_directory) pairs. File reading: false when unreadable.
using Lister = std::function<std::vector<std::pair<std::string, bool>>(const std::string&)>;
using Reader = std::function<bool(const std::string&, std::string&)>;

// Every entry in `root` for this build, or with an empty build_id every build's entries that sit
// in a folder whose name holds this game's title ID ("0100...", "Tears [0100...]"). Title ID folders of other games are not entered.
inline std::vector<Entry> Find(const std::string& root, std::uint64_t title_id, const std::string& build_id,
                               const Lister& list, const Reader& read) {
    std::vector<Entry> found;
    const bool any_build = build_id.empty();
    if (!any_build && build_id.size() < 16) return found;
    char title[17];
    std::snprintf(title, sizeof(title), "%016llx", static_cast<unsigned long long>(title_id));
    const std::string build = any_build ? std::string{} : Lower(build_id.substr(0, 16));
    struct Pending {
        std::string path;
        int depth;
        bool in_title; // inside a folder named for this game
    };
    std::vector<Pending> pending{{root, 0, false}};
    unsigned visited = 0;
    while (!pending.empty() && visited < 50000) {
        const Pending dir = pending.back();
        pending.pop_back();
        auto names = list(dir.path);
        std::sort(names.begin(), names.end());
        const std::string dir_name = Lower(dir.path.substr(dir.path.find_last_of('/') + 1));
        for (const auto& [name, is_directory] : names) {
            ++visited;
            const std::string path = dir.path + "/" + name;
            const std::string lower = Lower(name);
            if (is_directory) {
                // A folder named for another game's title ID ("0100...", "[0100...]") is skipped.
                std::string digits;
                for (const char c : lower) if (std::isxdigit(static_cast<unsigned char>(c))) digits += c;
                const bool title_folder = digits.size() == 16 && digits.starts_with("01") &&
                    lower.size() <= 18 && (lower.size() == 16 || (lower.front() == '[' && lower.back() == ']'));
                if ((!title_folder || digits == title) && dir.depth < 10)
                    pending.push_back({path, dir.depth + 1, dir.in_title || lower.find(title) != std::string::npos});
                continue;
            }
            if (any_build && !dir.in_title) continue;
            const std::string relative = path.substr(root.size() + 1);
            const bool cheat_file = lower.size() == 20 && lower.ends_with(".txt") &&
                std::all_of(lower.begin(), lower.end() - 4, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
            if (dir_name == "cheats" && cheat_file && (any_build || lower == build + ".txt")) {
                std::string text;
                if (read(path, text)) {
                    auto entries = ParseCheats(text, relative);
                    found.insert(found.end(), entries.begin(), entries.end());
                }
            } else if (lower.ends_with(".pchtxt")) {
                std::string text;
                if (read(path, text) && (any_build ? PchtxtBuildId(text).size() >= 16 : SameBuild(PchtxtBuildId(text), build))) {
                    Entry entry;
                    entry.kind = Kind::Pchtxt;
                    entry.source = relative;
                    entry.id = relative;
                    // The last folders name the mod ("Game/[title]/1.0.2/60fps/exefs/x.pchtxt").
                    std::string shown = relative;
                    for (int parts = 0, at = static_cast<int>(shown.size()) - 1; at >= 0; --at)
                        if (shown[at] == '/' && ++parts == 3) {
                            shown = shown.substr(at + 1);
                            break;
                        }
                    entry.name = shown;
                    entry.build = PchtxtBuildId(text).substr(0, 16);
                    entry.text = std::move(text);
                    found.push_back(std::move(entry));
                }
            }
        }
    }
    return found;
}

// Picking `index` clears the other entries of its group in the same source.
inline void Toggle(const std::vector<Entry>& entries, std::set<std::string>& chosen, std::size_t index) {
    const Entry& entry = entries.at(index);
    if (chosen.erase(entry.id)) return;
    if (!entry.group.empty())
        for (const auto& other : entries)
            if (other.source == entry.source && other.group == entry.group) chosen.erase(other.id);
    chosen.insert(entry.id);
}

// The cheat file Eden loads for the chosen cheats: each source's master code once, then entries.
inline std::string CheatFile(const std::vector<Entry>& entries, const std::set<std::string>& chosen) {
    std::string out;
    std::set<std::string> masters;
    for (const auto& entry : entries)
        if (entry.kind == Kind::Cheat && chosen.contains(entry.id) && masters.insert(entry.source).second)
            out += entry.master;
    for (const auto& entry : entries)
        if (entry.kind == Kind::Cheat && chosen.contains(entry.id)) out += entry.text;
    return out;
}
} // namespace Eden::Patches
