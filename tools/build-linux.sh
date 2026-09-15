#!/usr/bin/env bash
# Build dsound.dll on Linux using the real MSVC toolchain running under Wine.
#
# The project is MSVC-only in substance, not just in build files: bf/stl.cpp and
# bf/object.cpp are almost entirely __declspec(naked) functions with MS inline
# asm, and dllmain.cpp uses __try/__except. So this does not port the build to
# another compiler -- it runs cl.exe and link.exe themselves, via the wrappers
# from https://github.com/mstorsjo/msvc-wine.
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
#   tools/build-linux.sh              # build to build/Release-Win32/dsound.dll
#   BF1942_DIR=... tools/build-linux.sh --install
#
# Environment:
#   MSVC_ROOT   toolchain prefix (default ~/tools/msvc)
#   WINEPREFIX  wine prefix for the compiler (default ~/.wine-msvc)
#   BF1942_DIR  game directory for --install
#   JOBS        parallel cl processes (default: nproc)

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

mkdir -p "$OBJ/src/bf"

# --- version -------------------------------------------------------------
# The vcxproj generates this from `git describe --tags --long`. This fork has no
# tags, and a local build must never lose to the update server -- the updater
# would replace the DLL we are trying to test. 99.x always compares as newer.
VERSION="${BF42PLUS_VERSION:-$(git describe --tags --long 2>/dev/null || echo "99.0.0-0-g$(git rev-parse --short HEAD 2>/dev/null || echo unknown)")}"
NEW_GITVERSION="#define GIT_VERSION $VERSION"
if [[ ! -f src/gitversion.h ]] || [[ "$(cat src/gitversion.h)" != "$NEW_GITVERSION" ]]; then
    echo "$NEW_GITVERSION" > src/gitversion.h
fi
echo "==> version $VERSION"

# --- flags ---------------------------------------------------------------
# Mirrors the Release|Win32 configuration in bf42plus.vcxproj, with two
# deliberate differences:
#   /Z7 instead of /Zi   -- debug info in the .obj, so parallel cl processes do
#                           not need mspdbsrv.exe, which is unreliable on Wine.
#                           link /DEBUG still produces a normal PDB.
#   no /GL               -- LTCG buys nothing here and is one more moving part.
# Joined forms (/DFOO, not /D FOO): these go into a response file, where every
# whitespace run separates arguments, so a detached option value is lost.
# _UNICODE/UNICODE come from <CharacterSet>Unicode</CharacterSet>, not from the
# explicit PreprocessorDefinitions list. Without them the W-suffixed Win32 APIs
# the code calls resolve to their A variants and nothing type-checks.
DEFINES=(/DWIN32 /DNDEBUG /DBF42PLUS_EXPORTS /D_WINDOWS /D_USRDLL /D_UNICODE /DUNICODE)
INCLUDES=(/Ilibs/fpng /Ilibs/stb /Ilibs/simpleini /Ilibs/libsodium/include)
COMMON=(/nologo /c /W4 /O2 /Oi /Gy /GF /sdl /permissive- /MD /Z7
        /Zc:threadSafeInit- /wd5105 /wd4201 "${DEFINES[@]}" "${INCLUDES[@]}")
CXXFLAGS=("${COMMON[@]}" /std:c++20 /EHsc)
CFLAGS=("${COMMON[@]}" /std:c17)

# --- sources -------------------------------------------------------------
# Read from the vcxproj so the two build systems cannot drift apart. pch.cpp is
# skipped: it exists only to create the precompiled header, and every
# translation unit includes pch.h directly, so without /Yc it is an empty
# object file that still costs a full parse of the header set.
mapfile -t SOURCES < <(grep -oP '<ClCompile Include="\K[^"]+' bf42plus.vcxproj \
    | tr '\\' '/' | grep -v '^src/pch\.cpp$')

# Flags go through response files so each parallel worker is a one-liner and
# there is a artifact to inspect when cl disagrees with what we think we passed.
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

# Every TU pulls in the same headers, so the project'"'"'s existing /W4 warnings would
# otherwise be repeated 17 times. Collapse to one line each, normalising the mix
# of absolute Wine (Z:\...) and relative paths cl reports.
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
"$BIN/rc" /nologo /fo "$OUT/bf42plus.res" res/bf42plus.rc

echo "==> linking"
mapfile -t OBJS < <(find "$OBJ" -name '*.obj' | sort)
"$BIN/link" /nologo /DLL /MACHINE:X86 \
    /SUBSYSTEM:WINDOWS,5.02 /LARGEADDRESSAWARE /DEBUG /OPT:REF /OPT:ICF \
    /DEF:res/bf42plus.def \
    /OUT:"$OUT/dsound.dll" /IMPLIB:"$OUT/dsound.lib" /PDB:"$OUT/dsound.pdb" \
    /PDBALTPATH:dsound.pdb \
    "${OBJS[@]}" "$OUT/bf42plus.res" \
    libs/libsodium/libsodium.lib \
    kernel32.lib user32.lib gdi32.lib comctl32.lib advapi32.lib \
    winhttp.lib shell32.lib shlwapi.lib

ls -la "$OUT/dsound.dll"

if (( INSTALL )); then
    [[ -n "${BF1942_DIR:-}" ]] || { echo "error: --install needs BF1942_DIR" >&2; exit 1; }
    [[ -d "$BF1942_DIR" ]] || { echo "error: BF1942_DIR does not exist: $BF1942_DIR" >&2; exit 1; }
    # Keep one generation back, the way tools/copyoutput.bat does.
    [[ -f "$BF1942_DIR/dsound.dll" ]] && mv -f "$BF1942_DIR/dsound.dll" "$BF1942_DIR/dsound_old.dll"
    cp -f "$OUT/dsound.dll" "$OUT/dsound.pdb" "$BF1942_DIR/"
    echo "==> installed to $BF1942_DIR"
fi
