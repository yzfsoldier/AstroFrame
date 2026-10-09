#!/usr/bin/env bash
# Starts ASTRO BOT Rescue Mission on the Steam Frame: the emulator as a native Linux program,
# showing the game through the headset's OpenXR runtime. The Start Command for the SteamOS
# Devkit Client (Runtime: Steam Linux Runtime 3.0 ARM64 (Sniper)), or run it from a shell.
#   ./astroquest.sh                 play
#   ./astroquest.sh --check         only say what would be played, and how
# Settings: settings.txt next to this script, or ASTROQUEST_<KEY>=value in the environment.
set -uo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
check_only=0
[[ "${1:-}" == "--check" ]] && check_only=1

say() { printf '[AstroQuest] %s\n' "$*"; }

# --- settings -----------------------------------------------------------------------------------

declare -A settings=()
extra_env=()
if [[ -f "$here/settings.txt" ]]; then
    while IFS= read -r line || [[ -n "$line" ]]; do
        line=${line%$'\r'}
        line=${line#$'\xef\xbb\xbf'}
        [[ -z "$line" || "$line" == \#* || "$line" != *=* ]] && continue
        key=${line%%=*}
        value=${line#*=}
        key=${key//[[:space:]]/}
        if [[ "$key" == env ]]; then
            extra_env+=("$value")
        else
            settings[$key]=$value
        fi
    done < "$here/settings.txt"
fi

# setting <key> [default]: ASTROQUEST_<KEY> first, then settings.txt, then the default.
setting() {
    local key=$1 default=${2:-} var
    var="ASTROQUEST_${key^^}"
    if [[ -n "${!var:-}" ]]; then
        printf '%s' "${!var}"
    elif [[ -n "${settings[$key]:-}" ]]; then
        printf '%s' "${settings[$key]}"
    else
        printf '%s' "$default"
    fi
}

# --- where things are ---------------------------------------------------------------------------

data=$(setting data "${XDG_DATA_HOME:-$HOME/.local/share}/AstroQuest")
mkdir -p "$data/user/log" "$data/games" || { say "cannot create $data"; exit 1; }

find_game() {
    local wanted dir
    wanted=$(setting game)
    if [[ -n "$wanted" ]]; then
        [[ -d "$wanted" ]] && wanted="$wanted/eboot.bin"
        [[ -f "$wanted" ]] && { printf '%s' "$wanted"; return; }
        say "game=$wanted is not there; looking in the games folders"
    fi
    for dir in "$here/games" "$data/games" "$HOME/AstroQuest/games"; do
        [[ -f "$dir/CUSA12392/eboot.bin" ]] && { printf '%s' "$dir/CUSA12392/eboot.bin"; return; }
    done
    # Any folder holding the game, whatever it is called.
    local sfo
    for dir in "$here/games" "$data/games" "$HOME/AstroQuest/games"; do
        for sfo in "$dir"/*/sce_sys/param.sfo; do
            [[ -f "$sfo" ]] || continue
            if grep -aq CUSA12392 "$sfo" && [[ -f "$(dirname "$(dirname "$sfo")")/eboot.bin" ]]; then
                printf '%s' "$(dirname "$(dirname "$sfo")")/eboot.bin"
                return
            fi
        done
    done
}

game=$(find_game)
if [[ -z "$game" ]]; then
    say "No game found. Put the folder CUSA12392 (the one with eboot.bin in it) into one of"
    say "  $here/games"
    say "  $data/games"
    say "  $HOME/AstroQuest/games"
    say "or name it with game= in settings.txt."
    exit 1
fi
game_dir=$(dirname "$game")
if [[ ! -f "$game_dir/sce_sys/param.sfo" ]]; then
    say "$game_dir has no sce_sys/param.sfo: without it the emulator does not recognise the game."
    say "(A game unpacked from a .pkg needs it extracted separately; see README.md.)"
    exit 1
fi
if [[ ! -f "$game_dir/sce_module/libc.prx" ]]; then
    say "$game_dir is not a complete copy of the game (sce_module/libc.prx is missing)."
    say "An update alone is not the game: put it beside the game as CUSA12392-UPDATE."
    exit 1
fi

emulator="$here/bin/shadps4"
[[ -x "$emulator" ]] || { say "$emulator is missing: build it with steam-frame/build.sh"; exit 1; }

# The emulator's own settings: a log in a file, and controllers read while its window (if it has
# one) is not in front. Only what is set here; it fills in the rest itself.
config="$data/user/config.json"
if [[ ! -f "$config" ]]; then
    cat > "$config" <<'EOF'
{
  "Log": { "type": "file", "sync": false, "filter": "*:Info", "append": false,
           "size_limit": 52428800 },
  "GPU": { "present_mode": "Mailbox", "full_screen": false },
  "Input": { "motion_controls_enabled": true, "background_controller_input": true },
  "Vulkan": { "vkvalidation_enabled": false }
}
EOF
fi

# --- what the settings mean to the emulator -----------------------------------------------------

export SHADPS4_VR_FOV_OF=headset
export SHADPS4_VR_FOV_SYMMETRIC=1
export SHADPS4_VR_FOV=$(setting fov 85)

refresh=$(setting refresh_rate 90)
[[ "$refresh" != 0 ]] && export SHADPS4_XR_REFRESH_RATE=$refresh
pace=$(setting pace)
[[ -n "$pace" ]] && export SHADPS4_VR_PACE=$pace
fps=$(setting fps)
[[ -n "$fps" ]] && export SHADPS4_VR_FPS_CAP=$fps

resolution=$(setting resolution)
case "$resolution" in
    "") ;;
    game) export SHADPS4_TITLE_RESOLUTION=title ;;
    816) export SHADPS4_TITLE_RESOLUTION=3 ;;
    960) export SHADPS4_TITLE_RESOLUTION=4 ;;
    1200) export SHADPS4_TITLE_RESOLUTION=5 ;;
    1440) export SHADPS4_TITLE_RESOLUTION=6 ;;
    *[!0-9]*) say "resolution=$resolution is not one of 816, 960, 1200, 1440, a width or game" ;;
    *) width=$(( (resolution + 4) / 8 * 8 ))
       (( width > 4320 )) && width=4320
       (( width > 1440 )) && export SHADPS4_TITLE_EYE_WIDTH=$width ;;
esac

sharpen=$(setting sharpen 0.6)
[[ "$sharpen" != 0 ]] && export SHADPS4_VR_SHARPEN=$sharpen
foveation=$(setting foveation 0)
[[ "$foveation" != 0 ]] && export SHADPS4_FOVEATION=$foveation
# One sample a pixel by default (the emulator smooths edges itself), as on the Quest: the fastest.
msaa=$(setting msaa 1)
export SHADPS4_MAX_MSAA=$msaa
[[ "$(setting antialias 1)" == 0 ]] && export SHADPS4_RESOLVE_AA=0
if [[ "$msaa" == 1 && "$(setting tu_sysmem 1)" != 0 ]]; then
    export TU_DEBUG=${TU_DEBUG:+$TU_DEBUG,}sysmem
fi
[[ "$(setting real_time 1)" == 0 ]] && export SHADPS4_TITLE_TIMESTEP=0
lds_barriers=$(setting lds_barriers)
[[ -n "$lds_barriers" ]] && export SHADPS4_LDS_BARRIERS=$lds_barriers
[[ "$(setting hands 1)" == 0 ]] && export SHADPS4_XR_HANDS=0
[[ "$(setting controllers 1)" == 0 ]] && export SHADPS4_XR_CONTROLLERS=0
[[ "$(setting controller_hand right)" == left ]] && export SHADPS4_XR_PAD_HAND=left
turn=$(setting turn)
[[ -n "$turn" ]] && export SHADPS4_VR_TURN=$turn
predict=$(setting predict_ms)
[[ -n "$predict" ]] && export SHADPS4_XR_PREDICT_MS=$predict
[[ "$(setting stick_touchpad 1)" == 0 ]] && export SHADPS4_STICK_TOUCHPAD=0
gain=$(setting mic_gain)
[[ -n "$gain" ]] && export SHADPS4_MIC_GAIN=$gain
[[ "$(setting surround 1)" == 0 ]] && export SHADPS4_VIRTUAL_SURROUND=0
[[ "$(setting pause 1)" == 0 ]] && export SHADPS4_XR_PAUSE=0
export SHADPS4_XR_WAIT=$(setting wait 60)

# The game speaks the system's language, if it has it: LANG=de_DE.UTF-8 is de-DE.
language=$(setting language)
if [[ -z "$language" ]]; then
    language=${LC_ALL:-${LC_MESSAGES:-${LANG:-en_US}}}
    language=${language%%.*}
    language=${language%%@*}
    [[ "$language" == C || "$language" == POSIX ]] && language=en_US
    language=${language/_/-}
fi
export SHADPS4_CONSOLE_LANGUAGE=$language

# Only the headset shows the game; a window is for trying it out on a PC.
if [[ "$(setting window 0)" != 1 ]]; then
    export SHADPS4_HEADLESS=1
fi

# A few lines every few seconds in the log: the frame rate and where a frame's time goes, how
# each sound port keeps up, what the microphone hears.
export SHADPS4_FRAME_STATS=${SHADPS4_FRAME_STATS:-1}
export SHADPS4_AUDIOOUT_STATS=${SHADPS4_AUDIOOUT_STATS:-1}
export SHADPS4_AUDIOIN_STATS=${SHADPS4_AUDIOIN_STATS:-1}
# The emulator has more busy threads than the processor has big cores: the sound and the
# threads that keep time with the display come first (where the system lets a program raise
# its threads; otherwise this changes nothing).
export SHADPS4_THREAD_NICE=${SHADPS4_THREAD_NICE:-"Game:Main=-14,shadPS4:GpuComm=-14,sndx_out_thread=-16,sndx_audio3d=-16,shadPS4:AudioOu=-16,shadPS4:XrHost=-18,shadPS4:Present=-18"}
export MESA_SHADER_CACHE_DIR=${MESA_SHADER_CACHE_DIR:-$data/cache/mesa}
mkdir -p "$MESA_SHADER_CACHE_DIR"

for pair in "${extra_env[@]}"; do
    [[ "$pair" == *=* ]] && export "${pair%%=*}=${pair#*=}"
done

# --- run ----------------------------------------------------------------------------------------

say "Game: $game"
say "Data (saves, logs): $data"
say "Field of view ${SHADPS4_VR_FOV}%, refresh ${SHADPS4_XR_REFRESH_RATE:-as set in SteamVR} Hz," \
    "language $SHADPS4_CONSOLE_LANGUAGE"
if [[ -n "${XR_RUNTIME_JSON:-}" ]]; then
    say "OpenXR runtime: $XR_RUNTIME_JSON"
fi
if [[ "$check_only" == 1 ]]; then
    env | grep -E '^(SHADPS4_|TU_DEBUG|XR_|MESA_)' | sort | sed 's/^/  /'
    exit 0
fi

log_dir="$data/user/log"
for name in shad_log.txt console.txt; do
    [[ -f "$log_dir/$name" ]] && mv -f "$log_dir/$name" "$log_dir/${name%.txt}.prev.txt"
done

# The emulator keeps its user folder in the folder it is started in.
cd "$data" || exit 1
"$emulator" -g "$game" > "$log_dir/console.txt" 2>&1
status=$?
if [[ $status -ne 0 ]]; then
    say "The emulator ended with code $status. Its logs: $log_dir/shad_log.txt, $log_dir/console.txt"
fi
exit $status
