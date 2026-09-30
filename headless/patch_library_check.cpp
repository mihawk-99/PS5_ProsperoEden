// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the patch library (patch_library.h). Build: c++ -std=c++20 patch_library_check.cpp
#include "patch_library.h"
#include <cassert>
#include <map>

int main() {
    using namespace Eden::Patches;
    // Grouping: frame rates and per-mode resolutions exclude each other; the rest combine.
    assert(GroupKey("60 FPS") == "fps" && GroupKey("OG 30 FPS") == "fps" && GroupKey("120(?) FPS For Emulator") == "fps");
    assert(GroupKey("Dynamic Resolution 60 FPS tweak") == "dynamic fps");
    assert(GroupKey("Docked RRS 90% (810p) v2") == "resolution docked" && GroupKey("Docked 1080p") == "resolution docked");
    assert(GroupKey("Handheld 480p") == "resolution handheld");
    assert(GroupKey("Disable RRS Lock for Docked (Re-enable DRS)").empty() && GroupKey("Main FOV 90").empty());

    // Parsing: entries with code only, the master code kept for every entry of the file.
    const char* text =
        "{Master Code}\r\n580F0000 01234567\r\n\r\n[--SectionStart:Frame rate--]\n"
        "[60 FPS]\n04000000 00ABCDEF 52800020\n[30 FPS]\n04000000 00ABCDEF 52800040\n"
        "[Docked 900p]\n04000000 00111111 00000384\n";
    auto entries = ParseCheats(text, "titles/0100F2C0115B6000/cheats/168DD518D925C7A3.txt");
    assert(entries.size() == 3 && entries[0].name == "60 FPS" && entries[2].name == "Docked 900p");
    assert(entries[0].master == "{Master Code}\n580F0000 01234567\n");
    assert(entries[1].text == "[30 FPS]\n04000000 00ABCDEF 52800040\n");

    // Choosing: 60 then 30 FPS keeps only 30; resolution combines; the file carries one master.
    std::set<std::string> chosen;
    Toggle(entries, chosen, 0);
    Toggle(entries, chosen, 2);
    Toggle(entries, chosen, 1);
    assert(chosen.size() == 2 && !chosen.contains(entries[0].id) && chosen.contains(entries[1].id));
    const auto file = CheatFile(entries, chosen);
    assert(file == "{Master Code}\n580F0000 01234567\n[30 FPS]\n04000000 00ABCDEF 52800040\n"
                   "[Docked 900p]\n04000000 00111111 00000384\n");
    Toggle(entries, chosen, 1);
    assert(!chosen.contains(entries[1].id));

    // Finding: the matching build's cheat file and .pchtxt; other games' folders are skipped.
    std::map<std::string, std::vector<std::pair<std::string, bool>>> dirs{
        {"/p", {{"titles", true}, {"mods", true}}},
        {"/p/titles", {{"0100F2C0115B6000", true}, {"01007EF00011E000", true}}},
        {"/p/titles/0100F2C0115B6000", {{"cheats", true}, {"Tears.txt", false}}},
        {"/p/titles/0100F2C0115B6000/cheats", {{"168DD518D925C7A3.txt", false}, {"9A10ED9435C06733.txt", false}}},
        {"/p/titles/01007EF00011E000", {{"cheats", true}}},
        {"/p/mods", {{"Tears [0100F2C0115B6000]", true}, {"loose.pchtxt", false}}},
        {"/p/mods/Tears [0100F2C0115B6000]", {{"60fps", true}}},
        {"/p/mods/Tears [0100F2C0115B6000]/60fps", {{"exefs", true}}},
        {"/p/mods/Tears [0100F2C0115B6000]/60fps/exefs", {{"a.pchtxt", false}}},
    };
    std::map<std::string, std::string> files{
        {"/p/titles/0100F2C0115B6000/cheats/168DD518D925C7A3.txt", text},
        {"/p/titles/0100F2C0115B6000/cheats/9A10ED9435C06733.txt", "[Other build]\n04000000 0 0\n"},
        {"/p/mods/Tears [0100F2C0115B6000]/60fps/exefs/a.pchtxt", "@nsobid-168dd518d925c7a3aaaa\n@enabled\n0 1\n@stop\n"},
        {"/p/mods/loose.pchtxt", "@nsobid-0123456789abcdef\n"},
    };
    std::vector<std::string> listed;
    const Lister list = [&](const std::string& path) {
        listed.push_back(path);
        return dirs.contains(path) ? dirs.at(path) : std::vector<std::pair<std::string, bool>>{};
    };
    const Reader read = [&](const std::string& path, std::string& out) {
        if (!files.contains(path)) return false;
        out = files.at(path);
        return true;
    };
    const auto found = Find("/p", 0x0100F2C0115B6000ull, "168DD518D925C7A3F00D", list, read);
    assert(found.size() == 4 && found[3].kind == Kind::Pchtxt && found[3].name == "60fps/exefs/a.pchtxt");
    assert(std::find(listed.begin(), listed.end(), "/p/titles/01007EF00011E000") == listed.end());
    std::printf("patch library: all checks passed\n");
}
