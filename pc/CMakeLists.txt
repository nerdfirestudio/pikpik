# pikpik host build.
# M1 scope: compile the decompiled game code (C++ game + jaudio + hvqm4dec)
# for the host with clang. SDK libs / CodeWarrior runtime / MSL / TRK are
# excluded — their APIs will be provided by pc_* shims (M2+).
cmake_minimum_required(VERSION 3.20)
project(pikmin_metal C CXX)

option(PC_O2 "Build the decomp at -O2 instead of -O0 (docs/ARCHITECTURE.md)" OFF)
option(PC_UBSAN "UBSan on the decomp, log-and-continue" OFF)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Where the decompiled game source lives — the directory holding src/ and
# include/. Two layouts use this build:
#
#   this tree            the decomp sits at the repo root, so the default wins
#   published tree       only the port layer is distributed; setup.sh fetches
#                        the decomp into vendor/pikmin and the build scripts
#                        pass -DPIKMIN_DECOMP_DIR=<repo>/vendor/pikmin
#
# Keeping it one variable is what lets both layouts share this file verbatim.
set(PIKMIN_DECOMP_DIR "${REPO_ROOT}" CACHE PATH
    "Directory containing src/ and include/ of the Pikmin decompilation")

# The application's own name: the executable/target, the macOS bundle, and the
# user-data directory under ~/Library/Application Support. Held in one place
# because the published build ships under a different name than this tree —
# see docs/LEGAL.md on trademark. Changing it moves the save directory.
set(PC_APP_NAME "Pikmin" CACHE STRING "Name of the built application and target")

if(NOT EXISTS "${PIKMIN_DECOMP_DIR}/src/sysBootup.cpp")
    message(FATAL_ERROR
        "No decompiled game source at ${PIKMIN_DECOMP_DIR}\n"
        "Expected ${PIKMIN_DECOMP_DIR}/src and ${PIKMIN_DECOMP_DIR}/include.\n"
        "In the published tree, run ./setup.sh first — it fetches the\n"
        "decompilation and applies this port's patches. See docs/LEGAL.md\n"
        "for why the game source is not distributed with this repository.")
endif()

# --- iOS cross-build (configure with -DCMAKE_SYSTEM_NAME=iOS) ---------------
# The engine builds as STATIC LIBRARIES instead of an executable; the Xcode
# shell project (Pikmin/Pikmin.xcodeproj) links them and owns signing, the
# Info.plist, and deployment. main() is renamed to SDL_main so SDL2's UIKit
# backend can own UIApplicationMain. See docs/BUILDING_IOS.md.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PC_IOS TRUE)
    set(PC_IOS_SDL2_PREFIX "${REPO_ROOT}/ios/sdl2-ios"
        CACHE PATH "Install prefix of the iOS static SDL2 build")
    if(NOT EXISTS "${PC_IOS_SDL2_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "iOS static SDL2 not found at ${PC_IOS_SDL2_PREFIX}\n"
            "Run build_ios.sh first — it builds/installs ios/sdl2-ios.")
    endif()
else()
    set(PC_IOS FALSE)
endif()

# --- Flags ------------------------------------------------------------------
# -funsigned-char: MWCC built with `-char unsigned`; Apple arm64 defaults
#   signed. Behavior-critical, not cosmetic.
# -fwrapv -fno-strict-aliasing: neutralize decomp UB (signed overflow,
#   type punning). NOTE: does NOT cover out-of-range float->int (brk #1
#   class) — see docs/ARCHITECTURE.md.
# -O0 for bring-up; -O2 is earned later by fixing UB families, not by luck.
set(PC_COMMON_FLAGS
    -funsigned-char
    -fwrapv
    -fno-strict-aliasing
    # Keep every C++ symbol out of dyld's weak-coalescing pass: the game
    # exports a no-op operator delete and -funsigned-char-flavored inline
    # instantiations, and letting system frameworks (Metal -> MLAssetIO)
    # bind those crashed their own initializers. Hidden = the game keeps
    # its allocator internally, the system keeps libc++'s.
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -g
    -ferror-limit=100
    # --- Layer 1: define the UB away (docs/ARCHITECTURE.md) -----------------------
    # Not UB, but required for the -O0/-O2 A/B oracle: without it clang is
    # free to fuse a*b+c into one fma, and a fused result differs in the last
    # bit from the -O0 build's separate mul+add. Bit-identical FP is what lets
    # the oracle call a frame diff a MISCOMPILE rather than rounding.
    # (GC's PPC had fused madd, so contraction ON is arguably more faithful —
    # that is a later, separate A/B; the verified baseline is this renderer.)
    -ffp-contract=off
    # The GameCube maps physical memory at address 0, so a "null" deref in the
    # decomp can be benign — and clang's default is to infer from a deref that
    # the pointer cannot be null and then DELETE the null check that follows.
    # That inference is wrong for this target, not merely risky.
    -fno-delete-null-pointer-checks
    # C++11 onward lets the optimizer assume a side-effect-free loop
    # terminates; the decomp has empty spin/wait loops that only exit through
    # a hardware register or another thread. gnu++03 should not be applying
    # the C++11 rule at all — this pins that against clang-version drift.
    -fno-finite-loops
    # Out-of-range float->int is UB in C++ and clang exploits it — this is the
    # documented `brk #1` class (docs/ARCHITECTURE.md), the one thing
    # -fwrapv does NOT cover. With this flag the cast lowers to arm64's native
    # saturating fcvtzs, which is what PPC's fctiwz did. Known divergence,
    # recorded rather than chased: NaN converts to 0 on arm64 but 0x80000000
    # on PPC.
    -fno-strict-float-cast-overflow
    # Every uninitialized stack read becomes a deterministic zero instead of
    # whatever the stack frame happened to hold. This covers the UNENUMERATED
    # sites; the 16 the census found are still read and fixed individually,
    # because zero is deterministic but not necessarily the value MWCC's
    # stack held. Determinism is what the -O0/-O2 oracle needs — an
    # uninitialized read is a legitimate source of frame divergence that has
    # nothing to do with a miscompile.
    -ftrivial-auto-var-init=zero
    # MWCC allowed &TemporaryObject(...); every use is at least an audit
    # item (block-scope-escape family) but suppressing gets us compiling —
    # re-enable to enumerate the sites.
    -Wno-address-of-temporary
    # MSL-era gaps (math constants, __frsqrte, std:: float functions):
    -include ${CMAKE_CURRENT_SOURCE_DIR}/include/pc_host_compat.h
)

# The decomp itself builds with warnings off ("-w off"), and bring-up kept
# that. It hides exactly the UB families that become MISCOMPILES at -O2, so
# the blanket -w is replaced by a curated always-on set (docs/ARCHITECTURE.md
# Layer 2). The whole set costs 74 findings across 444 TUs — small enough to
# stay on permanently, which is the point: these classes must not come back.
# On Apple clang a plain -w BEATS later -W flags, so the base is
# -Wno-everything, not -Wall.
list(APPEND PC_COMMON_FLAGS
    -Wno-everything
    # Fall-through out of a non-void function. At -O0 it returns register
    # residue; at -O2 clang marks the path unreachable and may delete it AND
    # the caller's downstream code. Was the #1 -O2 blocker at 41 sites; now
    # zero, and an ERROR so it can never come back. Non-void stubs use
    # TRAP_UNIMPLEMENTED_RET (include/types.h).
    -Werror=return-type
    -Wuninitialized -Wsometimes-uninitialized
    -Warray-bounds -Winfinite-recursion
    # The LP64 audit (was the PC_DIAG_LP64 option — folded in, since a 32-bit
    # slot carrying a pointer is this port's #1 bug class). pointer->int is
    # already clean at 0 hits; int->pointer is Work-list C.
    -Wpointer-to-int-cast -Wint-to-pointer-cast)

# --- Optimization level -----------------------------------------------------
# -O0 is the bring-up default and the trusted reference build.
# -O2 is EARNED, not assumed: with Layer 1 above defining the UB away and the
# census classes fixed, PC_O2=ON builds the candidate that the frame-dump
# oracle A/Bs against the -O0 reference. Keep -g in both so a divergence is
# debuggable.
if(PC_O2)
    list(APPEND PC_COMMON_FLAGS -O2)
else()
    list(APPEND PC_COMMON_FLAGS -O0)
endif()

# --- Layer 3: the UBSan soak ------------------------------------------------
# Log-and-continue, so one report does not end the run and a whole play
# session yields a full census of signatures.
#   UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:log_path=<dir>/ubsan
# alignment off: arm64 tolerates unaligned access and the big-endian overlay
#   casts all over this decomp would be pure noise.
# function/vptr off: gnu++03 decomp plus a replaced allocator make them
#   noisy and irrelevant.
# UBSan respects -fwrapv and -fno-strict-float-cast-overflow above, so what
# survives (shifts, OOB indexing, bool/enum loads) is real signal.
# ASan is deliberately NOT offered: the game's arena allocator (sysNew) hides
# object bounds from it.
if(PC_UBSAN)
    list(APPEND PC_COMMON_FLAGS
        -fsanitize=undefined
        # local-bounds is NOT part of -fsanitize=undefined and has to be asked
        # for by name. It is the runtime form of the check that found the one
        # real bug of the first soak (animMgr.cpp:357, a 4-byte stack
        # overwrite that -O2 would have broken), so it earns its place: it
        # catches the OOB accesses -Warray-bounds cannot see statically.
        -fsanitize=local-bounds
        -fno-sanitize=alignment,function,vptr
        -fsanitize-recover=all
        -fno-omit-frame-pointer)
endif()

set(PC_DEFINES
    VERSION_GPIE01_01     # USA Rev 1 — the 100%-matched target
    BUILD_VERSION=5       # index of GPIE01_01 in the decomp's VERSIONS list
    NDEBUG=1
    TARGET_PC=1
    PC_APP_NAME="${PC_APP_NAME}"
)

# --- Sources ----------------------------------------------------------------
# Game code + engine glue + audio + FMV decoder. Everything else in src/
# (SDK libs, Runtime, MSL_C, TRK, EXI debug stubs) is intentionally absent:
# those are the APIs the pc_* port layer implements.
file(GLOB_RECURSE PIKMIN_GAME_SOURCES CONFIGURE_DEPENDS
    "${PIKMIN_DECOMP_DIR}/src/plugPikiColin/*.cpp"
    "${PIKMIN_DECOMP_DIR}/src/plugPikiKando/*.cpp"
    "${PIKMIN_DECOMP_DIR}/src/plugPikiNakata/*.cpp"
    "${PIKMIN_DECOMP_DIR}/src/plugPikiNishimura/*.cpp"
    "${PIKMIN_DECOMP_DIR}/src/plugPikiOgawa/*.cpp"
    "${PIKMIN_DECOMP_DIR}/src/plugPikiYamashita/*.cpp"
    "${PIKMIN_DECOMP_DIR}/src/sysCommon/*.cpp"
    "${PIKMIN_DECOMP_DIR}/src/sysDolphin/*.cpp"
)
list(APPEND PIKMIN_GAME_SOURCES "${PIKMIN_DECOMP_DIR}/src/sysBootup.cpp")

file(GLOB PIKMIN_C_SOURCES CONFIGURE_DEPENDS
    "${PIKMIN_DECOMP_DIR}/src/jaudio/*.c"
    "${PIKMIN_DECOMP_DIR}/src/hvqm4dec/*.c"
)

# main() lives in sysBootup.cpp — link it into the executable, not the
# archive, so the entry point is always pulled in.
list(REMOVE_ITEM PIKMIN_GAME_SOURCES "${PIKMIN_DECOMP_DIR}/src/sysBootup.cpp")

# The decomp's cflags_base passes `-lang c++` to MWCC: every .c file
# (jaudio, hvqm4dec, SDK) is compiled as C++ in the original build, and some
# (jaudio/pikidemo.c) include C++ headers. Mirror that.
set_source_files_properties(${PIKMIN_C_SOURCES} PROPERTIES LANGUAGE CXX)

add_library(pikmin_core STATIC ${PIKMIN_GAME_SOURCES} ${PIKMIN_C_SOURCES})

target_include_directories(pikmin_core PUBLIC
    "${PIKMIN_DECOMP_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(pikmin_core PUBLIC ${PC_DEFINES})
target_compile_options(pikmin_core PRIVATE ${PC_COMMON_FLAGS})

# MWCC 1.2.5 C++ is a pre-C++11 dialect; gnu++03 keeps clang's expectations
# aligned (and types.h's `#define nullptr 0` fallback active).
target_compile_options(pikmin_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++03 -fno-exceptions>
)

# --- Port layer + executable ------------------------------------------------
# The pc_* shims are modern C++ (std::map etc.) — no gnu++03 restriction.
add_library(pikmin_pc STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_sdk_shims.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_card.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_gx_stub.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_window.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_audio.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_dsp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_paths.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_sync.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_sync_net.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_crash.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_input_rec.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_touch_pad.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_touch_draw.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_uninit.cpp"
)
if(PC_IOS)
    # UIKit replaces Cocoa; the rest of the frameworks exist on both. The
    # trailing set is what static SDL2 itself pulls in (its own cmake config
    # lists them) — on desktop the Homebrew dylib carries them implicitly.
    target_include_directories(pikmin_pc PUBLIC
        "${PC_IOS_SDL2_PREFIX}/include/SDL2" "${PC_IOS_SDL2_PREFIX}/include")
    target_link_libraries(pikmin_pc PUBLIC
        "${PC_IOS_SDL2_PREFIX}/lib/libSDL2.a"
        "-framework Metal" "-framework QuartzCore" "-framework UIKit"
        "-framework ImageIO" "-framework CoreGraphics" "-framework Foundation"
        "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio"
        "-framework CoreBluetooth" "-framework CoreMotion" "-framework CoreVideo"
        "-framework OpenGLES"
        "-weak_framework GameController" "-weak_framework CoreHaptics")
else()
find_library(PC_METAL Metal REQUIRED)
find_library(PC_QUARTZ QuartzCore REQUIRED)
find_library(PC_COCOA Cocoa REQUIRED)
find_library(PC_IMAGEIO ImageIO REQUIRED)
find_library(PC_COREGFX CoreGraphics REQUIRED)
target_include_directories(pikmin_pc PRIVATE /opt/homebrew/include/SDL2)
target_link_directories(pikmin_pc PUBLIC /opt/homebrew/lib)
target_link_libraries(pikmin_pc PUBLIC SDL2 ${PC_METAL} ${PC_QUARTZ} ${PC_COCOA} ${PC_IMAGEIO} ${PC_COREGFX})
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/pc_window.mm"
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-O2")
# The per-vertex CPU T&L path (pc_cap_emit_vertex) scales with on-screen
# pikmin; -O2 on the GX layer only. The "-O2 is earned" caveat above is about
# the decomp's UB, not this modern port code; the other pc_* files stay -O0
# with the rest of the bring-up build. (Source-level COMPILE_OPTIONS land
# after the target's -O0, so the -O2 here wins.)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/pc_gx_stub.cpp"
    PROPERTIES COMPILE_OPTIONS "-O2")
target_include_directories(pikmin_pc PRIVATE "${PIKMIN_DECOMP_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(pikmin_pc PRIVATE ${PC_DEFINES})
target_compile_options(pikmin_pc PRIVATE -funsigned-char -fwrapv -fno-strict-aliasing -fvisibility=hidden -fvisibility-inlines-hidden -g -O0 -Wall
    -include ${CMAKE_CURRENT_SOURCE_DIR}/include/pc_host_compat.h)

# Note: the game defines its own static `System sys;` in system.cpp, which
# sets gsys during static init — no pc-side boot object is needed.
if(PC_IOS)
# --- iOS: the entry point becomes SDL_main inside a static lib --------------
# sysBootup.cpp's main() is renamed by the preprocessor and pc_ios_main.cpp
# provides the extern "C" SDL_main that the Xcode shell's main.m calls (see
# that file for why the rename does not go straight to SDL_main: C++ mangling).
# main.m's reference to _SDL_main is also what pulls these archive members in.
set_source_files_properties("${PIKMIN_DECOMP_DIR}/src/sysBootup.cpp"
    PROPERTIES COMPILE_DEFINITIONS "main=pc_ios_game_main")
# Same target NAME in both branches so every target_* call below is shared;
# on iOS it just archives instead of linking (libPikmin.a).
add_library(${PC_APP_NAME} STATIC
    "${PIKMIN_DECOMP_DIR}/src/sysBootup.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_ios_main.cpp")
foreach(t ${PC_APP_NAME} pikmin_core pikmin_pc)
    set_target_properties(${t} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endforeach()
else()
add_executable(${PC_APP_NAME}
    "${PIKMIN_DECOMP_DIR}/src/sysBootup.cpp"
)
endif()
target_include_directories(${PC_APP_NAME} PRIVATE "${PIKMIN_DECOMP_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(${PC_APP_NAME} PRIVATE ${PC_DEFINES})
target_compile_options(${PC_APP_NAME} PRIVATE ${PC_COMMON_FLAGS}
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++03 -fno-exceptions>)
# Game archive twice around the shims so cross-references resolve either way.
# iOS: pthread is inside libSystem and there is no libpthread to name.
if(PC_IOS)
    target_link_libraries(${PC_APP_NAME} pikmin_core pikmin_pc pikmin_core)
else()
    target_link_libraries(${PC_APP_NAME} pikmin_core pikmin_pc pikmin_core pthread)
endif()
# The game's replaceable operator new/delete (sysNew.h/.cpp — delete is a
# NO-OP) must not be exported: system frameworks (Metal -> MLAssetIO)
# bind the main executable's replaceable allocation operators and their
# initializers crash. Visibility flags don't cover replaceable operators
# (clang emits them strong/default), so unexport them at link.
# iOS has no link step here (Pikmin is an archive), so the same flags live in
# the Xcode shell's OTHER_LDFLAGS — keep the two lists in sync.
foreach(sym __Znwm __Znam __Znwmi __Znami __ZdlPv __ZdaPv)
    if(NOT PC_IOS)
        target_link_options(${PC_APP_NAME} PRIVATE "-Wl,-unexported_symbol,${sym}")
    endif()
endforeach()

# UBSan's runtime is pulled in by the driver, so the sanitize flag has to be
# on the LINK too, not just the compiles.
if(PC_UBSAN)
    target_link_options(${PC_APP_NAME} PRIVATE -fsanitize=undefined)
endif()
