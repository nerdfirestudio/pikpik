#ifndef PC_CONFIG_H
#define PC_CONFIG_H

// M7: settings.ini and keybindings.ini, both in
// ~/Library/Application Support/Pikmin (see pc_paths.h).
//
// Up to M6 every knob was an environment variable, which is fine for a build
// tree and useless for a shipped .app — you cannot set PC_AUDIO=1 by
// double-clicking. Environment variables still WIN over the files, so every
// existing script and headless test keeps working unchanged; the files are the
// default layer underneath them.
//
// Both files are written out with their defaults (and comments) on first run,
// so they are discoverable rather than something you have to be told about.

#ifdef __cplusplus
extern "C" {
#endif

// Parses both files. Idempotent, and safe to call before SDL_Init.
void pc_config_load(void);

// "section.key" lookup, e.g. pc_config_int("video.scale", 2).
// Resolution order: environment override (see the table in pc_config.cpp),
// then settings.ini, then the supplied default.
int pc_config_int(const char* key, int fallback);

// Same lookup, string-valued — e.g. pc_config_str("sync.server", "").
// The returned pointer is either `fallback` or a pointer into the settings
// table, which lives for the process; NEVER a temporary. Values are trimmed
// and capped at PC_SETTING_VALUE_MAX-1 characters.
//
// Note the ini parser truncates a line at its first '#' or ';', so a value
// that needs either character cannot be expressed in this file format. That
// is fine for the keys here (URLs, names) and is why there is no escaping.
const char* pc_config_str(const char* key, const char* fallback);

// Keyboard bindings for one action. Writes up to `max` SDL scancodes and
// returns how many. Actions:
//   buttons: a b x y z l r start
//   dpad:    dup ddown dleft dright
//   stick:   up down left right
//   c-stick: cup cdown cleft cright
// Returns the built-in default binding when the file does not mention the
// action, so callers never need their own fallback table.
int pc_config_keys(const char* action, int* out_scancodes, int max);

#ifdef __cplusplus
}
#endif

#endif // PC_CONFIG_H
