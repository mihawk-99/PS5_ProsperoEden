#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EDEN_METADATA_TITLE = 1,
    EDEN_METADATA_COVER = 2,
};

// Cached for this process; restart after replacing setup files. Empty means ready.
const char* eden_startup_error(void);
uint64_t eden_game_title_id(const char* rom_path);
// The build ID of the game's main executable as 64 hex digits (the ExeFS "main" NSO's module
// ID), which cheats and patches are made for. 0 when it cannot be read, else 1.
int eden_game_build_id(const char* rom_path, char* hex, size_t hex_capacity);

int eden_extract_game_metadata(const char* rom_path, const char* keys_dir,
                               const char* cover_tga_path, char* title,
                               size_t title_capacity);

#ifdef __cplusplus
}

#include <string>
#include <utility>
#include <vector>

namespace Eden {
// What a game has besides itself: updates and DLC from the updates and roms folders (including
// an update packed into the game's own file), found as Eden finds them at launch.
struct GameContent {
    std::string base_version;                                // the game's own display version
    std::vector<std::pair<uint32_t, std::string>> updates;   // version number and name, newest first
    unsigned dlc = 0;
};
// Reads the updates and roms folders again (the Library does on opening).
void RescanGameContent();
GameContent ReadGameContent(const char* rom_path, uint64_t title_id);
// The build ID (64 hex digits) of the program that runs with this update (0: the game itself),
// which is what cheats and patches are made for. Empty when it cannot be read.
std::string ReadBuildId(const char* rom_path, uint32_t update_version);
} // namespace Eden
#endif
