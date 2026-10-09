#!/usr/bin/env bash
# Builds AstroQuest for the Steam Frame: the emulator as a native Linux ARM64 program that shows
# the game through the headset's own OpenXR runtime (SteamVR on SteamOS). No Android, no Lepton.
#
#   steam-frame/build.sh                 everything: sources, FEXCore, the emulator, the package
#   steam-frame/build.sh fetch           only fetch the sources (submodules, FEX) at their pins
#   steam-frame/build.sh fex             ... up to FEXCore (the x86-64 CPU backend)
#   steam-frame/build.sh core            ... up to the emulator
#   steam-frame/build.sh package         only (re)make the package from what was built
#
# Where it builds:
#   - natively on an aarch64 Linux machine, best inside the Steam Linux Runtime 3.0 (sniper)
#     SDK for arm64, so that the program runs in the runtime the headset starts it in (the
#     Devkit Client's "Steam Linux Runtime 3.0 ARM64 (Sniper)"); see README-STEAM-FRAME.md;
#   - on an x86-64 Linux machine with STEAM_FRAME_CROSS=1 (clang, lld and a GCC aarch64 cross
#     sysroot, or STEAM_FRAME_SYSROOT=<dir>): see toolchain-aarch64-cross.cmake.
#
# Settings (environment):
#   STEAM_FRAME_CROSS=1       cross-compile (default: native when the machine is aarch64)
#   STEAM_FRAME_SYSROOT=dir   aarch64 sysroot for cross-compiling
#   STEAM_FRAME_CLANG=clang-19  the clang to use (default: the newest clang-NN there is)
#   STEAM_FRAME_BUILD=dir     build folder (default: build/steam-frame)
#   STEAM_FRAME_JOBS=n        parallel jobs (default: number of processors)
#   STEAM_FRAME_SDL_X11=0     build SDL without X11/Wayland (the headset needs neither: the
#                             game is shown in the headset, and a window is only for testing)
#   STEAM_FRAME_CMAKE_ARGS="..."  more arguments for configuring the emulator
#                             (-DLIBUSB_ENABLE_UDEV=OFF where libudev is missing, say)
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/.." && pwd)
core_src="$root/shadps4-arm64-main"
build=${STEAM_FRAME_BUILD:-$root/build/steam-frame}
jobs=${STEAM_FRAME_JOBS:-$(nproc)}
step=${1:-all}

say() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }

machine=$(uname -m)
cross=${STEAM_FRAME_CROSS:-}
if [[ -z "$cross" ]]; then
    [[ "$machine" == aarch64 || "$machine" == arm64 ]] && cross=0 || cross=1
fi

pick_clang() {
    if [[ -n "${STEAM_FRAME_CLANG:-}" ]]; then echo "$STEAM_FRAME_CLANG"; return; fi
    local best="" v
    for v in 22 21 20 19 18 17; do
        if command -v "clang-$v" >/dev/null && command -v "clang++-$v" >/dev/null; then
            best="clang-$v"; break
        fi
    done
    [[ -z "$best" ]] && command -v clang >/dev/null && best=clang
    [[ -n "$best" ]] || die "clang is needed (17 or later)"
    echo "$best"
}
clang_c=$(pick_clang)
clang_cxx=${clang_c/clang/clang++}
export STEAM_FRAME_CLANG=$clang_c

need() { command -v "$1" >/dev/null || die "$1 is needed"; }
for tool in git cmake ninja python3; do need "$tool"; done

# --- sources ------------------------------------------------------------------------------------

# A git checkout of <url> at <revision> in <dir>, shallow, whatever was there before.
checkout_at() {
    local dir=$1 url=$2 revision=$3
    if [[ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" == "$revision" ]] &&
       [[ -n "$(ls -A "$dir" | grep -vx .git)" ]]; then
        return 0
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$revision"
    git -C "$dir" checkout -q FETCH_HEAD
}

# Some submodules come from hosts a build machine may not reach; GitHub mirrors have the same
# commits.
mirror_of() {
    case "$1" in
        https://android.googlesource.com/platform/external/aac*) echo https://github.com/mstorsjo/fdk-aac.git ;;
        *) echo "$1" ;;
    esac
}

# Every submodule of <repo> under <prefix> at the commit the repository pins.
fetch_gitlinks() {
    local repo=$1 prefix=$2 modules=$3 key_prefix=${4:-}
    local mode type sha path url
    while read -r mode type sha path; do
        [[ "$type" == commit ]] || continue
        url=$(git config -f "$modules" --get "submodule.$key_prefix$path.url" || true)
        if [[ -z "$url" ]]; then
            # (Some list their submodules under another name than the path.)
            local name
            name=$(git config -f "$modules" --get-regexp '^submodule\..*\.path$' |
                   awk -v p="$key_prefix$path" '$2==p {print $1}' | head -1)
            [[ -n "$name" ]] && url=$(git config -f "$modules" --get "${name%.path}.url" || true)
        fi
        [[ -n "$url" ]] || { echo "  (no URL for $path, skipped)"; continue; }
        if checkout_at "$repo/$path" "$url" "$sha" 2>/dev/null ||
           checkout_at "$repo/$path" "$(mirror_of "$url")" "$sha"; then
            echo "  $path"
        else
            die "could not fetch $path ($url at $sha)"
        fi
    done < <(git -C "$repo" ls-tree -r HEAD "$prefix")
}

fetch_sources() {
    say "Fetching the emulator's libraries at their pinned commits"
    # (The repository's own .gitmodules names them relative to the top.)
    fetch_gitlinks "$root" shadps4-arm64-main/externals "$root/.gitmodules"
    local ext="$core_src/externals"
    fetch_gitlinks "$ext/sirit" externals "$ext/sirit/.gitmodules"
    fetch_gitlinks "$ext/zydis" dependencies "$ext/zydis/.gitmodules"

    say "Fetching FEX (x86-64 on ARM64) at the pinned commit"
    local lock="$core_src/runtime/locks/components.lock.json"
    local fex_url fex_rev
    read -r fex_url fex_rev < <(python3 -c '
import json, sys
lock = json.load(open(sys.argv[1]))
fex = next(c for c in lock["components"] if c["name"] == "fex")
print(fex["url"], fex["revision"])' "$lock")
    local fex="$core_src/runtime/sources/fex"
    local patch="$core_src/runtime/patches/fex-fexcore-only.patch"
    if [[ "$(git -C "$fex" rev-parse HEAD 2>/dev/null)" != "$fex_rev" ]]; then
        checkout_at "$fex" "$fex_url" "$fex_rev"
    fi
    local sub sha url
    for sub in External/unordered_dense External/rpmalloc External/xxhash External/fmt \
               External/range-v3 Source/Common/cpp-optparse; do
        sha=$(git -C "$fex" ls-tree HEAD "$sub" | awk '{print $3}')
        url=$(git config -f "$fex/.gitmodules" --get-regexp '^submodule\..*\.path$' |
              awk -v p="$sub" '$2==p {print $1}' | head -1)
        url=$(git config -f "$fex/.gitmodules" --get "${url%.path}.url")
        checkout_at "$fex/$sub" "$url" "$sha"
        echo "  fex/$sub"
    done
    # FEX trimmed to its core (the patch the Quest build uses as well), applied once.
    if git -C "$fex" apply --reverse --check "$patch" 2>/dev/null; then
        echo "  (FEXCore-only patch already applied)"
    else
        git -C "$fex" apply "$patch"
        echo "  FEXCore-only patch applied"
    fi
}

# --- FEXCore ------------------------------------------------------------------------------------

cross_args=()
if [[ "$cross" == 1 ]]; then
    cross_args=(-DCMAKE_TOOLCHAIN_FILE="$here/toolchain-aarch64-cross.cmake")
    [[ -n "${STEAM_FRAME_SYSROOT:-}" ]] && cross_args+=(-DSTEAM_FRAME_SYSROOT="$STEAM_FRAME_SYSROOT")
else
    cross_args=(-DCMAKE_C_COMPILER="$clang_c" -DCMAKE_CXX_COMPILER="$clang_cxx"
                -DCMAKE_LINKER_TYPE=LLD)
fi

build_fex() {
    say "Building FEXCore for aarch64 ($([[ $cross == 1 ]] && echo cross || echo native), $clang_c)"
    local fex="$core_src/runtime/sources/fex"
    local out="$build/fexcore"
    [[ -d "$fex/FEXCore" ]] || die "FEX is not there: run '$0 fetch' first"
    cmake -S "$fex" -B "$out" -G Ninja "${cross_args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DTUNE_CPU=none \
        -DBUILD_FEXCORE_ONLY=ON \
        -DFEXCORE_PROJECT_SOURCE_DIR="$core_src/src" \
        -DFEXCORE_SMOKE_SOURCE="$core_src/runtime/probes/fexcore-smoke.cpp" \
        -DFEXCORE_GUEST_HARNESS_SOURCES="$core_src/src/core/fex/fex_guest_engine.cpp;$core_src/src/core/guest_cpu/fex_guest_cpu.cpp;$core_src/src/core/guest_cpu/hle_call_adapter.cpp;$core_src/src/core/guest_cpu/fex_hle_bridge.cpp;$core_src/runtime/probes/fexcore-guest-harness.cpp" \
        -DBUILD_TESTING=OFF -DBUILD_FEX_LINUX_TESTS=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF \
        -DENABLE_GDB_SYMBOLS=OFF -DENABLE_LTO=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
        -DENABLE_OFFLINE_TELEMETRY=OFF -DENABLE_VIXL_DISASSEMBLER=OFF -DENABLE_VIXL_SIMULATOR=OFF \
        -DENABLE_ZYDIS=OFF -DENABLE_FEXCORE_PROFILER=OFF
    # (The smoke probe links every library the emulator takes from FEX.)
    cmake --build "$out" --target fexcore-smoke --parallel "$jobs"
}

# --- the emulator -------------------------------------------------------------------------------

build_core() {
    say "Building the emulator for the Steam Frame"
    local out="$build/core"
    local args=("${cross_args[@]}")
    if [[ "$cross" == 1 ]]; then
        # One program of the build runs on the build machine: the one that packs the fonts.
        local embed="$build/host-tools/Dear_ImGui_FontEmbed"
        if [[ ! -x "$embed" ]]; then
            mkdir -p "$build/host-tools"
            "${CXX_FOR_BUILD:-c++}" -O2 -o "$embed" \
                "$core_src/externals/dear_imgui/misc/fonts/binary_to_compressed_c.cpp"
        fi
        args+=(-DIMGUI_FONT_EMBED_EXECUTABLE="$embed")
    fi
    # (spdlog is configured before fmt and would download fmt's release tarball at configure
    # time; FETCHCONTENT_SOURCE_DIR_FMT below hands it the pinned copy in externals instead.)
    if [[ "${STEAM_FRAME_SDL_X11:-1}" == 0 ]]; then
        args+=(-DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON)
    fi
    if [[ -n "${STEAM_FRAME_CMAKE_ARGS:-}" ]]; then
        local extra
        read -r -a extra <<< "$STEAM_FRAME_CMAKE_ARGS"
        args+=("${extra[@]}")
    fi
    cmake -S "$core_src" -B "$out" -G Ninja "${args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DENABLE_STEAM_FRAME=ON \
        -DENABLE_OPENXR=ON \
        -DENABLE_FEX_GUEST_CPU=ON \
        -DFEXCORE_GUEST_CPU_SOURCE_DIR="$core_src/runtime/sources/fex" \
        -DFEXCORE_GUEST_CPU_BUILD_DIR="$build/fexcore" \
        -DENABLE_BACHATA_RUNTIME=OFF \
        -DENABLE_USERFAULTFD=OFF -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF \
        -DENABLE_TESTS=OFF \
        -DFETCHCONTENT_SOURCE_DIR_FMT="$core_src/externals/fmt" \
        -DSDL_X11_XCURSOR=OFF -DSDL_X11_XDBE=OFF -DSDL_X11_XINPUT=OFF -DSDL_X11_XFIXES=OFF \
        -DSDL_X11_XRANDR=OFF -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XSHAPE=OFF -DSDL_X11_XSYNC=OFF \
        -DSDL_X11_XTEST=OFF
    cmake --build "$out" --target shadps4 --parallel "$jobs"
}

# --- the package --------------------------------------------------------------------------------

package() {
    say "Making the package"
    local bin="$build/core/shadps4"
    [[ -x "$bin" ]] || die "the emulator has not been built: run '$0 core' first"
    local dist="$root/build/AstroQuest-SteamFrame"
    rm -rf "$dist"
    mkdir -p "$dist/bin" "$dist/games"
    cp "$bin" "$dist/bin/shadps4"
    local strip_tool=llvm-strip
    command -v "llvm-strip-${clang_c#clang-}" >/dev/null && strip_tool="llvm-strip-${clang_c#clang-}"
    command -v "$strip_tool" >/dev/null && "$strip_tool" --strip-debug "$dist/bin/shadps4" || true
    cp "$here/astroquest.sh" "$dist/astroquest.sh"
    cp "$here/settings.txt" "$dist/settings.txt"
    cp "$root/README-STEAM-FRAME.md" "$dist/README-STEAM-FRAME.md"
    cp "$root/LICENSE" "$dist/LICENSE"
    cp "$root/THIRD-PARTY-NOTICES.md" "$dist/THIRD-PARTY-NOTICES.md"
    chmod +x "$dist/astroquest.sh" "$dist/bin/shadps4"
    cat > "$dist/games/PUT-YOUR-GAME-HERE.txt" <<'EOF'
Put your own dumped copy of ASTRO BOT Rescue Mission here: the folder CUSA12392 (the one with
eboot.bin, sce_sys and sce_module in it), and an update, if you have one, as CUSA12392-UPDATE.
No game files come with AstroQuest.
EOF
    # What the program needs from the system, for whoever checks it against the runtime.
    if command -v llvm-readelf >/dev/null || command -v "llvm-readelf-${clang_c#clang-}" >/dev/null; then
        local readelf=llvm-readelf
        command -v "llvm-readelf-${clang_c#clang-}" >/dev/null && readelf="llvm-readelf-${clang_c#clang-}"
        "$readelf" -d "$dist/bin/shadps4" | awk '/NEEDED/ {print $NF}' | tr -d '[]' \
            > "$dist/bin/needed-libraries.txt" || true
        "$readelf" -V "$dist/bin/shadps4" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 \
            > "$dist/bin/newest-glibc-symbol.txt" || true
    fi
    (cd "$root/build" && tar -czf AstroQuest-SteamFrame.tar.gz AstroQuest-SteamFrame)
    say "Done: $dist (and build/AstroQuest-SteamFrame.tar.gz)"
    echo "Upload the folder with the SteamOS Devkit Client: Start Command 'astroquest.sh',"
    echo "Runtime 'Steam Linux Runtime 3.0 ARM64 (Sniper)'. See README-STEAM-FRAME.md."
}

case "$step" in
    fetch) fetch_sources ;;
    fex) fetch_sources; build_fex ;;
    core) [[ -f "$build/fexcore/FEXCore/Source/libFEXCore.a" ]] || { fetch_sources; build_fex; }
          build_core ;;
    package) package ;;
    all) fetch_sources; build_fex; build_core; package ;;
    *) die "unknown step '$step' (fetch, fex, core, package or nothing for all)" ;;
esac
