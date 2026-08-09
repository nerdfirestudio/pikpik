// pc_ios_main.cpp — iOS entry-point bridge. Built only when PC_IOS.
//
// SDL2's UIKit backend must own UIApplicationMain, so the Xcode shell's
// main.m calls SDL_UIKitRunApp(argc, argv, SDL_main) and the game's entry
// point has to be reachable under the name SDL_main.
//
// The obvious route — -Dmain=SDL_main on the file that defines main, which is
// what the prior port port does — does NOT work here. There, main() lives in a
// .c file, so the renamed symbol is plain `_SDL_main`. Pikmin's main() lives
// in sysBootup.cpp, which is C++: the rename produced `__Z8SDL_mainiPPc`
// (mangled), while main.m's `extern int SDL_main(...)` looks for `_SDL_main`
// with C linkage. Silent-ish link failure, and the kind that reads as "SDL is
// wired up wrong" rather than "the name is mangled".
//
// So sysBootup.cpp is renamed to an ordinary C++ function
// (-Dmain=pc_ios_game_main) and this file provides the extern "C" wrapper.
// Calling main() directly would be the shorter path, but [basic.start.main]
// forbids a program from calling its own main, so the rename is what keeps
// this well-formed rather than merely working.

extern int pc_ios_game_main(int argc, char* argv[]); // sysBootup.cpp, renamed

extern "C" int SDL_main(int argc, char* argv[])
{
	return pc_ios_game_main(argc, argv);
}
