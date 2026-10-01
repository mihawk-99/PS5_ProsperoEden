#pragma once

#include "radio_input.h"
#include "preferences.h"
#include "patch_library.h"
#include <set>
#include <string>
#include <vector>
#include <ctime>

namespace Rml { class ElementDocument; }

class EdenApp final {
public:
    bool Initialize(Rml::ElementDocument* document, const std::string& launch_error = {});
    void HandleInput(const radio_input_event_t& event);
    void Poll();
    void Shutdown() { document_ = nullptr; }
    const std::string& SelectedGame() const { return selected_game_; }

private:
    void Update();
    void UpdateSettings();
    void SaveSettings();
    void UpdateDialog();
    void Open(const char* id, int dialog);
    void Close();
    // Settings > Game files: a folder browser for the keys/firmware/roms folder.
    void OpenFiles();
    bool BrowseTo(const std::string& directory);
    void HandleFilesInput(const radio_input_event_t& event);
    void UpdateFiles();
    // A game's Patches page: the patch folder's entries for every build of the game, grouped by
    // version, those of versions not chosen greyed out (headless/patch_library.h).
    void OpenPatches(int game);
    void HandlePatchesInput(const radio_input_event_t& event);
    void UpdatePatches();

    Rml::ElementDocument* document_ = nullptr;
    Eden::Preferences preferences_;
    int option_ = 0;
    std::string last_game_;
    std::vector<std::string> recent_games_;
    std::time_t shown_minute_ = 0;
    bool continue_ready_ = false;
    bool setup_ready_ = false;
    int selected_ = 0;
    int dialog_ = 0;
    int dialog_selected_ = 0;
    std::string selected_game_;
    std::string browse_dir_;
    std::vector<std::string> browse_entries_; // ".." first unless at "/", then subfolders
    int browse_selected_ = 0;
    std::string files_message_;
    int patches_game_index_ = 0;
    void LoadPatches();
    uint64_t patches_title_{};
    std::string patches_build_;
    std::string patches_game_;
    std::vector<Eden::Patches::Entry> patches_;
    struct PatchRow {
        int entry = -1;     // index in patches_; -1 for a version's header row
        std::string build;  // the version's build (16 hex digits, lower case)
        std::string label;  // the version's name
        bool installed = false; // the game or one of its updates in the updates folder
        uint32_t version = 0;   // with installed: 0 the game itself, else the update
    };
    std::vector<PatchRow> patch_versions_; // one header per version, in the order shown
    std::vector<PatchRow> patch_rows_;     // what the list shows: headers, and open versions' entries
    std::set<std::string> patches_open_;   // builds whose entries are shown
    void ShowPatchRows();
    std::set<std::string> patches_chosen_;
    int patches_selected_ = 0;
    std::string patches_message_;
};
