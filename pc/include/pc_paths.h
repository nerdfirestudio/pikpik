#ifndef PC_PATHS_H
#define PC_PATHS_H

// M7: where things live once the game is not run out of its build tree.
//
// Up to M6 everything was resolved relative to the CURRENT WORKING DIRECTORY —
// "gamedata" for the disc filesystem, "./save" for the memory card. That works
// only when you cd into build-m1 and run ./Pikmin. A double-clicked .app is
// launched with an arbitrary cwd (often "/"), so every one of those lookups
// fails and the game dies during asset loading.
//
// Resolution is therefore anchored to the EXECUTABLE, not the cwd, and user
// data moves to ~/Library/Application Support/Pikmin so an app bundle stays
// read-only (and survives being replaced by a new build).
//
// _NSGetExecutablePath is used rather than SDL_GetBasePath: it is the platform
// primitive, it works before SDL_Init, and it does not depend on SDL having
// been initialised in the right video driver — the prior port's notes call this
// out specifically.

#ifdef __cplusplus
extern "C" {
#endif

// Root of the extracted disc filesystem — the directory CONTAINING dataDir.
// Search order, first hit wins:
//   1. $PC_GAME_ROOT              (dev override, unchanged)
//   2. <exe>/../Resources/gamedata (inside a .app bundle)
//   3. <exe>/gamedata              (next to a loose binary)
//   4. ./gamedata                  (cwd — the historical build-tree behaviour)
const char* pc_path_data(void);

// ~/Library/Application Support/Pikmin, created on first use. Holds saves and
// the .ini files. Falls back to "." if HOME is somehow unset.
const char* pc_path_support(void);

// Absolute path to a file inside the support directory. Returns a pointer to
// a rotating set of static buffers — copy it if you need to keep it.
const char* pc_path_support_file(const char* name);

#ifdef __cplusplus
}
#endif

#endif // PC_PATHS_H
