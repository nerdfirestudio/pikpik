// pc_config.cpp — settings.ini / keybindings.ini (M7). See pc_config.h.
//
// Fixed buffers throughout, same rule as pc_paths.cpp: this can be reached
// before the game's arena exists, and the game replaces operator new with one
// that returns NULL until then.

#include "pc_config.h"

#include "pc_paths.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PC_MAX_SETTINGS 64
#define PC_MAX_KEYS_PER_ACTION 4

// Long enough for a sync server URL with a hostname rather than an IP —
// the only value in this file that is not a small number or a key name.
#define PC_SETTING_VALUE_MAX 160

// ------------------------------------------------------------------ settings

struct PcSetting {
	char key[48];                     // "section.name"
	char value[PC_SETTING_VALUE_MAX]; // raw text; parsed on demand
};

static PcSetting s_settings[PC_MAX_SETTINGS];
static int s_nsettings;
static int s_loaded;

// Environment overrides. These predate the ini files and are what every
// existing script and headless test uses, so they must keep winning — the file
// is the layer underneath. `present` means "set at all" for the flag-style
// variables (PC_AUDIO=1), which is how they have always been tested.
struct PcEnvOverride {
	const char* key;
	const char* env;
	int flag_presence_means; // value to use when the variable is merely present
};

static const PcEnvOverride s_env_overrides[] = {
	{ "audio.enabled", "PC_AUDIO", 1 },
	{ "input.gamepad", "PC_NO_PAD", 0 },
};

// String-valued overrides. Same layering rule as the integers above: these
// exist so a headless test can point one build at a scratch server without
// editing the user's settings.ini out from under them.
struct PcEnvStrOverride {
	const char* key;
	const char* env;
};

static const PcEnvStrOverride s_env_str_overrides[] = {
	{ "sync.server", "PC_SYNC_SERVER" },
	{ "sync.key", "PC_SYNC_KEY" },
	{ "sync.device", "PC_SYNC_DEVICE" },
};

// ------------------------------------------------------------- keybindings

struct PcBinding {
	const char* action;
	const char* defaults; // comma-separated SDL key names
};

// THE canonical default bindings. They used to be spread between a scancode
// table and a run of if-statements in pc_window.mm, which meant the level read
// and the press latch could drift apart; now there is one list and both read
// from it.
static const PcBinding s_defaults[] = {
	{ "start", "Return" },
	{ "a", "Space,X" },
	{ "b", "Left Shift,C" },
	{ "x", "V" },
	{ "y", "B" },
	{ "z", "Z" },
	{ "l", "Q" },
	{ "r", "E" },
	{ "up", "Up,W" },
	{ "down", "Down,S" },
	{ "left", "Left,A" },
	{ "right", "Right,D" },
	{ "cup", "I" },
	{ "cdown", "K" },
	{ "cleft", "J" },
	{ "cright", "L" },
	{ "dup", "" },
	{ "ddown", "" },
	{ "dleft", "" },
	{ "dright", "" },
};

struct PcKeyBinding {
	char action[16];
	char keys[96];
};

static PcKeyBinding s_bindings[32];
static int s_nbindings;

// ------------------------------------------------------------------ parsing

static void pc_trim(char* s)
{
	char* p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	if (p != s) {
		memmove(s, p, strlen(p) + 1);
	}
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
		s[--n] = '\0';
	}
}

// Minimal INI: [section] headers, key = value, '#' or ';' comments.
static void pc_parse_ini(const char* path, void (*emit)(const char* section, const char* key, const char* value))
{
	FILE* f = fopen(path, "r");
	if (!f) {
		return;
	}
	char section[32] = "";
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* hash = strpbrk(line, "#;");
		if (hash) {
			*hash = '\0';
		}
		pc_trim(line);
		if (!line[0]) {
			continue;
		}
		if (line[0] == '[') {
			char* close = strchr(line, ']');
			if (close) {
				*close = '\0';
				strncpy(section, line + 1, sizeof(section) - 1);
				section[sizeof(section) - 1] = '\0';
				pc_trim(section);
			}
			continue;
		}
		char* eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char key[64], value[PC_SETTING_VALUE_MAX];
		strncpy(key, line, sizeof(key) - 1);
		key[sizeof(key) - 1] = '\0';
		strncpy(value, eq + 1, sizeof(value) - 1);
		value[sizeof(value) - 1] = '\0';
		pc_trim(key);
		pc_trim(value);
		if (key[0]) {
			emit(section, key, value);
		}
	}
	fclose(f);
}

static void pc_emit_setting(const char* section, const char* key, const char* value)
{
	if (s_nsettings >= PC_MAX_SETTINGS) {
		return;
	}
	PcSetting* s = &s_settings[s_nsettings++];
	snprintf(s->key, sizeof(s->key), "%s.%s", section, key);
	strncpy(s->value, value, sizeof(s->value) - 1);
	s->value[sizeof(s->value) - 1] = '\0';
}

static void pc_emit_binding(const char* section, const char* key, const char* value)
{
	(void)section;
	if (s_nbindings >= (int)(sizeof(s_bindings) / sizeof(s_bindings[0]))) {
		return;
	}
	PcKeyBinding* b = &s_bindings[s_nbindings++];
	strncpy(b->action, key, sizeof(b->action) - 1);
	b->action[sizeof(b->action) - 1] = '\0';
	strncpy(b->keys, value, sizeof(b->keys) - 1);
	b->keys[sizeof(b->keys) - 1] = '\0';
}

// ------------------------------------------------------------ default files

static int pc_file_exists(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static void pc_write_default_settings(const char* path)
{
	FILE* f = fopen(path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "# Pikmin — settings\n"
	           "# Written automatically on first run. Delete this file to get it back.\n"
	           "# Environment variables (PC_AUDIO, PC_NO_PAD, ...) override anything here.\n"
	           "\n"
	           "[video]\n"
	           "# Window size is scale x the GameCube's 640x480.\n"
	           "scale      = 2\n"
	           "fullscreen = 0\n"
	           "\n"
	           "[audio]\n"
	           "enabled = 1\n"
	           "\n"
	           "[input]\n"
	           "# 0 disables controllers and leaves the keyboard active.\n"
	           "gamepad = 1\n"
	           "\n"
	           "[sync]\n"
	           "# Save sync across your own devices (see docs/SAVE_SYNC.md).\n"
	           "# Both server and key must be set, and IDENTICAL on every device\n"
	           "# that should share the save — that is the whole pairing step.\n"
	           "#\n"
	           "# server: base URL of the sync server. Empty = sync off.\n"
	           "# key:    any phrase you invent. It IS the credential: the server\n"
	           "#         never sees it, but anyone who has it has your save.\n"
	           "# device: name shown when another device holds the save. Defaults\n"
	           "#         to this machine's hostname.\n"
	           "server = \n"
	           "key    = \n"
	           "device = \n");
	fclose(f);
}

static void pc_write_default_keys(const char* path)
{
	FILE* f = fopen(path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "# Pikmin — keyboard bindings\n"
	           "# Written automatically on first run. Delete this file to get it back.\n"
	           "#\n"
	           "# One action per line; several keys may share an action, comma separated.\n"
	           "# Names are SDL key names — \"Space\", \"Return\", \"Left Shift\", \"A\", \"Up\".\n"
	           "# An empty value unbinds the action. A controller, if present, is always\n"
	           "# live alongside the keyboard and is not configured here.\n"
	           "#\n"
	           "# Pikmin really wants the analog triggers and both sticks, so a gamepad\n"
	           "# plays considerably better than the keyboard whatever you bind.\n"
	           "\n"
	           "[buttons]\n");
	for (unsigned i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
		if (i == 8) {
			fprintf(f, "\n[stick]\n");
		}
		if (i == 12) {
			fprintf(f, "\n# C-stick — camera.\n");
		}
		if (i == 16) {
			fprintf(f, "\n# D-pad. Unbound by default; the stick covers movement.\n");
		}
		fprintf(f, "%-7s = %s\n", s_defaults[i].action, s_defaults[i].defaults);
	}
	fclose(f);
}

// ------------------------------------------------------------------- public

void pc_config_load(void)
{
	if (s_loaded) {
		return;
	}
	s_loaded = 1;

	const char* settings_path = pc_path_support_file("settings.ini");
	if (!pc_file_exists(settings_path)) {
		pc_write_default_settings(settings_path);
	}
	pc_parse_ini(settings_path, pc_emit_setting);

	// pc_path_support_file rotates a small buffer set, so re-fetch rather than
	// hold the earlier pointer across the calls above.
	const char* keys_path = pc_path_support_file("keybindings.ini");
	if (!pc_file_exists(keys_path)) {
		pc_write_default_keys(keys_path);
	}
	pc_parse_ini(keys_path, pc_emit_binding);
}

int pc_config_int(const char* key, int fallback)
{
	pc_config_load();

	for (unsigned i = 0; i < sizeof(s_env_overrides) / sizeof(s_env_overrides[0]); i++) {
		if (strcmp(s_env_overrides[i].key, key) != 0) {
			continue;
		}
		const char* v = getenv(s_env_overrides[i].env);
		if (v) {
			return *v ? atoi(v) : s_env_overrides[i].flag_presence_means;
		}
	}

	for (int i = 0; i < s_nsettings; i++) {
		if (strcmp(s_settings[i].key, key) == 0) {
			return atoi(s_settings[i].value);
		}
	}
	return fallback;
}

const char* pc_config_str(const char* key, const char* fallback)
{
	pc_config_load();

	for (unsigned i = 0; i < sizeof(s_env_str_overrides) / sizeof(s_env_str_overrides[0]); i++) {
		if (strcmp(s_env_str_overrides[i].key, key) != 0) {
			continue;
		}
		const char* v = getenv(s_env_str_overrides[i].env);
		if (v) {
			return v; // environ strings live as long as the process
		}
	}

	for (int i = 0; i < s_nsettings; i++) {
		if (strcmp(s_settings[i].key, key) == 0) {
			// An empty value means "not configured", not "the empty string":
			// `sync_server = ` is how the shipped file spells OFF, and every
			// caller wants its own default in that case.
			return s_settings[i].value[0] ? s_settings[i].value : fallback;
		}
	}
	return fallback;
}

int pc_config_keys(const char* action, int* out, int max)
{
	pc_config_load();

	const char* list = NULL;
	for (int i = 0; i < s_nbindings; i++) {
		if (strcmp(s_bindings[i].action, action) == 0) {
			list = s_bindings[i].keys;
			break;
		}
	}
	if (!list) {
		for (unsigned i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
			if (strcmp(s_defaults[i].action, action) == 0) {
				list = s_defaults[i].defaults;
				break;
			}
		}
	}
	if (!list || !*list) {
		return 0;
	}

	int n = 0;
	char buf[96];
	strncpy(buf, list, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	for (char* tok = strtok(buf, ","); tok && n < max; tok = strtok(NULL, ",")) {
		pc_trim(tok);
		if (!*tok) {
			continue;
		}
		const SDL_Scancode sc = SDL_GetScancodeFromName(tok);
		if (sc == SDL_SCANCODE_UNKNOWN) {
			fprintf(stderr, "[PC] keybindings.ini: '%s' is not an SDL key name (action '%s')\n", tok, action);
			continue;
		}
		out[n++] = (int)sc;
	}
	return n;
}
