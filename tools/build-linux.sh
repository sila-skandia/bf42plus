#!/usr/bin/env bash
# Build bf42++.dll on Linux using the real MSVC toolchain running under Wine.
#
# The project is MSVC-only in substance, not just in build files: bf/stl.cpp and
# bf/object.cpp are almost entirely __declspec(naked) functions with MS inline
# asm, and dllmain.cpp uses __try/__except. So this does not port the build to
# another compiler -- it runs cl.exe and link.exe themselves, via the wrappers
# from https://github.com/mstorsjo/msvc-wine.
#
# CMake is not used here either. Pointing CMake at msvc-wine means a toolchain
# file and a lot of probing through Wine for no benefit: the source list is a
# plain literal in CMakeLists.txt, so this script reads it from there instead and
# the two cannot drift apart. The authoritative build is CMake on Windows, in
# .github/workflows/windows-build-msvc.yml; this is the fast local check.
#
# One-time setup:
#   sudo pacman -S --needed msitools
#   git clone https://github.com/mstorsjo/msvc-wine ~/tools/msvc-wine
#   cd ~/tools/msvc-wine
#   ./vsdownload.py --accept-license --major 17 --host-arch x64 \
#       --architecture x86 --dest ~/tools/msvc
#   ./install.sh ~/tools/msvc
#
# Usage:
#   tools/build-linux.sh              # build to build/Release-Win32/bf42++.dll
#   tools/build-linux.sh --install    # also copy into the game dir as dsound.dll
#
# Environment:
#   MSVC_ROOT   toolchain prefix (default ~/tools/msvc)
#   WINEPREFIX  wine prefix for the compiler (default ~/.wine-msvc)
#   BF1942_DIR  game directory for --install
#   JOBS        parallel cl processes (default: nproc)
#   VERBOSE     set to list the distinct warnings instead of just counting them

set -euo pipefail

cd "$(dirname "$0")/.."

# Deliberately not the default prefix: the game lives in ~/.wine with a dsound
# dll override, and running a different wine version against a prefix triggers a
# prefix update. The compiler gets its own throwaway prefix.
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-msvc}"
export WINEDEBUG="${WINEDEBUG:--all}"

MSVC_ROOT="${MSVC_ROOT:-$HOME/tools/msvc}"
BIN="$MSVC_ROOT/bin/x86"
OUT="build/Release-Win32"
OBJ="$OUT/obj"
JOBS="${JOBS:-$(nproc)}"
INSTALL=0
[[ "${1:-}" == "--install" ]] && INSTALL=1

for tool in cl rc link; do
    [[ -x "$BIN/$tool" ]] || { echo "error: $BIN/$tool not found -- see setup in the header of this script" >&2; exit 1; }
done

# --- sources -------------------------------------------------------------
# Read from CMakeLists.txt so the two build systems cannot drift apart. Only the
# unconditional target_sources() block is taken: the rest is the Tracy profiler,
# which is BUILD_FOR_BF1942_R-only. res/*.def and res/*.rc are listed there too
# and are handled separately below, not passed to cl.
mapfile -t SOURCES < <(
    awk '/target_sources\(.*PRIVATE/ { inblock = 1; next }
         inblock && /^\)/          { exit }
         inblock' CMakeLists.txt \
    | grep -oE '\S+\.(cpp|c)\b'
)
(( ${#SOURCES[@]} )) || { echo "error: no sources found in CMakeLists.txt" >&2; exit 1; }

mkdir -p "$OUT"
for src in "${SOURCES[@]}"; do mkdir -p "$OBJ/$(dirname "$src")"; done

# --- flags ---------------------------------------------------------------
# Mirrors the Release|Win32 CMake configuration, with one deliberate difference:
#   /Z7 instead of /Zi   -- debug info in the .obj, so parallel cl processes do
#                           not need mspdbsrv.exe, which is unreliable on Wine.
#                           link /DEBUG still produces a normal PDB.
# /arch:IA32 is not optional: it keeps the compiler on x87, which the game's
# inline asm and the FPU-precision patches in bfhook.cpp depend on.
# _UNICODE/UNICODE are set by target_compile_definitions. Without them the
# W-suffixed Win32 APIs the code calls resolve to their A variants and nothing
# type-checks. REDUCE_AV_SCORE matches the CMake option's default of ON.
# Joined forms (/DFOO, not /D FOO): these go into a response file, where every
# whitespace run separates arguments, so a detached option value is lost.
DEFINES=(/DWIN32 /DNDEBUG /D_WINDOWS /D_USRDLL /D_UNICODE /DUNICODE /DREDUCE_AV_SCORE)
INCLUDES=(/Ilibs/fpng /Ilibs/stb /Ilibs/simpleini /Ilibs/tracy/tracy)
COMMON=(/nologo /c /W4 /O2 /Oi /Gy /GF /permissive- /MD /Z7
        /Zc:threadSafeInit /arch:IA32
        /wd4584   # multiple inheritance warnings
        /wd5105 /wd4201
        "${DEFINES[@]}" "${INCLUDES[@]}")
CXXFLAGS=("${COMMON[@]}" /std:c++20 /EHsc)
CFLAGS=("${COMMON[@]}" /std:c17)

# Flags go through response files so each parallel worker is a one-liner and
# there is an artifact to inspect when cl disagrees with what we think we passed.
printf '%s\n' "${CXXFLAGS[@]}" > "$OUT/cl-cxx.rsp"
printf '%s\n' "${CFLAGS[@]}"   > "$OUT/cl-c.rsp"

echo "==> compiling ${#SOURCES[@]} sources with $JOBS jobs"
export BIN OBJ OUT
printf '%s\n' "${SOURCES[@]}" | xargs -P "$JOBS" -I{} bash -c '
    src="$1"; obj="$OBJ/${src%.*}.obj"; log="$obj.log"
    rsp="$OUT/cl-cxx.rsp"; [[ "$src" == *.c ]] && rsp="$OUT/cl-c.rsp"
    if "$BIN/cl" @"$rsp" /Fo"$obj" "$src" > "$log" 2>&1; then
        echo "    $src"
    else
        echo "--- FAILED: $src"
        cat "$log"
        exit 1
    fi
' _ {}

# Every TU pulls in the same headers, so the project's existing /W4 warnings would
# otherwise be repeated once per source. Collapse to one line each, normalising the
# mix of absolute Wine (Z:\...) and relative paths cl reports.
mapfile -t WARNINGS < <(find "$OBJ" -name '*.obj.log' -exec cat {} + \
    | grep -E 'warning [A-Z][0-9]+' \
    | sed -E 's#\\#/#g; s#^.*/(src|libs)/#\1/#; s/\(([0-9]+)\) ?:/(\1):/' \
    | sort -u)
if (( ${#WARNINGS[@]} )); then
    if [[ -n "${VERBOSE:-}" ]]; then
        echo "==> ${#WARNINGS[@]} distinct warnings"
        printf '    %s\n' "${WARNINGS[@]}"
    else
        echo "==> ${#WARNINGS[@]} distinct warnings (VERBOSE=1 to list)"
    fi
fi

echo "==> resources"
"$BIN/rc" /nologo /fo "$OUT/bf42plus.res" /I res res/bf42plus.rc > /dev/null

# --- link ----------------------------------------------------------------
# advapi32 is not in the CMake target_link_libraries list because CMake links the
# default Windows library set on top of it; here it has to be named, or
# getStringFromRegistry_hook's RegCreateKeyW/RegSetValueA are unresolved.
echo "==> linking"
mapfile -t OBJS < <(find "$OBJ" -name '*.obj' | sort)
"$BIN/link" /nologo /DLL /MACHINE:X86 \
    /SUBSYSTEM:WINDOWS,5.02 /LARGEADDRESSAWARE /DEBUG /OPT:REF /OPT:ICF \
    /DEF:res/bf42plus.def \
    /OUT:"$OUT/bf42++.dll" /IMPLIB:"$OUT/bf42++.lib" /PDB:"$OUT/bf42++.pdb" \
    /PDBALTPATH:bf42++.pdb \
    "${OBJS[@]}" "$OUT/bf42plus.res" \
    kernel32.lib user32.lib gdi32.lib comctl32.lib advapi32.lib \
    shell32.lib shlwapi.lib winmm.lib

ls -la "$OUT/bf42++.dll"

if (( INSTALL )); then
    [[ -n "${BF1942_DIR:-}" ]] || { echo "error: --install needs BF1942_DIR" >&2; exit 1; }
    [[ -d "$BF1942_DIR" ]] || { echo "error: BF1942_DIR does not exist: $BF1942_DIR" >&2; exit 1; }
    # Installed as dsound.dll, not bf42++.dll. The DLL exports DirectSoundCreate8
    # (res/bf42plus.def) and forwards to the real library, so under that name the
    # game loads it by itself; bf42++.dll is the same binary injected by
    # bf42++.exe instead. Upstream's releases ship one build under both names.
    #
    # The rolling dsound_old.dll below would, on a second install, overwrite the
    # only copy of the release DLL the first install displaced. So the first DLL
    # seen is also kept as dsound_release.dll, which nothing overwrites.
    for f in dsound.dll dsound_old.dll; do
        if [[ -f "$BF1942_DIR/$f" && ! -f "$BF1942_DIR/dsound_release.dll" ]]; then
            cp -p "$BF1942_DIR/$f" "$BF1942_DIR/dsound_release.dll"
            echo "==> kept existing DLL as dsound_release.dll ($f)"
        fi
    done
    # Keep one generation back, the way tools/copyoutput.bat does.
    [[ -f "$BF1942_DIR/dsound.dll" ]] && mv -f "$BF1942_DIR/dsound.dll" "$BF1942_DIR/dsound_old.dll"
    cp -f "$OUT/bf42++.dll" "$BF1942_DIR/dsound.dll"
    cp -f "$OUT/bf42++.pdb" "$BF1942_DIR/dsound.pdb"
    echo "==> installed to $BF1942_DIR as dsound.dll"
fi
