/* Thin iOS shell — the entire game lives in the static libs built by
 * build_ios.sh (libPikmin.a / libpikmin_core.a / libpikmin_pc.a).
 *
 * SDL2's UIKit backend must own the application object, so this main() hands
 * off to SDL_UIKitRunApp, which installs SDL's UIApplicationDelegate and then
 * calls SDL_main. SDL_main is pc_ios_main.cpp's extern "C" wrapper around
 * sysBootup.cpp's main() (renamed to pc_ios_game_main by the iOS CMake
 * branch) — see pc/src/pc_ios_main.cpp for why the wrapper exists.
 *
 * There is deliberately no SwiftUI here: an @main App type would claim
 * UIApplicationMain, which SDL needs. See docs/BUILDING_IOS.md.
 */
#define SDL_MAIN_HANDLED /* keep SDL_main.h from renaming THIS main */
#include <SDL.h>
#include <SDL_main.h>
#include <stdlib.h>

extern int SDL_main(int argc, char *argv[]); /* pc_ios_main.cpp */

int main(int argc, char *argv[]) {
    /* iOS: SDL's CoreAudio backend defaults the AVAudioSession category to
     * "ambient", which is silenced by the mute switch / Control Center mute
     * and stops on screen lock. "playback" is the game-audio category: it
     * plays regardless of the mute setting. Must be set before audio init. */
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

    /* Defer the home-indicator swipe (the first swipe reveals the bar, the
     * second acts) so bottom-edge touches don't background the game. */
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");

    /* Bring-up: the port layer's logging is env-gated (PC_VERBOSE, read in
     * pc_sdk_shims.cpp), not argv-gated, so route it on here to get engine
     * logs into the Xcode console. Drop once the iPad build is stable.
     * Other useful knobs to set the same way, or in the Xcode scheme:
     *   PC_SKIP_INTRO=1   boot straight to gameplay
     *   PC_GAME_ROOT=...   override the Documents/gamedata default */
    setenv("PC_VERBOSE", "1", 0);

    /* The game ignores argc/argv entirely (sysBootup.cpp takes them and never
     * reads them), so pass them straight through rather than synthesizing. */
    return SDL_UIKitRunApp(argc, argv, SDL_main);
}
