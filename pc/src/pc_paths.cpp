// pc_paths.cpp — executable-anchored path resolution (M7). See pc_paths.h for
// why the cwd cannot be trusted once the game ships as a bundle.
//
// NOTHING HERE ALLOCATES. This runs arbitrarily early — pc_card_selftest() is
// called from main() before gsys->Initialise(), so the arena does not exist
// yet — and the game REPLACES operator new (sysNew.cpp, where delete is a
// no-op). Before the arena is up that allocator returns NULL, and because the
// build is -fno-exceptions std::string does not throw on failure: it quietly
// memmoves into a null pointer. A std::string in this file therefore crashes in
// libsystem_platform`_platform_memmove with a null destination, which reads as
// a mysterious libc fault rather than an allocation failure. Fixed buffers.

#include "pc_paths.h"

#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// The application's own name, and so the name of its user-data directory. The
// build defines it (pc/CMakeLists.txt, PC_APP_NAME); the fallback keeps this
// file compilable on its own. Changing it moves existing saves.
#ifndef PC_APP_NAME
#define PC_APP_NAME "Pikmin"
#endif
#include <sys/types.h>
#include <unistd.h>

#define PC_PATH_MAX 2048

// Directory containing the running executable, with symlinks resolved.
//
// _NSGetExecutablePath can hand back a path containing "..", a symlink, or a
// relative component, so realpath() is not optional — getting it wrong is
// silent, yielding a plausible directory that just does not hold the resources.
static const char* pc_exe_dir(void)
{
	static char dir[PC_PATH_MAX];
	static int done;
	if (done) {
		return dir;
	}
	done = 1;

	char raw[PC_PATH_MAX];
	uint32_t size = sizeof(raw);
	if (_NSGetExecutablePath(raw, &size) != 0) {
		dir[0] = '\0'; // callers fall through to the cwd-relative default
		return dir;
	}

	char resolved[PC_PATH_MAX];
	const char* full = realpath(raw, resolved) ? resolved : raw;

	strncpy(dir, full, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	char* slash          = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
	} else {
		strcpy(dir, ".");
	}
	return dir;
}

static int pc_is_dir(const char* p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p. Returns non-zero if the leaf exists as a directory afterwards.
static int pc_mkdirs(const char* path)
{
	char acc[PC_PATH_MAX];
	size_t n = 0;
	for (size_t i = 0; path[i] && n < sizeof(acc) - 1; i++) {
		acc[n++] = path[i];
		if (path[i] == '/' && n > 1) {
			acc[n - 1] = '\0';
			if (!pc_is_dir(acc)) {
				mkdir(acc, 0755);
			}
			acc[n - 1] = '/';
		}
	}
	acc[n] = '\0';
	if (!pc_is_dir(acc)) {
		mkdir(acc, 0755);
	}
	return pc_is_dir(acc);
}

const char* pc_path_data(void)
{
	static char root[PC_PATH_MAX];
	static int done;
	if (done) {
		return root;
	}
	done = 1;

	// An EMPTY PC_GAME_ROOT counts as unset: `PC_GAME_ROOT= ./Pikmin` is a
	// natural way to try to clear it, and honouring "" would resolve every
	// asset against the filesystem root.
	const char* env = getenv("PC_GAME_ROOT");
	if (env && *env) {
		strncpy(root, env, sizeof(root) - 1);
		return root;
	}

#if TARGET_OS_IPHONE
	// iOS: the sandbox is the only writable place, and Documents/ is the one
	// the user can reach — UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace
	// (see the Xcode shell's Info.plist) expose it to the Files app, which is
	// how the disc data gets onto the device at all. HOME is the container
	// root, so this needs no Foundation call and stays allocation-free.
	//
	// This must be checked BEFORE the executable-relative branches below: on
	// iOS the bundle is flat, so "beside the executable" resolves INSIDE the
	// signed .app — readable, but the wrong answer for user-supplied data.
	{
		const char* home = getenv("HOME");
		if (home && *home) {
			char cand[PC_PATH_MAX];
			snprintf(cand, sizeof(cand), "%s/Documents/gamedata", home);
			if (pc_is_dir(cand)) {
				strncpy(root, cand, sizeof(root) - 1);
				fprintf(stderr, "[PC] game data: %s (iOS Documents)\n", root);
				return root;
			}
			// Create it empty so the folder is VISIBLE in the Files app on a
			// fresh install, and return it regardless: on iOS this IS the data
			// root, populated or not. Falling through to the executable-relative
			// branches would report missing assets against a path inside the
			// .app, which reads as a broken build rather than "you have not
			// imported the game yet".
			pc_mkdirs(cand);
			strncpy(root, cand, sizeof(root) - 1);
			fprintf(stderr, "[PC] game data: %s (iOS Documents, EMPTY — import the disc data via the Files app)\n", root);
			return root;
		}
	}
#endif

	const char* exe = pc_exe_dir();
	if (*exe) {
		char cand[PC_PATH_MAX];

		// Inside a bundle the binary sits in Contents/MacOS, so the resources
		// are one level up in Contents/Resources.
		snprintf(cand, sizeof(cand), "%s/../Resources/gamedata", exe);
		if (pc_is_dir(cand)) {
			char resolved[PC_PATH_MAX];
			const char* full = realpath(cand, resolved) ? resolved : cand;
			strncpy(root, full, sizeof(root) - 1);
			fprintf(stderr, "[PC] game data: %s (bundle)\n", root);
			return root;
		}

		// gamedata.path: a one-line text file holding an absolute path to the
		// data. A SYMLINK would be the obvious way to avoid copying 640 MB into
		// the bundle, but codesign rejects a symlink whose destination escapes
		// the bundle ("invalid destination for symbolic link in bundle"), which
		// leaves the app unsignable. A plain file is ordinary signable content
		// and costs nothing.
		snprintf(cand, sizeof(cand), "%s/../Resources/gamedata.path", exe);
		FILE* pf = fopen(cand, "r");
		if (pf) {
			char line[PC_PATH_MAX];
			if (fgets(line, sizeof(line), pf)) {
				size_t n = strlen(line);
				while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
					line[--n] = '\0';
				}
				if (n && pc_is_dir(line)) {
					fclose(pf);
					strncpy(root, line, sizeof(root) - 1);
					fprintf(stderr, "[PC] game data: %s (bundle pointer)\n", root);
					return root;
				}
			}
			fclose(pf);
		}

		snprintf(cand, sizeof(cand), "%s/gamedata", exe);
		if (pc_is_dir(cand)) {
			strncpy(root, cand, sizeof(root) - 1);
			fprintf(stderr, "[PC] game data: %s (beside executable)\n", root);
			return root;
		}
	}

	strcpy(root, "gamedata"); // historical build-tree behaviour
	return root;
}

const char* pc_path_support(void)
{
	static char dir[PC_PATH_MAX];
	static int done;
	if (done) {
		return dir;
	}
	done = 1;

	// Explicit override wins, same shape as PC_GAME_ROOT / PC_CARD_DIR.
	const char* env = getenv("PC_CONFIG_DIR");
	if (env && *env) {
		strncpy(dir, env, sizeof(dir) - 1);
		pc_mkdirs(dir);
		fprintf(stderr, "[PC] user data: %s (PC_CONFIG_DIR)\n", dir);
		return dir;
	}

#if TARGET_OS_IPHONE
	// iOS: Documents/ again, so the memory-card file sits beside the imported
	// gamedata — visible in the Files app (so a save can be pulled off the
	// device) and included in device backups. The "portable when loose" branch
	// below is actively WRONG here: on iOS the bundle is flat, so it would
	// resolve inside the signed .app, where every write fails.
	{
		const char* home = getenv("HOME");
		if (home && *home) {
			snprintf(dir, sizeof(dir), "%s/Documents", home);
			if (!pc_mkdirs(dir)) {
				fprintf(stderr, "[PC] could not create %s — falling back to the working directory\n", dir);
				strcpy(dir, ".");
			} else {
				fprintf(stderr, "[PC] user data: %s (iOS Documents)\n", dir);
			}
			return dir;
		}
	}
#endif

	// PORTABLE WHEN LOOSE. A loose build keeps its settings, keybindings and
	// saves right next to the binary, so a development tree is one directory
	// rather than split between build-m1/ and Application Support.
	//
	// A BUNDLE cannot do that: writing inside a .app invalidates its code
	// signature, and bundles are meant to be read-only. So when the executable
	// lives in <something>.app/Contents/MacOS we use the macOS-conventional
	// location instead. That is the whole reason for the split — not taste.
	const char* exe = pc_exe_dir();
	int bundled     = 0;
	if (*exe) {
		const size_t n   = strlen(exe);
		const char* tail = "/Contents/MacOS";
		const size_t t   = strlen(tail);
		bundled          = (n >= t && strcmp(exe + n - t, tail) == 0);
	}

	if (!bundled && *exe) {
		strncpy(dir, exe, sizeof(dir) - 1);
		fprintf(stderr, "[PC] user data: %s (portable — beside the executable)\n", dir);
		return dir;
	}

	const char* home = getenv("HOME");
	if (!home || !*home) {
		strcpy(dir, "."); // no HOME (launchd with a stripped environment)
		return dir;
	}

	snprintf(dir, sizeof(dir), "%s/Library/Application Support/%s", home, PC_APP_NAME);
	if (!pc_mkdirs(dir)) {
		fprintf(stderr, "[PC] could not create %s — falling back to the working directory\n", dir);
		strcpy(dir, ".");
	} else {
		fprintf(stderr, "[PC] user data: %s (bundled)\n", dir);
	}
	return dir;
}

const char* pc_path_support_file(const char* name)
{
	// Rotating buffers so two calls can be live in one expression.
	static char bufs[4][PC_PATH_MAX];
	static int next;
	char* out = bufs[next];
	next      = (next + 1) & 3;
	snprintf(out, PC_PATH_MAX, "%s/%s", pc_path_support(), name ? name : "");
	return out;
}
